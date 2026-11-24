// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// wavefront.cpp: the wavefront images (wavefront.hpp).

#include "wavefront.hpp"

#include "routed.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>

namespace rv {

std::uint32_t frontier_cells(std::uint32_t n) {
    const double f = std::ceil(static_cast<double>(n) * kFrontierShare);
    return f < 1 ? 1u : static_cast<std::uint32_t>(f);
}

namespace {

std::uint32_t premultiplied(double r, double g, double b, double a) {
    a = std::max(0.0, std::min(1.0, a));
    auto ch = [&](double v) {
        const long q = std::lround(std::max(0.0, std::min(255.0, v)) * a);
        return static_cast<std::uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
    };
    const std::uint8_t px[4] = {ch(r), ch(g), ch(b), static_cast<std::uint8_t>(std::lround(a * 255.0))};
    std::uint32_t v;
    std::memcpy(&v, px, 4);
    return v;
}

void unite(std::array<std::int32_t, 4> &box, std::int32_t i, std::int32_t j) {
    if (box[0] >= box[2] || box[1] >= box[3]) {
        box = {{i, j, i + 1, j + 1}};
        return;
    }
    box[0] = std::min(box[0], i);
    box[1] = std::min(box[1], j);
    box[2] = std::max(box[2], i + 1);
    box[3] = std::max(box[3], j + 1);
}

void unite(std::array<std::int32_t, 4> &box, const std::array<std::int32_t, 4> &b) {
    if (b[0] >= b[2] || b[1] >= b[3]) return;
    if (box[0] >= box[2] || box[1] >= box[3]) {
        box = b;
        return;
    }
    box[0] = std::min(box[0], b[0]);
    box[1] = std::min(box[1], b[1]);
    box[2] = std::max(box[2], b[2]);
    box[3] = std::max(box[3], b[3]);
}

} // namespace

void PixelBuffer::Free::operator()(std::uint8_t *p) const {
    std::free(p);
}

void PixelBuffer::allocate(std::size_t n) {
    if (n <= capacity_) { // zero already (clear)
        size_ = n;
        return;
    }
    // calloc, not a vector: a vector writes its zeros and so makes every page
    // resident; calloc's large blocks are mapped zeroed on demand.
    p_.reset();
    size_ = capacity_ = 0;
    void *p = std::calloc(n, 1);
    if (p == nullptr) throw std::bad_alloc();
    p_.reset(static_cast<std::uint8_t *>(p));
    size_ = capacity_ = n;
}

void WavefrontImages::reset() {
    for (WaveImage &im : img_) {
        const std::uint32_t g = im.generation;
        im = WaveImage();
        im.generation = g + 1;
        im.dirty_from = g;
    }
    layers_ = 0;
    style_ = Style();
    search_ = kNone32;
    wave_.reset();
    source_ = kNone32;
    count_ = 0;
    nc_ = 0;
    nx_ = 0;
}

void WavefrontImages::make_luts() {
    for (int k = 0; k < 2; ++k) {
        const Rgba c = style_.color[k];
        const double r = (c >> 24) & 0xFF, g = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF;
        auto white = [](double v, double f) { return v + (255.0 - v) * f; };
        for (int q = 0; q < 256; ++q) {
            const double x = q / 255.0;
            auto lerp = [&](double a0, double a1) { return a0 + (a1 - a0) * x; };
            ramp_[k][q] = premultiplied(lerp(white(r, colors.near_white), r), lerp(white(g, colors.near_white), g),
                                        lerp(white(b, colors.near_white), b), lerp(colors.near_alpha, colors.far_alpha));
        }
        front_[k] = premultiplied(white(r, colors.frontier_white), white(g, colors.frontier_white),
                                  white(b, colors.frontier_white), colors.frontier_alpha);
    }
}

void WavefrontImages::touch(int k, std::int32_t i, std::int32_t j) {
    unite(img_[k].bounds, i, j);
    unite(img_[k].dirty, i, j);
    changed_[k] = true;
}

void WavefrontImages::paint(std::uint32_t k, bool frontier) {
    const std::uint32_t cell = wave_->cells[k];
    const std::uint32_t layer = static_cast<std::uint32_t>(cell / nc_);
    const int im = layers_ == 1 ? 0 : static_cast<int>(layer & 1);
    if (!style_.visible[im]) return;
    WaveImage &img = img_[im];
    if (img.pixels.empty()) img.pixels.allocate(static_cast<std::size_t>(img.width) * img.height * 4);
    const std::uint64_t idx = cell % nc_;
    std::uint32_t color;
    if (frontier) {
        color = front_[im];
    } else {
        int q = 0;
        if (!wave_->costs.empty()) {
            const std::int64_t c = wave_->costs[k];
            if (c > 0 && max_cost_ > 0) q = static_cast<int>(std::min<std::int64_t>(255, c * 255 / max_cost_));
        } else if (wave_->cells.size() > 1) {
            q = static_cast<int>(static_cast<std::uint64_t>(k) * 255 / (wave_->cells.size() - 1));
        }
        color = ramp_[im][q];
    }
    std::uint8_t *p = img.pixels.data() + 4 * idx;
    if (p[3] == 0) ++img.cells;
    std::memcpy(p, &color, 4);
    touch(im, static_cast<std::int32_t>(idx % static_cast<std::uint64_t>(nx_)),
          static_cast<std::int32_t>(idx / static_cast<std::uint64_t>(nx_)));
}

void WavefrontImages::clear_cell(std::uint32_t k) {
    const std::uint32_t cell = wave_->cells[k];
    const int im = layers_ == 1 ? 0 : static_cast<int>((cell / nc_) & 1);
    WaveImage &img = img_[im];
    if (!style_.visible[im] || img.pixels.empty()) return;
    const std::uint64_t idx = cell % nc_;
    std::uint8_t *p = img.pixels.data() + 4 * idx;
    if (p[3] != 0 && img.cells > 0) --img.cells;
    std::memset(p, 0, 4);
    touch(im, static_cast<std::int32_t>(idx % static_cast<std::uint64_t>(nx_)),
          static_cast<std::int32_t>(idx / static_cast<std::uint64_t>(nx_)));
}

// Zero the pixels inside the bounds (outside them every pixel is zero);
// true if there were any.
static bool zero_drawn(WaveImage &img) {
    const auto &b = img.bounds;
    if (!(b[0] < b[2] && b[1] < b[3] && !img.pixels.empty())) return false;
    for (std::int32_t y = b[1]; y < b[3]; ++y)
        std::memset(img.pixels.data() + 4 * (static_cast<std::size_t>(y) * img.width + b[0]), 0,
                    4 * static_cast<std::size_t>(b[2] - b[0]));
    return true;
}

void WavefrontImages::clear_all() {
    for (int k = 0; k < 2; ++k) {
        WaveImage &img = img_[k];
        const auto &b = img.bounds;
        if (zero_drawn(img)) {
            unite(img.dirty, b);
            changed_[k] = true;
        }
        img.bounds = {{0, 0, 0, 0}};
        img.cells = 0;
    }
}

void WavefrontImages::load(Trace &t, std::uint32_t source) {
    wave_.reset();
    source_ = kNone32;
    count_ = 0;
    if (source == kNone32) return;
    std::shared_ptr<const Wavefront> w = t.wavefront(source);
    if (!w || w->cells.empty()) return;
    wave_ = std::move(w);
    source_ = source;
    const auto n = static_cast<std::uint32_t>(wave_->cells.size());
    frontier_ = frontier_cells(n);
    max_cost_ = 0;
    for (std::int32_t c : wave_->costs) max_cost_ = std::max<std::int64_t>(max_cost_, c);
    // Each cell at most once (the format says so; a file that breaks it
    // is still drawn, by repainting only).
    unique_ = true;
    if (!dense_cell_array_ok(2 * nc_, n)) {
        // A large grid and a small search: sort a copy rather than hold a
        // byte per cell of the grid.
        std::vector<std::uint32_t> sorted(wave_->cells);
        std::sort(sorted.begin(), sorted.end());
        unique_ = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
        return;
    }
    if (seen_.size() < 2 * nc_) seen_.assign(static_cast<std::size_t>(2 * nc_), 0);
    std::uint32_t k = 0;
    for (; k < n; ++k) {
        std::uint8_t &s = seen_[wave_->cells[k]];
        if (s != 0) {
            unique_ = false;
            break;
        }
        s = 1;
    }
    for (std::uint32_t q = 0; q < k; ++q) seen_[wave_->cells[q]] = 0;
}

void WavefrontImages::update(Trace &t, const WaveRequest &req) {
    changed_[0] = changed_[1] = false;
    // This update's dirty boxes start empty; an image that does not change
    // keeps the box and base of its last change.
    const std::array<std::array<std::int32_t, 4>, 2> old_dirty{{img_[0].dirty, img_[1].dirty}};
    const std::uint32_t old_from[2] = {img_[0].dirty_from, img_[1].dirty_from};
    img_[0].dirty = img_[1].dirty = std::array<std::int32_t, 4>{{0, 0, 0, 0}};

    Style st;
    const bool have_run = req.session < t.runs().size() && t.runs()[req.session].valid;
    std::int32_t nx = 0, ny = 0;
    if (have_run) {
        const Run &run = t.runs()[req.session];
        nx = run.nx;
        ny = run.ny;
        st.session = req.session;
        st.layers = req.layers == 1 ? 1 : 2;
        for (int k = 0; k < 2; ++k) {
            st.visible[k] = req.visible[k];
            st.color[k] = req.color[k];
        }
        const Frame fr(run);
        const double c = run.cell;
        for (int k = 0; k < 2; ++k) {
            WaveImage &im = img_[k];
            im.rect = {{fr.x(-c / 2), fr.y(-c / 2), fr.x((nx - 0.5) * c), fr.y((ny - 0.5) * c)}};
            im.order = req.order[k];
            im.board_layer = req.board_layer[k];
        }
    }
    const bool same_style = st == style_;
    const bool resized = img_[0].width != static_cast<std::uint32_t>(nx) || img_[0].height != static_cast<std::uint32_t>(ny);
    if (resized) {
        for (int k = 0; k < 2; ++k) {
            WaveImage &im = img_[k];
            if (im.width != 0 || nx != 0) {
                unite(img_[k].dirty, std::array<std::int32_t, 4>{{0, 0, std::max<std::int32_t>(nx, static_cast<std::int32_t>(im.width)),
                                                                 std::max<std::int32_t>(ny, static_cast<std::int32_t>(im.height))}});
                changed_[k] = true;
            }
            // Keep the memory, all zero, for the next drawing (PixelBuffer).
            zero_drawn(im);
            im.pixels.clear();
            im.width = static_cast<std::uint32_t>(nx);
            im.height = static_cast<std::uint32_t>(ny);
            im.bounds = {{0, 0, 0, 0}};
            im.cells = 0;
        }
        wave_.reset();
        source_ = kNone32;
        count_ = 0;
    }
    if (!same_style) {
        style_ = st;
        make_luts();
    }
    layers_ = have_run ? st.layers : 0;
    if (have_run) {
        nc_ = t.runs()[req.session].nc();
        nx_ = nx;
    }

    // The arrays to show.
    const std::uint32_t want = have_run ? req.source : kNone32;
    const bool same_source = want == source_ && wave_ != nullptr;
    const std::uint32_t old_count = same_source ? count_ : 0;
    std::exception_ptr failure;
    if (!same_source) {
        try {
            load(t, want);
        } catch (...) {
            // Unreadable arrays: show nothing, report the error after the
            // images are consistent again.
            failure = std::current_exception();
            wave_.reset();
            source_ = kNone32;
        }
    }
    const std::uint32_t n1 = wave_ ? std::min<std::uint32_t>(req.count, static_cast<std::uint32_t>(wave_->cells.size())) : 0;
    search_ = n1 > 0 ? req.search : kNone32;

    const std::uint32_t n0 = old_count;
    const std::uint32_t delta = n1 > n0 ? n1 - n0 : n0 - n1;
    const bool step = !always_rebuild && same_style && !resized && same_source && unique_ && n1 > 0 && delta < n1;
    if (step) {
        if (n1 != n0) ++incremental;
        const std::int64_t F = frontier_;
        const std::int64_t a = n0, b = n1;
        if (b > a) {
            // Cells that leave the frontier, then the new cells.
            for (std::int64_t k = std::max<std::int64_t>(0, a - F); k < std::min(a, b - F); ++k)
                paint(static_cast<std::uint32_t>(k), false);
            for (std::int64_t k = a; k < b; ++k) paint(static_cast<std::uint32_t>(k), k >= b - F);
        } else if (b < a) {
            // Cells hidden again, then the cells that become frontier.
            for (std::int64_t k = b; k < a; ++k) clear_cell(static_cast<std::uint32_t>(k));
            for (std::int64_t k = std::max<std::int64_t>(0, b - F); k < std::min(b, a - F); ++k)
                paint(static_cast<std::uint32_t>(k), true);
        }
    } else {
        ++rebuilds;
        clear_all();
        const std::int64_t F = frontier_;
        for (std::uint32_t k = 0; k < n1; ++k) paint(k, static_cast<std::int64_t>(k) >= static_cast<std::int64_t>(n1) - F);
    }
    count_ = n1;

    for (int k = 0; k < 2; ++k) {
        WaveImage &im = img_[k];
        if (changed_[k]) {
            im.dirty_from = im.generation;
            ++im.generation;
        } else {
            im.dirty = old_dirty[k];
            im.dirty_from = old_from[k];
        }
    }
    if (failure) std::rethrow_exception(failure);
}

} // namespace rv

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// test_limits.cpp: what a hostile or broken file cannot make the core do.
//
// The grid limit (TRACE_FORMAT.md 3.4: at most 8192 cells per side) and
// memory in proportion to a search on the largest grid; the bound on rule
// area hatching; times that no pacing turns into an out-of-range conversion
// (run these under `make asan`, which includes UBSan); opening a named pipe,
// a directory or a device fails at once.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "display.hpp"
#include "hittest.hpp"
#include "playback.hpp"
#include "rv.h"
#include "theme.hpp"
#include "timeline.hpp"
#include "trace.hpp"
#include "wavefront.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct SessionGuard {
    rv_session *s = nullptr;
    SessionGuard() { rv_create(&s); }
    ~SessionGuard() { rv_destroy(s); }
};

bool has_issue(const rv::Trace &t, const char *text) {
    for (const rv::Issue &is : t.issues())
        if (is.severity == rv::Issue::Error && is.message.find(text) != std::string::npos) return true;
    return false;
}

// One cell of a search's stored wavefront, in grid terms.
struct WaveCell {
    std::uint32_t layer;
    std::int32_t i, j;
};

// A RUN of nx x ny and one search whose stored wavefront is `cells` (in
// order), then END.
std::vector<unsigned char> one_search(std::int32_t nx, std::int32_t ny, const std::vector<WaveCell> &cells) {
    synth::Writer w;
    w.chunk("RUN ", synth::encode(synth::make_run(nx, ny, 8)));
    const std::uint64_t nc = static_cast<std::uint64_t>(nx) * static_cast<std::uint64_t>(ny);
    auto cell = [&](const WaveCell &c) {
        return static_cast<std::uint32_t>(c.layer * nc + static_cast<std::uint64_t>(c.j) * static_cast<std::uint64_t>(nx) +
                                          static_cast<std::uint64_t>(c.i));
    };
    synth::WSrch s;
    s.net = 1;
    s.flags = (1u << 7) | (1u << 9); // has_costs, has_trace
    s.expanded = static_cast<std::int64_t>(cells.size());
    s.duration_s = 0.01;
    s.src = {cell(cells.front())};
    s.tgt = {cell(cells.back())};
    for (std::size_t k = 0; k < cells.size(); ++k) {
        s.trace_cell.push_back(cell(cells[k]));
        s.trace_d.push_back(static_cast<std::int32_t>(10 * k));
    }
    s.ntrace_total = static_cast<std::uint32_t>(cells.size());
    w.chunk("SRCH", synth::encode(s));
    w.chunk("END ", synth::encode(synth::WEnd()));
    return w.out;
}

// The cells of the memory tests, placed relative to the grid size: corners,
// both layers, and one cell twice (positions 1 and 5).
std::vector<WaveCell> spread_cells(std::int32_t nx, std::int32_t ny) {
    return {{0, 0, 0},           {0, nx - 1, ny - 1}, {1, nx / 2, 17 % ny}, {1, 3, ny - 1},
            {0, nx - 1, 0},      {0, nx - 1, ny - 1}, {1, 0, ny / 3},      {0, nx / 3, ny / 2}};
}

std::uint32_t pixel(const rv::WaveImage &im, std::int32_t i, std::int32_t j) {
    std::uint32_t v = 0;
    if (!im.pixels.empty())
        std::memcpy(&v, im.pixels.data() + 4 * (static_cast<std::size_t>(j) * im.width + static_cast<std::size_t>(i)), 4);
    return v;
}

struct Shown {
    std::unique_ptr<rv::Trace> trace;
    std::unique_ptr<rv::Timeline> timeline;
    std::uint32_t event = rv::kNone32;
    rv::WaveRequest req;
};

// The trace opened, its search event found and the request that shows all
// of its wavefront.
Shown show_search(const std::vector<unsigned char> &bytes) {
    Shown sh;
    sh.trace = rvtest::open_bytes(bytes);
    sh.timeline.reset(new rv::Timeline(*sh.trace, rv::FitLengthPacing()));
    for (std::uint32_t e = 0; e < sh.timeline->size(); ++e)
        if (sh.timeline->events()[e].kind == rv::EvKind::Search) sh.event = e;
    if (sh.event == rv::kNone32) return sh;
    const rv::Event &ev = sh.timeline->events()[sh.event];
    sh.req.session = ev.session;
    sh.req.source = rv::wavefront_source(*sh.trace, *sh.timeline, sh.event);
    sh.req.search = ev.record;
    sh.req.count = sh.req.source != rv::kNone32 ? sh.trace->searches()[sh.req.source].ntrace : 0;
    sh.req.color[0] = 0xC83434FFu;
    sh.req.color[1] = 0x4D7FC4FFu;
    return sh;
}

// The wavefront hits at cell (i, j), as (router layer, order index) pairs.
std::set<std::pair<std::uint32_t, std::uint32_t>> wave_hits(rv::Picker &pk, Shown &sh, std::int32_t i, std::int32_t j) {
    static const std::vector<rv::CopperGeom> no_copper;
    rv::PickInput in;
    in.trace = sh.trace.get();
    in.timeline = sh.timeline.get();
    in.geometry = &no_copper;
    in.event = sh.event;
    in.router_layer_count = 2;
    in.wave_shown = true;
    in.wave_source = sh.req.source;
    in.wave_search = sh.req.search;
    in.wave_count = sh.req.count;
    const rv::Run &run = sh.trace->runs()[sh.req.session];
    const rv::Frame fr(run);
    in.x = fr.cx(i);
    in.y = fr.cy(j);
    std::set<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const rv::Hit &h : pk.pick(in))
        if (h.kind == rv::hitkind::Wavefront) out.emplace(h.router_layer, h.order_index);
    return out;
}

} // namespace

// --- 1. the grid limit and memory in proportion to the search ------------------------

TEST(run_grid_beyond_the_router_limit_is_corrupt) {
    const std::int32_t sizes[][2] = {{8193, 10}, {10, 8193}, {70000, 70000}, {8192, 8192}};
    for (const auto &sz : sizes) {
        synth::Writer w;
        w.chunk("RUN ", synth::encode(synth::make_run(sz[0], sz[1], 8)));
        synth::WMark m;
        m.text = "after";
        w.chunk("MARK", synth::encode(m));
        auto t = rvtest::open_bytes(w.out);
        const bool ok = sz[0] <= 8192 && sz[1] <= 8192;
        CHECK_EQ(t->chunks().size(), std::size_t(2));
        CHECK_EQ(t->runs().size(), std::size_t(1));
        CHECK_EQ(t->chunks()[0].corrupt, !ok);
        CHECK_EQ(t->runs()[0].valid, ok);
        CHECK_EQ(has_issue(*t, "8192 cells per side"), !ok);
        CHECK_EQ(t->error_count(), ok ? 0u : 1u);
    }
    // Through the C interface: the file opens, the RUN is reported corrupt
    // with the reason, and the session has no grid.
    rvtest::TempDir dir;
    synth::Writer w;
    w.chunk("RUN ", synth::encode(synth::make_run(8193, 8193, 8)));
    const std::string path = dir.path("big.grtrace");
    CHECK(synth::write_file(path, w.out));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    rv_issue_info is;
    CHECK_EQ(rv_issue(s.s, 0, &is), RV_OK);
    CHECK_EQ(is.severity, RV_ISSUE_ERROR);
    CHECK(is.message != nullptr && std::strstr(is.message, "8192 cells per side") != nullptr);
    rv_run_info ri;
    CHECK_EQ(rv_run(s.s, 0, &ri), RV_OK);
    CHECK_EQ(ri.valid, 0u);
    uint32_t images = 9;
    CHECK_EQ(rv_wavefront_image_count(s.s, &images), RV_OK);
    CHECK_EQ(images, 0u);
}

TEST(wavefront_on_the_largest_grid_costs_what_the_search_does) {
    const std::int32_t n = 8192;
    const std::vector<WaveCell> cells = spread_cells(n, n);
    Shown sh = show_search(one_search(n, n, cells));
    CHECK(sh.event != rv::kNone32 && sh.req.source != rv::kNone32);
    if (sh.req.source == rv::kNone32) return;
    CHECK_EQ(sh.req.count, static_cast<std::uint32_t>(cells.size()));
    rv::WavefrontImages wi;
    wi.update(*sh.trace, sh.req);
    // The duplicate check sorted the entries: no array of 2 * 8192^2 bytes.
    CHECK_EQ(wi.scratch_bytes(), std::size_t(0));
    CHECK_EQ(wi.count(), 2);
    for (int k = 0; k < 2; ++k) {
        const rv::WaveImage &im = wi.image(k);
        CHECK_EQ(im.width, 8192u);
        CHECK_EQ(im.height, 8192u);
        CHECK_EQ(im.pixels.size(), std::size_t(8192) * 8192 * 4);
    }
    // Every cell drawn on its layer, nothing else counted.
    std::set<std::pair<std::uint32_t, std::pair<std::int32_t, std::int32_t>>> distinct;
    for (const WaveCell &c : cells) {
        CHECK(pixel(wi.image(static_cast<int>(c.layer)), c.i, c.j) != 0);
        distinct.insert({c.layer, {c.i, c.j}});
    }
    std::uint32_t per_layer[2] = {0, 0};
    for (const auto &d : distinct) ++per_layer[d.first];
    CHECK_EQ(wi.image(0).cells, per_layer[0]);
    CHECK_EQ(wi.image(1).cells, per_layer[1]);
    CHECK(pixel(wi.image(0), 1, 1) == 0);
    // Moving within the search (fewer, then all entries) and away and back:
    // the same pixels; the image memory is reused (its size never changes).
    rv::WaveRequest fewer = sh.req;
    fewer.count = 3;
    wi.update(*sh.trace, fewer);
    CHECK(pixel(wi.image(0), n - 1, 0) == 0); // entry 4 hidden again
    CHECK(pixel(wi.image(0), 0, 0) != 0);
    rv::WaveRequest none;
    wi.update(*sh.trace, none);
    CHECK_EQ(wi.count(), 0);
    CHECK(wi.image(0).pixels.empty());
    wi.update(*sh.trace, sh.req);
    for (const WaveCell &c : cells) CHECK(pixel(wi.image(static_cast<int>(c.layer)), c.i, c.j) != 0);
    CHECK_EQ(wi.image(0).cells, per_layer[0]);
    CHECK_EQ(wi.scratch_bytes(), std::size_t(0));
}

TEST(wavefront_hit_index_in_proportion_to_the_search) {
    // The same relative cells on a small grid (a per-cell index) and on the
    // largest (sorted pairs): the same hits, with the first position of the
    // cell listed twice.
    const std::int32_t sides[] = {40, 8192};
    std::vector<std::vector<std::set<std::pair<std::uint32_t, std::uint32_t>>>> results;
    for (std::int32_t n : sides) {
        const std::vector<WaveCell> cells = spread_cells(n, n);
        Shown sh = show_search(one_search(n, n, cells));
        CHECK(sh.req.source != rv::kNone32);
        if (sh.req.source == rv::kNone32) return;
        rv::Picker pk;
        std::vector<std::set<std::pair<std::uint32_t, std::uint32_t>>> got;
        for (const WaveCell &c : cells) got.push_back(wave_hits(pk, sh, c.i, c.j));
        got.push_back(wave_hits(pk, sh, 1, 1)); // not in the wavefront
        const std::size_t dense = 4 * 2 * static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
        if (n == 40) CHECK_EQ(pk.wave_index_bytes(), dense);
        else CHECK_EQ(pk.wave_index_bytes(), 8 * cells.size());
        results.push_back(got);
    }
    CHECK(results.size() == 2 && results[0] == results[1]);
    if (results.size() != 2) return;
    const auto &r = results[1];
    CHECK(r[0].count({0u, 0u}) == 1);
    CHECK(r[1].count({0u, 1u}) == 1); // (n-1, n-1) on layer 0: first at 1, again at 5
    CHECK(r[5].count({0u, 5u}) == 0);
    CHECK(r[2].count({1u, 2u}) == 1);
    CHECK(r[8].empty());
}

// --- 2. rule-area hatching is bounded -------------------------------------------------

namespace {

synth::WBrd rule_area_board(const std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>> &polys) {
    synth::WBrd b;
    b.layers = {{0, 0, "F.Cu"}};
    for (const auto &contour : polys) {
        synth::WPolyset p;
        p.role = 6;
        p.polys = {{contour}};
        b.polysets.push_back(p);
        b.order.emplace_back(1u, static_cast<std::uint32_t>(b.polysets.size() - 1));
    }
    return b;
}

struct Drawn {
    std::uint64_t hatch = 0, outline_points = 0;
};

Drawn draw(const synth::WBrd &b) {
    synth::Writer w;
    w.chunk("BRD ", synth::encode(b));
    auto t = rvtest::open_bytes(w.out);
    Drawn d;
    if (t->boards().empty()) return d;
    auto geo = t->board_geometry(0);
    const rv::DisplayList dl = rv::build_display_list(t->boards()[0], *geo, rv::Theme());
    for (const rv::Batch &x : dl.batches) {
        if (x.cls != static_cast<std::uint32_t>(rv::ObjClass::RuleAreas)) continue;
        if (x.prim == rv::Prim::Segs) d.hatch += x.count;
        if (x.prim == rv::Prim::Stroke)
            for (std::uint32_t c = x.first; c < x.first + x.count; ++c) d.outline_points += dl.contours[2 * c + 1];
    }
    return d;
}

// A square of side mm with corners at (x0, 0), `per_side` points per side.
std::vector<std::pair<std::int32_t, std::int32_t>> square(std::int32_t x0_mm, std::int32_t side_mm, std::int32_t per_side) {
    std::vector<std::pair<std::int32_t, std::int32_t>> c;
    const std::int64_t s = static_cast<std::int64_t>(side_mm) * 1000000, x0 = static_cast<std::int64_t>(x0_mm) * 1000000;
    auto at = [&](std::int64_t x, std::int64_t y) { c.emplace_back(static_cast<std::int32_t>(x0 + x), static_cast<std::int32_t>(y)); };
    for (std::int32_t k = 0; k < per_side; ++k) at(s * k / per_side, 0);
    for (std::int32_t k = 0; k < per_side; ++k) at(s, s * k / per_side);
    for (std::int32_t k = 0; k < per_side; ++k) at(s - s * k / per_side, s);
    for (std::int32_t k = 0; k < per_side; ++k) at(0, s - s * k / per_side);
    return c;
}

} // namespace

TEST(rule_area_hatching_is_bounded) {
    // A normal rule area: hatched.
    const Drawn small = draw(rule_area_board({square(0, 2, 1)}));
    CHECK(small.hatch > 0);
    CHECK_EQ(small.outline_points, std::uint64_t(5));
    // A 10,000-point saw tooth 2000 mm wide and high (the 80 KB file that
    // gave 7 million segments): its outline only, quickly.
    std::vector<std::pair<std::int32_t, std::int32_t>> saw;
    const std::int32_t teeth = 5000, w = 400000, h = 2000000000;
    for (std::int32_t k = 0; k < teeth; ++k) {
        saw.emplace_back(k * w, 0);
        saw.emplace_back(k * w + w / 2, h);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const Drawn big = draw(rule_area_board({saw}));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(big.hatch, std::uint64_t(0));
    CHECK_EQ(big.outline_points, std::uint64_t(2 * teeth + 1));
    CHECK(ms < 2000);
    // Many rule areas, each under the per-polygon bound: hatched until the
    // board's bound, the rest outlined.  Each square: 100 mm, 2000 edges,
    // about 283 lines (566,000 edge tests, one segment each); 14 fit in
    // kHatchWorkPerBoard.  (Squares at other places may get a line more or
    // less.)
    const Drawn one = draw(rule_area_board({square(0, 100, 500)}));
    CHECK(one.hatch > 200);
    std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>> many;
    for (std::int32_t k = 0; k < 20; ++k) many.push_back(square(110 * k, 100, 500));
    const Drawn all = draw(rule_area_board(many));
    CHECK_EQ(all.outline_points, 20 * one.outline_points);
    const std::uint64_t hatched = one.hatch > 0 ? (all.hatch + one.hatch / 2) / one.hatch : 0;
    const std::uint64_t off = all.hatch > hatched * one.hatch ? all.hatch - hatched * one.hatch : hatched * one.hatch - all.hatch;
    std::cout << "    rule areas: " << hatched << " of 20 squares hatched, " << all.hatch << " segments; saw tooth "
              << ms << " ms\n";
    CHECK(hatched >= 8 && hatched < 20);
    CHECK(off <= hatched);
    CHECK(all.hatch * 2 <= rv::kHatchWorkPerBoard);
}

// --- 3. extreme chunk times -------------------------------------------------------------

TEST(extreme_chunk_times_in_every_pacing_mode) {
    // Chunk times far out of range: negative, NaN, infinite and 1e300.  Each
    // pacing mode, seeking and playing, every text: no conversion out of
    // range (UBSan in `make asan` aborts on one), finite times.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<std::vector<double>> schedules = {
        {0, 1, 2, 3, 4, 5, 6, 1e300},
        {-5, -1e300, nan, 1, nan, 2, 3, 1e300},
        {0, 1e300, 1e300, 1.5e308, inf, 1.7e308, 1.79e308, 1.79e308},
        {nan, nan, nan, nan, nan, nan, nan, nan},
        {-1e300, -1e299, -1, 0, 1e-300, 1e10, 1e200, -inf},
    };
    rvtest::TempDir dir;
    int file = 0;
    for (const std::vector<double> &ts : schedules) {
        synth::Writer w;
        synth::WNets nets;
        nets.entries = {{1, "A"}};
        std::size_t k = 0;
        w.chunk_at("RUN ", synth::encode(synth::make_run(6, 5, 4)), ts[k++]);
        w.chunk_at("NETS", synth::encode(nets), ts[k++]);
        synth::WMark m;
        m.text = "routing";
        w.chunk_at("MARK", synth::encode(m), ts[k++]);
        synth::WSrch s;
        s.net = 1;
        s.src = {0};
        s.tgt = {7};
        s.expanded = 100;
        w.chunk_at("SRCH", synth::encode(s), ts[k++]);
        s.search_id = 1;
        s.flags = 1u << 5; // found
        s.path = {0, 1, 7};
        s.cost = 30;
        w.chunk_at("SRCH", synth::encode(s), ts[k++]);
        synth::WCmit c;
        c.net = 1;
        c.width_mm = 0.2;
        c.path = {0, 1, 7};
        w.chunk_at("CMIT", synth::encode(c), ts[k++]);
        synth::WRip r;
        r.net = 1;
        r.removed = {0};
        w.chunk_at("RIP ", synth::encode(r), ts[k++]);
        w.chunk_at("END ", synth::encode(synth::WEnd()), ts[k++]);
        const std::string path = dir.path("times" + std::to_string(file++) + ".grtrace");
        CHECK(synth::write_file(path, w.out));
        for (uint32_t live = 0; live < 2; ++live) {
            SessionGuard ss;
            rv_session *x = ss.s;
            CHECK_EQ(rv_open_ex(x, path.c_str(), live ? RV_OPEN_LIVE : 0), RV_OK);
            const rv_pacing pacings[] = {
                {RV_PACING_FIT, 0, 180.0, 10.0},       {RV_PACING_NETS_ONLY, 0, 86400.0, 10.0},
                {RV_PACING_ONE_NET, 1, 1.0, 10.0},     {RV_PACING_SKIP_FAILED, 0, 180.0, 10.0},
                {RV_PACING_REAL_TIME, 0, 180.0, 0.01}, {RV_PACING_REAL_TIME, 0, 180.0, 10.0},
                {RV_PACING_REAL_TIME, 0, 180.0, 100000.0},
            };
            for (const rv_pacing &p : pacings) {
                uint32_t changed = 0;
                CHECK_EQ(rv_set_pacing(x, &p, &changed), RV_OK);
                rv_timeline tl;
                CHECK_EQ(rv_get_timeline(x, &tl), RV_OK);
                CHECK(std::isfinite(tl.total) && tl.total >= 0);
                CHECK(std::isfinite(tl.span) && tl.span >= 0);
                for (uint32_t e = 0; e < tl.event_count; ++e)
                    CHECK(std::isfinite(tl.events[e].start) && std::isfinite(tl.events[e].duration) &&
                          tl.events[e].duration >= 0 && tl.events[e].duration <= rv::kMaxEventSeconds);
                auto texts = [&]() {
                    for (uint32_t which = RV_TEXT_CAPTION; which <= RV_TEXT_PACING; ++which) {
                        const char *text = nullptr;
                        CHECK_EQ(rv_text(x, which, &text), RV_OK);
                        CHECK(text != nullptr);
                    }
                    rv_position pos;
                    CHECK_EQ(rv_position_get(x, &pos), RV_OK);
                    rv_nets_info ni;
                    CHECK_EQ(rv_nets_get(x, &ni), RV_OK);
                    const char *hover = nullptr;
                    rv_hover_status(x, 10, 10, &hover);
                };
                const double fractions[] = {0.0, 0.3, 0.5, 0.999, 1.0};
                for (double f : fractions) {
                    CHECK_EQ(rv_seek_fraction(x, f, &changed), RV_OK);
                    texts();
                }
                const double times[] = {-1e300, -1, 0.5, 1e9, 1e300, inf, -inf, nan};
                for (double t : times) {
                    rv_seek_time(x, t, &changed);
                    texts();
                }
                CHECK_EQ(rv_jump(x, 0, &changed), RV_OK);
                CHECK_EQ(rv_play(x, 1, &changed), RV_OK);
                const double steps[] = {0.016, 1e6, 1e300};
                for (double dt : steps) {
                    rv_advance(x, dt, &changed);
                    texts();
                }
                for (uint32_t kind = 0; kind < RV_STEP_KIND_COUNT; ++kind) {
                    rv_step(x, kind, 1, &changed);
                    rv_step(x, kind, -1, &changed);
                }
                texts();
                CHECK_EQ(rv_play(x, 0, &changed), RV_OK);
            }
        }
    }
    // The clock texts themselves, past any time the timeline makes.
    rv::PacingSettings ps;
    ps.mode = rv::PacingMode::RealTime;
    const double values[] = {-1, 0, 59.96, 1e9, 1e18, 1e300, inf, -inf, nan};
    for (double v : values) CHECK(!rv::pacing_text(ps, nullptr, v).empty());
}

// --- 4. a named pipe, a directory or a device fails at once ------------------------------

TEST(opening_a_fifo_directory_or_device_fails_promptly) {
    rvtest::TempDir dir;
    CHECK(dir.ok());
    auto open_fails = [](const std::string &path, uint32_t flags) {
        std::atomic<int> status(-1);
        std::string error;
        std::thread th([&]() {
            rv_session *s = nullptr;
            rv_create(&s);
            const rv_status st = rv_open_ex(s, path.c_str(), flags);
            error = rv_last_error(s);
            rv_destroy(s);
            status = static_cast<int>(st);
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (status.load() < 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool blocked = status.load() < 0;
        if (blocked) {
            // The open is waiting for a writer: be that writer, so it returns.
            std::ofstream unblock(path);
        }
        th.join();
        CHECK(!blocked);
        CHECK_EQ(status.load(), static_cast<int>(RV_E_IO));
        CHECK(error.find("not a regular file") != std::string::npos);
        return !blocked && status.load() == static_cast<int>(RV_E_IO);
    };
    // A directory.
    const std::string sub = dir.path("adir");
    std::error_code ec;
    CHECK(std::filesystem::create_directory(sub, ec));
    CHECK(open_fails(sub, 0));
    CHECK(open_fails(sub, RV_OPEN_WAIT | RV_OPEN_LIVE));
    // A named pipe, made with the system's mkfifo command (there is no
    // standard C++ call for it); skipped where that command does not exist.
    const std::string fifo = dir.path("pipe.grtrace");
    const std::string cmd = "mkfifo '" + fifo + "' 2>/dev/null";
    if (std::system(cmd.c_str()) == 0 && std::filesystem::is_fifo(fifo, ec)) {
        CHECK(open_fails(fifo, 0));
        CHECK(open_fails(fifo, RV_OPEN_WAIT | RV_OPEN_LIVE));
    } else {
        std::cout << "    fifo: mkfifo not available, skipped\n";
    }
    // A device.
    if (std::filesystem::is_character_file("/dev/null", ec)) CHECK(open_fails("/dev/null", 0));
}

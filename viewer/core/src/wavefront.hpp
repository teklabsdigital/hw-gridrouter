// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// wavefront.hpp: the A* wavefront of the current search as images.
//
// One premultiplied RGBA image per router layer at grid resolution (nx x ny
// pixels, pixel (i, j) is cell (i, j); row 0 is the smallest board y), drawn
// by a client under that layer's copper (the RV_SLOT_WAVEFRONT slot, order
// RV_SUB_WAVEFRONT of the layer's rank) with nearest-neighbour scaling.
//
// What the images show is a pure function of
//   (router session, wavefront arrays, cells revealed, layer visibility and
//   colours):
// the first `count` entries of the arrays in expansion order, each coloured
// by its cost so far relative to the largest cost of the whole wavefront
// (by its position in the order when the search stored no costs), the last
// frontier_cells(n) of them brighter.  update() reaches a new state either
// incrementally (moving within one search, either way) or by clearing and
// repainting; both give the same pixels (tests check this).
//
// Memory: the images, and the arrays of the one search shown (a shared_ptr
// from the trace's wavefront cache); nothing else is kept.  An image's
// pixels are allocated only when a cell is first drawn on it, as zeroed
// memory from calloc, which the system maps on demand: only the pages that
// hold drawn cells become resident, so a small search on a large grid (up
// to 8192 x 8192, 256 MiB of pixels per image) costs little.  The duplicate
// check uses a per-cell array only when that is small or no larger than the
// search's own arrays (dense_cell_array_ok), else a sorted copy.
#ifndef RV_WAVEFRONT_HPP
#define RV_WAVEFRONT_HPP

#include "records.hpp"
#include "theme.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rv {

class Trace;

// Colours of the wavefront, mixed from the router layer's theme colour C.
// A cell's cost ratio r (0 at the sources, 1 at the largest cost) picks
//   colour = lerp(mix_white(C, near_white), C, r), alpha = lerp(near_alpha, far_alpha, r);
// frontier cells are mix_white(C, frontier_white) at frontier_alpha.
struct WaveColors {
    // M5: a faint tint of the layer's own colour that keeps the board
    // readable through it (M4 had 0.55, 0.60, 0.28, 0.85, 0.95).
    double near_white = 0.18;
    double near_alpha = 0.22;
    double far_alpha = 0.08;
    double frontier_white = 0.50;
    double frontier_alpha = 0.55;
};

// The share of a wavefront's stored entries drawn as its frontier: the most
// recently revealed max(1, ceil(n * kFrontierShare)) cells.
constexpr double kFrontierShare = 0.02; // M4: 0.04
std::uint32_t frontier_cells(std::uint32_t n);

// What the images should show.
struct WaveRequest {
    std::uint32_t session = kNone32;  // router session (RUN index); kNone32: no images
    std::uint32_t source = kNone32;   // search whose stored arrays are shown; kNone32: nothing shown
    std::uint32_t search = kNone32;   // the search event's own search_id (reported only)
    std::uint32_t count = 0;          // entries revealed
    int layers = 2;                   // 1 when both router layers have one name
    bool visible[2] = {true, true};   // per router layer; false: that image stays empty
    Rgba color[2] = {0, 0};           // theme colour per router layer
    std::uint32_t order[2] = {0, 0};  // order key of each image's slot
    std::uint32_t board_layer[2] = {kNone32, kNone32};
};

// Zeroed bytes from calloc (see Memory above); empty until allocate().  The
// memory is kept when the buffer is emptied (clear) and serves the next
// allocate that fits: freeing and allocating again would let calloc hand
// back the same block, which it must then zero, making all of it resident.
class PixelBuffer {
public:
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    std::uint8_t *data() { return p_.get(); }
    const std::uint8_t *data() const { return p_.get(); }
    // n zero bytes (n > 0); throws std::bad_alloc.
    void allocate(std::size_t n);
    // Empty again, keeping the memory.  The caller has zeroed every byte it
    // wrote, so the memory is all zero.
    void clear() { size_ = 0; }

private:
    struct Free {
        void operator()(std::uint8_t *p) const;
    };
    std::unique_ptr<std::uint8_t, Free> p_;
    std::size_t size_ = 0, capacity_ = 0;
};

struct WaveImage {
    std::uint32_t width = 0, height = 0;    // nx, ny (0 without a router session)
    PixelBuffer pixels;                     // width * height * 4 once something was drawn, else empty
    std::array<std::int32_t, 4> bounds{{0, 0, 0, 0}}; // x0, y0, x1, y1: outside it every pixel is 0
    std::array<std::int32_t, 4> dirty{{0, 0, 0, 0}};  // pixels changed from generation dirty_from
    std::uint32_t generation = 1, dirty_from = 0;
    std::uint32_t cells = 0;                // cells drawn
    std::array<double, 4> rect{{0, 0, 0, 0}}; // board mm of the whole image
    std::uint32_t order = 0, board_layer = kNone32;
};

class WavefrontImages {
public:
    // Bring the images to `req`.  Throws what Trace::wavefront throws.
    void update(Trace &t, const WaveRequest &req);
    // Forget everything (another trace).
    void reset();

    int count() const { return layers_; }
    const WaveImage &image(int k) const { return img_[k & 1]; }
    std::uint32_t search() const { return search_; }

    WaveColors colors;
    // Tests: always clear and repaint instead of updating incrementally.
    bool always_rebuild = false;
    // Counts of the two ways of updating (tests, benchmarks).
    std::uint64_t rebuilds = 0, incremental = 0;
    // Bytes of the per-cell scratch array of the duplicate check (tests).
    std::size_t scratch_bytes() const { return seen_.size(); }

private:
    struct Style {
        std::uint32_t session = kNone32;
        int layers = 0;
        bool visible[2] = {false, false};
        Rgba color[2] = {0, 0};
        bool operator==(const Style &o) const {
            return session == o.session && layers == o.layers && visible[0] == o.visible[0] &&
                   visible[1] == o.visible[1] && color[0] == o.color[0] && color[1] == o.color[1];
        }
    };
    void load(Trace &t, std::uint32_t source);
    void make_luts();
    void clear_all();
    void paint(std::uint32_t k, bool frontier);
    void clear_cell(std::uint32_t k);
    void touch(int img, std::int32_t i, std::int32_t j);

    WaveImage img_[2];
    int layers_ = 0;
    Style style_;
    std::uint32_t search_ = kNone32;
    // The arrays shown and facts about them.
    std::shared_ptr<const Wavefront> wave_;
    std::uint32_t source_ = kNone32;
    std::uint32_t count_ = 0;     // entries drawn
    std::uint32_t frontier_ = 1;  // frontier_cells(n)
    std::int64_t max_cost_ = 0;
    bool unique_ = true;          // no cell twice: incremental updates allowed
    std::uint64_t nc_ = 0;
    std::int32_t nx_ = 0;
    std::vector<std::uint8_t> seen_; // scratch for the duplicate check
    // Premultiplied colours (bytes R, G, B, A) per router layer: 256 ramp steps and the frontier.
    std::array<std::array<std::uint32_t, 256>, 2> ramp_{};
    std::array<std::uint32_t, 2> front_{};
    // The change being made: per image, whether anything changed and where.
    bool changed_[2] = {false, false};
};

} // namespace rv

#endif // RV_WAVEFRONT_HPP

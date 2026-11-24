// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// hittest.hpp: what is under a point of the view (M5).
//
// A pick looks at what is drawn: router copper alive at the position (not
// when hidden by its class, its layer, or because the board drawn holds the
// routing), the board's pads on copper layers, tracks, vias and zone fills
// (each only when its class and a layer it is on are shown), the failed-pad
// markers, and the cells of the wavefront revealed so far (when shown).
//
// Tolerance: a point hits an item within `tol` board mm of its edge (the
// caller converts kPickPoints view points); a zone fill and a wavefront cell
// only when the point is inside.
//
// Order, topmost first: failure markers (always drawn above everything),
// then the items drawn as shapes (router copper, pads, tracks, vias) by draw
// order from the top, then the areas (zone fills, wavefront cells) by draw
// order from the top.  Ties: the item written later first, then the nearer.
#ifndef RV_HITTEST_HPP
#define RV_HITTEST_HPP

#include "display.hpp"
#include "overlay.hpp"
#include "records.hpp"
#include "routed.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rv {

class Trace;
class Timeline;

// Pick tolerance in view points.
constexpr double kPickPoints = 3.0;

// Values match RV_HIT_* in rv.h.
namespace hitkind {
enum : std::uint32_t { Routed = 1, Pad, Track, Via, Zone, Failure, Wavefront };
} // namespace hitkind

struct Hit {
    std::uint32_t kind = 0;
    std::uint32_t order = 0;           // draw order
    std::uint32_t group = 1;           // 0 markers, 1 shapes, 2 areas
    std::uint64_t seq = 0;             // file order, for ties
    double dist = 0;                   // mm from the item's edge (0 inside)
    std::uint32_t layer = kNone32;     // board layer index (kNone32: none or not on a board layer)
    std::uint32_t router_layer = kNone32;
    std::int32_t net = 0;              // router net, if has_net
    bool has_net = false;
    std::int32_t board_net = -1;       // board net code, -1 if none
    std::uint32_t copper = kNone32;    // Routed: index into Trace::copper()
    std::uint32_t event = kNone32;     // Routed: the commit's event; Failure: the reporting event;
                                       // Wavefront: the search event
    std::uint32_t record = kNone32;    // board record index in its kind; Failure: failures() index;
                                       // Wavefront: search_id
    std::uint32_t cell = kNone32;      // Wavefront: the cell
    std::uint32_t order_index = kNone32; // Wavefront: position in the expansion order
    std::int64_t cost = -1;            // Wavefront: cost so far, -1 if not stored
    std::string net_name;              // router or board net name
    std::string footprint, pad;        // Pad: footprint reference and pad number
    std::string text;                  // one line
};

struct PickInput {
    Trace *trace = nullptr;
    const Timeline *timeline = nullptr;
    const std::vector<CopperGeom> *geometry = nullptr;
    std::uint32_t event = kNone32;
    // the board drawn (nullptrs without one)
    std::uint32_t board = kNone32;
    const DisplayList *board_list = nullptr; // for layer ranks and names
    std::vector<bool> layer_visible;          // by board layer index
    std::uint32_t classes = 0;                // visible classes, bit per class
    // router content
    bool routed_shown = false;                // class shown and not hidden by the board
    RouterLayer rl[2];
    int router_layer_count = 0;
    bool wave_shown = false;
    std::uint32_t wave_source = kNone32;      // search whose arrays are shown
    std::uint32_t wave_search = kNone32;      // the event's search
    std::uint32_t wave_count = 0;             // entries revealed
    double x = 0, y = 0, tol = 0;             // board mm
};

class Picker {
public:
    void reset();
    // Every hit at the point, topmost first.  Never throws for unreadable
    // wavefront arrays (they are skipped).
    std::vector<Hit> pick(const PickInput &in);
    // Bytes of the wavefront cell index (tests): a per-cell array only when
    // dense_cell_array_ok, else sorted (cell, position) pairs.
    std::size_t wave_index_bytes() const { return wave_first_.size() * 4 + wave_pairs_.size() * 8; }

private:
    struct Shape {
        std::uint32_t kind = 0, record = 0, layer = 0, order = 0;
        float bbox[4] = {0, 0, 0, 0}; // mm
    };
    void index_board(const PickInput &in);

    std::uint32_t board_ = kNone32;
    const void *board_list_ = nullptr;
    std::shared_ptr<const BoardGeometry> geo_;
    std::vector<Shape> shapes_;
    std::map<std::int32_t, std::string> board_nets_;
    std::uint32_t wave_source_ = kNone32;
    std::shared_ptr<const Wavefront> wave_;
    std::vector<std::uint32_t> wave_first_; // per cell: first position in the order, kNone32
    // Instead of wave_first_ on a grid too large for the search: (cell,
    // position) of every entry, sorted, so a cell's first position comes first.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> wave_pairs_;
    std::uint32_t first_position(std::uint64_t cell) const;
};

} // namespace rv

#endif // RV_HITTEST_HPP

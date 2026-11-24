// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// overlay.hpp: the dynamic display list at a playback position.
//
// What changes with the position is kept out of the board's static list, so
// that moving through the run never rebuilds or re-rasterises the board:
//   - routed copper alive at the event, on each router layer (RV_SUB_ROUTED),
//     and its via drills in the hole slot (RV_SUB_ROUTED_DRILL);
//   - ghosts of copper ripped up shortly before (RV_SUB_GHOST), fading;
//   - markers and highlights in the overlay rank above everything: failed
//     pads, the current event (the commit just made, the search's sources
//     and targets and path, the pads of the net being routed, a new via).
//
// The list is cut into slots: fixed ranges of `order` where the overlay has
// content.  A client draws the static batches between slots as cached
// bitmaps and redraws only the slots whose generation changed.
//
// Batch keys: a key always names the same geometry for the life of the
// timeline (until the trace is reopened or refreshed).  Copper is grouped in
// blocks of 64 copper ids per (slot, primitive, width), so one commit or rip
// changes only a few batches and a client can keep the paths of the others.
#ifndef RV_OVERLAY_HPP
#define RV_OVERLAY_HPP

#include "display.hpp"
#include "routed.hpp"
#include "theme.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rv {

class Trace;
class Timeline;

// Overlay sub-slots (values match rv.h).
namespace sub {
constexpr std::uint32_t kGhost = 0x34;
constexpr std::uint32_t kRoutedVia = 0x3A;
constexpr std::uint32_t kRoutedEnd = 0x3C; // end of a layer's routed slot
constexpr std::uint32_t kRoutedDrill = 0x2C; // in the hole slot
constexpr std::uint32_t kMarkFailure = 0x10; // in the overlay rank
constexpr std::uint32_t kMarkCopper = 0x20;
constexpr std::uint32_t kMarkCells = 0x28;
constexpr std::uint32_t kMarkPads = 0x30;
constexpr std::uint32_t kMarkVia = 0x38;
} // namespace sub

// M5, the net highlight: two slots between Edge.Cuts (rank 950) and the
// markers (kRankOverlay).  The dim slot holds one rectangle of the
// background colour over everything drawn below it; the highlight slot holds
// copies of the selected net's copper, brightened, in an order of their own
// (highlight_order) that keeps their relative stacking.
constexpr std::uint32_t kRankDim = 955;
constexpr std::uint32_t kRankHighlight = 960;
constexpr std::uint32_t kRankHighlightEnd = 970;
// The order in the highlight slot of an item drawn at `order` below: copper
// ranks (100 to 170, B.Cu to F.Cu) and the hole slot keep their order, and
// so do the sub-slots within each.
std::uint32_t highlight_order(std::uint32_t order);

constexpr std::uint32_t kClassRouted = 10;    // rv_object_class RV_OBJ_ROUTED
constexpr std::uint32_t kClassWavefront = 11; // rv_object_class RV_OBJ_WAVEFRONT
constexpr std::uint32_t kKeyBase = 0x80000000u; // overlay keys never collide with board keys

// Slot kinds; values match RV_SLOT_* in rv.h.  A Wavefront slot holds no
// batches: its content is the wavefront image of its router layer
// (wavefront.hpp), drawn at order_begin.
enum class SlotKind : std::uint32_t { Routed = 0, Drills = 1, Markers = 2, Wavefront = 3, Dim = 4, Highlight = 5 };

// One router layer of a session as the overlay and the wavefront see it.
struct RouterLayer {
    std::string name;
    std::uint32_t index = kNone32; // board layer index (kNone32 if the board lacks it)
    std::uint32_t rank = 0;        // draw rank
    Rgba color = 0;                // theme colour
    bool visible = true;
};

// The router layers of a session: their names, board indices, ranks,
// colours and visibility.  Returns 1 when both have the same name (then
// only layers[0] is meaningful), else 2.
int router_layers(const Trace &t, std::uint32_t session, const DisplayList *board, const Theme &theme,
                  const std::vector<std::pair<std::string, bool>> &layer_visible, RouterLayer layers[2]);

struct Slot {
    SlotKind kind = SlotKind::Routed;
    std::uint32_t order_begin = 0, order_end = 0; // [begin, end)
    std::uint32_t layer = kNone32;                // board layer index (Routed), else kNone32
    std::uint32_t first_batch = 0, batch_count = 0;
    std::uint32_t generation = 0;
    std::array<float, 4> bbox{{0, 0, 0, 0}};
};

// Colours of the overlay.
struct OverlayColors {
    Rgba src = rgba(60, 255, 60);
    Rgba tgt = rgba(255, 176, 0);
    Rgba path = rgba(255, 255, 255, 0.85);
    Rgba pads = rgba(255, 255, 255, 0.9);
    Rgba failure = rgba(255, 48, 48);
    Rgba via = rgba(255, 255, 255);
    double ghost_alpha = 0.75; // alpha of a fresh ghost
    double ghost_white = 0.5;  // ghosts are the layer colour mixed this far toward white
    double hilite_white = 0.6; // the current commit, mixed this far toward white
    // M5: the net highlight.  Everything below the dim slot is mixed this far
    // toward the background (a rectangle of the background at this alpha);
    // the selected net's copper is drawn again above it, its colour mixed
    // this far toward white (alpha kept; holes keep their colours).
    double dim_alpha = 0.65;
    double select_white = 0.3;
};

// Ghost alpha is quantised to this many levels, so the overlay changes at
// most this many times while a ghost fades.
constexpr int kGhostLevels = 16;
inline int ghost_level(double alpha) {
    const int l = static_cast<int>(alpha * kGhostLevels + 0.999999);
    return l < 1 ? 1 : (l > kGhostLevels ? kGhostLevels : l);
}

Rgba mix_white(Rgba c, double f);

struct OverlayInput {
    const Trace *trace = nullptr;
    const Timeline *timeline = nullptr;
    const std::vector<CopperGeom> *geometry = nullptr;
    const DisplayList *board = nullptr; // the static list drawn (for layer indices and ranks)
    const Theme *theme = nullptr;
    // visibility
    bool routed_visible = true;                   // class RV_OBJ_ROUTED
    std::vector<std::pair<std::string, bool>> layer_visible; // router layer name -> shown
    // position
    double time = 0;
    std::uint32_t event = kNone32;
    bool suppress_routed = false; // the board drawn already holds this routing
    bool settled = false;         // a search event in its settled part (playback.hpp)
    // M5: the net highlight.  Active: dim everything and draw the selection.
    bool select_active = false;
    bool select_has_net = false;   // a router net is selected: its copper is highlighted
    std::int32_t select_net = 0;
    const DisplayList *select_board = nullptr; // the board's copper of the selected net (build_net_list)
    std::uint32_t select_build = 0;            // changes whenever select_board is rebuilt
    std::vector<bool> board_layer_visible;     // by board layer index, for select_board
    std::uint32_t classes = 0;                 // visible object classes, bit per class
    std::array<float, 4> dim_rect{{0, 0, 0, 0}}; // what the dim rectangle covers, mm
};

class Overlay {
public:
    // Forget keys and slot generations (a new timeline).
    void reset();
    void build(const OverlayInput &in);

    const DisplayList &list() const { return list_; }
    const std::vector<Slot> &slots() const { return slots_; }
    std::uint32_t generation() const { return generation_; }
    // Changes when keys are forgotten.
    std::uint32_t key_epoch() const { return key_epoch_; }

    OverlayColors colors;

private:
    std::uint32_t key_for(const std::array<std::uint64_t, 4> &ident);

    DisplayList list_;
    std::vector<Slot> slots_;
    std::map<std::array<std::uint64_t, 4>, std::uint32_t> keys_;
    std::uint32_t next_key_ = kKeyBase;
    std::map<std::uint32_t, std::pair<std::vector<std::pair<std::uint32_t, Rgba>>, std::uint32_t>> slot_sigs_;
    std::uint32_t slot_gen_ = 0;
    std::uint32_t generation_ = 0;
    std::uint32_t key_epoch_ = 1;
};

} // namespace rv

#endif // RV_OVERLAY_HPP

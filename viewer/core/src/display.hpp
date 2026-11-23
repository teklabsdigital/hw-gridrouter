// display.hpp: the board display list.
//
// A BRD chunk becomes a list of batches of five primitives, in board mm,
// sorted bottom to top (rv.h describes the primitives).  The full list is
// built once per board and theme; visibility only selects which batches are
// handed to the client, so toggling a layer never rebuilds geometry.
#ifndef RV_DISPLAY_HPP
#define RV_DISPLAY_HPP

#include "records.hpp"
#include "theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rv {

// Values match rv_prim in rv.h.
enum class Prim : std::uint32_t { Fill = 1, Stroke = 2, Segs = 3, Disc = 4, Ring = 5 };

// Object classes; values match rv_object_class in rv.h.
enum class ObjClass : std::uint32_t {
    Zones = 0, Tracks, Pads, Vias, Holes, Text, FpGraphics, Graphics, RuleAreas, Outline,
    Routed, // router copper and its ghosts, in the overlay (M3)
};
constexpr std::uint32_t kClassCount = 11;
const char *class_name(std::uint32_t cls) noexcept;

// Sub-slots inside one layer's rank (order = rank * 256 + sub).  Values
// match RV_SUB_* in rv.h.  Wavefront (M4) is a reserved slot; Routed is
// filled by the overlay (overlay.hpp), never by the board list.
namespace sub {
constexpr std::uint32_t kWavefront = 0x08;
constexpr std::uint32_t kZone = 0x10;
constexpr std::uint32_t kZoneOutline = 0x14;
constexpr std::uint32_t kRuleArea = 0x18;
constexpr std::uint32_t kGraphic = 0x20;
constexpr std::uint32_t kText = 0x28;
constexpr std::uint32_t kTrack = 0x30;
constexpr std::uint32_t kRouted = 0x38;
constexpr std::uint32_t kPad = 0x40;
constexpr std::uint32_t kVia = 0x50;
constexpr std::uint32_t kOutline = 0x60;
// in the hole slot
constexpr std::uint32_t kHoleWall = 0x10;
constexpr std::uint32_t kPadHole = 0x20;
constexpr std::uint32_t kViaHole = 0x28;
constexpr std::uint32_t kNonPlated = 0x30;
} // namespace sub

constexpr std::uint32_t order_key(std::uint32_t rank, std::uint32_t s) { return rank * 256u + s; }

struct Batch {
    Prim prim = Prim::Fill;
    std::uint32_t cls = 0;
    std::uint32_t layer = kNone32;  // BRD layer index, kNone32 for the hole slot
    std::uint32_t order = 0;        // ascending = bottom to top
    Rgba color = 0;
    std::uint32_t key = 0;          // position in the full list; stable per geometry generation
    float width = 0;                // Stroke, Segs, Ring: line width, mm
    std::uint32_t first = 0, count = 0;
    std::array<float, 4> bbox{{0, 0, 0, 0}}; // min x, min y, max x, max y, mm
    // Classes that must all be visible for the batch to be drawn (bit k =
    // class k); includes cls.
    std::uint32_t needs = 0;
};

// One layer of the board, for the Appearance panel and visibility.
struct LayerEntry {
    std::uint32_t index = 0; // position in the BRD layer table
    std::string name;
    LayerClass cls;
    Rgba color = 0;
    std::uint32_t rank = 0;       // draw rank
    std::uint32_t panel = 0;      // Appearance panel rank
    std::uint32_t items = 0;      // drawable records on this layer
    std::int32_t kicad_id = 0, type = 0;
};

struct DisplayList {
    std::vector<Batch> batches;          // all batches, sorted by order
    std::vector<float> xy;               // 2 per point
    std::vector<std::uint32_t> contours; // 2 per contour: first point, point count
    std::vector<std::uint32_t> polys;    // 2 per polygon: first contour, contour count
    std::vector<float> segs;             // 4 per segment
    std::vector<float> circles;          // 3 per circle: x, y, r
    std::array<float, 4> extent{{0, 0, 0, 0}};  // board outline, else all content
    std::array<float, 4> content{{0, 0, 0, 0}}; // all content
    bool has_extent = false;
    std::vector<LayerEntry> layers;      // in Appearance panel order
};

// Build the full display list of one board.
DisplayList build_display_list(const BoardInfo &info, const BoardGeometry &geo, const Theme &theme);

// Signed area term of a closed contour, sum of x_i*y_{i+1} - x_{i+1}*y_i
// (twice the area).  Positive for the outline of a filled polygon in the
// display list, negative for its holes.
double contour_area2(const float *xy, std::uint32_t npts);

} // namespace rv

#endif // RV_DISPLAY_HPP

// theme.hpp: KiCad layer names, the "KiCad Default" colour theme, and the
// board draw order.
//
// Colours are KiCad 10.0.8's builtin theme "_builtin_default", read from a
// running pcbnew through COLOR_SETTINGS::GetColor (see viewer/README.md,
// "Theme").  Layers are keyed by their canonical KiCad name, never by id:
// KiCad 10 renumbered the ids, and both the KiCad 5-9 short names
// ("F.SilkS", "F.CrtYd", "Dwgs.User") and the KiCad 10 long names
// ("F.Silkscreen", "F.Courtyard", "User.Drawings") are accepted.
#ifndef RV_THEME_HPP
#define RV_THEME_HPP

#include <cstdint>
#include <string>

namespace rv {

// 0xRRGGBBAA, straight (not premultiplied) alpha.
using Rgba = std::uint32_t;

constexpr Rgba rgba(unsigned r, unsigned g, unsigned b, double a = 1.0) {
    return (static_cast<Rgba>(r & 0xFF) << 24) | (static_cast<Rgba>(g & 0xFF) << 16) |
           (static_cast<Rgba>(b & 0xFF) << 8) | static_cast<Rgba>(a * 255.0 + 0.5);
}

// Multiply the alpha of a colour by f (0..1).
Rgba with_opacity(Rgba c, double f);

// Values match RV_LAYER_* in rv.h.
enum class LayerKind : std::uint32_t {
    Other = 0, Copper, Adhesive, Paste, Silk, Mask, Courtyard, Fab, EdgeCuts, Margin, User,
};
// Values match RV_SIDE_* in rv.h.
enum class Side : std::uint32_t { None = 0, Front, Back, Inner };

struct LayerClass {
    LayerKind kind = LayerKind::Other;
    Side side = Side::None;
    // Copper: 0 for F.Cu, n for In<n>.Cu, 1000 for B.Cu.  User layers: 1 to
    // 4 for Drawings, Comments, Eco1, Eco2, 100 + n for User.<n>.
    int number = 0;
};

// Classify a canonical KiCad layer name.  Unknown names give kind Other.
LayerClass classify_layer(const std::string &name);

// Draw rank of a board layer: bottom to top, as KiCad shows a board with
// F.Cu active, except that silkscreen and the other front technical
// layers are drawn above all copper (see draw_rank in theme.cpp):
//   back technical (B.CrtYd ... B.Mask) < B.Cu < In30.Cu ... In1.Cu < F.Cu
//   < front technical (F.Mask ... F.CrtYd) < user layers < Margin
//   < holes < Edge.Cuts.
// Ranks are spaced so that global slots (holes, highlight) fit between.
std::uint32_t draw_rank(const LayerClass &c);

// Rank of the global hole slot (above every layer except Edge.Cuts) and of
// the topmost slot reserved for overlays (net highlight, M5).
constexpr std::uint32_t kRankHoles = 900;
constexpr std::uint32_t kRankOverlay = 1000;

// Position in KiCad's Appearance panel, top to bottom: copper front to
// back, then the technical pairs, Edge.Cuts, Margin, courtyards, fab, user.
std::uint32_t panel_rank(const LayerClass &c);

// The KiCad Default theme.
struct Theme {
    Rgba background = rgba(0, 16, 35);
    Rgba via_hole = rgba(227, 183, 46);
    Rgba via_hole_wall = rgba(236, 236, 236);
    Rgba plated_hole = rgba(194, 194, 0);
    Rgba non_plated_hole = rgba(26, 196, 210);
    // Appearance-panel opacities (KiCad's project defaults).
    double zone_opacity = 0.6;
    double track_opacity = 1.0;
    double via_opacity = 1.0;
    double pad_opacity = 1.0;
    // Hole wall (plating) thickness drawn around plated holes, mm.
    double hole_wall_mm = 0.02;

    // Colour of a board layer, by canonical name.
    Rgba layer_color(const std::string &name) const;
};

// Visible by default?  Masks, paste and courtyards are hidden (plan B4);
// everything else is shown.
bool default_layer_visible(const LayerClass &c);

} // namespace rv

#endif // RV_THEME_HPP

// theme.cpp: KiCad layer names, the KiCad Default theme and the draw order.
//
// Every colour below was read from KiCad 10.0.8 (pcbnew's builtin theme
// "_builtin_default", COLOR_SETTINGS::GetColor), not typed from memory.

#include "theme.hpp"

#include <cmath>
#include <cstdlib>

namespace rv {

Rgba with_opacity(Rgba c, double f) {
    if (f >= 1.0) return c;
    if (f <= 0.0) return c & 0xFFFFFF00u;
    const double a = static_cast<double>(c & 0xFF) * f;
    return (c & 0xFFFFFF00u) | static_cast<Rgba>(std::lround(a));
}

namespace {

bool starts_with(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool ends_with(const std::string &s, const char *suffix) {
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

// Parses the decimal number in s[from, to); -1 if it is not one.
int number_in(const std::string &s, std::size_t from, std::size_t to) {
    if (from >= to || to > s.size() || to - from > 4) return -1;
    int n = 0;
    for (std::size_t k = from; k < to; ++k) {
        if (s[k] < '0' || s[k] > '9') return -1;
        n = n * 10 + (s[k] - '0');
    }
    return n;
}

LayerKind technical_kind(const std::string &rest) {
    if (rest == "Adhes" || rest == "Adhesive") return LayerKind::Adhesive;
    if (rest == "Paste") return LayerKind::Paste;
    if (rest == "SilkS" || rest == "Silkscreen") return LayerKind::Silk;
    if (rest == "Mask") return LayerKind::Mask;
    if (rest == "CrtYd" || rest == "Courtyard") return LayerKind::Courtyard;
    if (rest == "Fab") return LayerKind::Fab;
    return LayerKind::Other;
}

} // namespace

LayerClass classify_layer(const std::string &name) {
    LayerClass c;
    if (name == "F.Cu") return {LayerKind::Copper, Side::Front, 0};
    if (name == "B.Cu") return {LayerKind::Copper, Side::Back, 1000};
    if (starts_with(name, "In") && ends_with(name, ".Cu")) {
        const int n = number_in(name, 2, name.size() - 3);
        if (n >= 1) return {LayerKind::Copper, Side::Inner, n};
        return c;
    }
    if (starts_with(name, "F.") || starts_with(name, "B.")) {
        const LayerKind k = technical_kind(name.substr(2));
        if (k != LayerKind::Other) return {k, name[0] == 'F' ? Side::Front : Side::Back, 0};
        return c;
    }
    if (name == "Edge.Cuts") return {LayerKind::EdgeCuts, Side::None, 0};
    if (name == "Margin") return {LayerKind::Margin, Side::None, 0};
    if (name == "Dwgs.User" || name == "User.Drawings") return {LayerKind::User, Side::None, 1};
    if (name == "Cmts.User" || name == "User.Comments") return {LayerKind::User, Side::None, 2};
    if (name == "Eco1.User" || name == "User.Eco1") return {LayerKind::User, Side::None, 3};
    if (name == "Eco2.User" || name == "User.Eco2") return {LayerKind::User, Side::None, 4};
    if (starts_with(name, "User.")) {
        const int n = number_in(name, 5, name.size());
        if (n >= 1) return {LayerKind::User, Side::None, 100 + n};
    }
    return c;
}

std::uint32_t draw_rank(const LayerClass &c) {
    const bool back = c.side == Side::Back;
    switch (c.kind) {
    case LayerKind::Copper:
        if (c.side == Side::Front) return 170;
        if (back) return 100;
        return 100 + static_cast<std::uint32_t>(c.number < 63 ? 64 - c.number : 1); // In1 highest
    case LayerKind::Courtyard: return back ? 10 : 205;
    case LayerKind::Fab: return back ? 11 : 204;
    case LayerKind::Silk: return back ? 12 : 203;
    case LayerKind::Paste: return back ? 13 : 202;
    case LayerKind::Adhesive: return back ? 14 : 201;
    case LayerKind::Mask: return back ? 15 : 200;
    case LayerKind::Other: return 300;
    case LayerKind::User:
        if (c.number >= 100) return 400 - static_cast<std::uint32_t>(c.number - 100 < 90 ? c.number - 100 : 90);
        return 400 + static_cast<std::uint32_t>(5 - c.number); // Eco2 401 ... Drawings 404
    case LayerKind::Margin: return 500;
    case LayerKind::EdgeCuts: return 950;
    }
    return 300;
}

std::uint32_t panel_rank(const LayerClass &c) {
    const std::uint32_t back = c.side == Side::Back ? 1 : 0;
    switch (c.kind) {
    case LayerKind::Copper: return static_cast<std::uint32_t>(c.number);
    case LayerKind::Adhesive: return 2000 + back;
    case LayerKind::Paste: return 2002 + back;
    case LayerKind::Silk: return 2004 + back;
    case LayerKind::Mask: return 2006 + back;
    case LayerKind::User:
        if (c.number < 100) return 2007 + static_cast<std::uint32_t>(c.number); // 2008 ... 2011
        return 3000 + static_cast<std::uint32_t>(c.number);
    case LayerKind::EdgeCuts: return 2012;
    case LayerKind::Margin: return 2013;
    case LayerKind::Courtyard: return 2014 + back;
    case LayerKind::Fab: return 2016 + back;
    case LayerKind::Other: return 5000;
    }
    return 5000;
}

bool default_layer_visible(const LayerClass &c) {
    return c.kind != LayerKind::Mask && c.kind != LayerKind::Paste && c.kind != LayerKind::Courtyard;
}

Rgba Theme::layer_color(const std::string &name) const {
    // In1.Cu ... In30.Cu, from KiCad 10.0.8.  KiCad's theme stops at In30.
    static const Rgba inner[30] = {
        rgba(127, 200, 127), rgba(206, 125, 44), rgba(79, 203, 203), rgba(219, 98, 139),
        rgba(167, 165, 198), rgba(40, 204, 217), rgba(232, 178, 167), rgba(242, 237, 161),
        rgba(141, 203, 129), rgba(237, 124, 51), rgba(91, 195, 235), rgba(247, 111, 142),
        rgba(167, 165, 198), rgba(40, 204, 217), rgba(232, 178, 167), rgba(242, 237, 161),
        rgba(237, 124, 51), rgba(91, 195, 235), rgba(247, 111, 142), rgba(167, 165, 198),
        rgba(40, 204, 217), rgba(232, 178, 167), rgba(242, 237, 161), rgba(237, 124, 51),
        rgba(91, 195, 235), rgba(247, 111, 142), rgba(167, 165, 198), rgba(40, 204, 217),
        rgba(232, 178, 167), rgba(242, 237, 161),
    };
    // User.1 ... User.9; from User.10 KiCad cycles through four colours.
    static const Rgba user[9] = {
        rgba(194, 194, 194), rgba(89, 148, 220), rgba(180, 219, 210), rgba(216, 200, 82),
        rgba(194, 194, 194), rgba(89, 148, 220), rgba(180, 219, 210), rgba(216, 200, 82),
        rgba(232, 178, 167),
    };
    static const Rgba user_cycle[4] = {rgba(89, 148, 220), rgba(180, 219, 210), rgba(216, 200, 82),
                                       rgba(194, 194, 194)};

    const LayerClass c = classify_layer(name);
    const bool back = c.side == Side::Back;
    switch (c.kind) {
    case LayerKind::Copper:
        if (c.side == Side::Front) return rgba(200, 52, 52);
        if (back) return rgba(77, 127, 196);
        return inner[(c.number - 1) % 30];
    case LayerKind::Adhesive: return back ? rgba(0, 0, 132) : rgba(132, 0, 132);
    case LayerKind::Paste: return back ? rgba(0, 194, 194, 0.9) : rgba(180, 160, 154, 0.9);
    case LayerKind::Silk: return back ? rgba(232, 178, 167) : rgba(242, 237, 161);
    case LayerKind::Mask: return back ? rgba(2, 255, 238, 0.4) : rgba(216, 100, 255, 0.4);
    case LayerKind::Courtyard: return back ? rgba(38, 233, 255) : rgba(255, 38, 226);
    case LayerKind::Fab: return back ? rgba(88, 93, 132) : rgba(175, 175, 175);
    case LayerKind::EdgeCuts: return rgba(208, 210, 205);
    case LayerKind::Margin: return rgba(255, 38, 226);
    case LayerKind::User:
        switch (c.number) {
        case 1: return rgba(194, 194, 194); // User.Drawings
        case 2: return rgba(89, 148, 220);  // User.Comments
        case 3: return rgba(180, 219, 210); // User.Eco1
        case 4: return rgba(216, 200, 82);  // User.Eco2
        default: {
            const int n = c.number - 100;
            if (n >= 1 && n <= 9) return user[n - 1];
            return user_cycle[(n - 10) % 4];
        }
        }
    case LayerKind::Other: return rgba(194, 194, 194); // as User.Drawings
    }
    return rgba(194, 194, 194);
}

} // namespace rv

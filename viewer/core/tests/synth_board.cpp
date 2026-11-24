// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// synth_board.cpp: the M2 stress board of the synthetic writer (synth.hpp).
//
// Like synth.cpp it shares no code with the reader.  Coordinates are board
// nm; the board sits at (100, 50) mm, where KiCad puts boards on a sheet.

#include "synth.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace synth {

namespace {

using Pt = std::pair<std::int32_t, std::int32_t>;
using Contour = std::vector<Pt>;
using Poly = std::vector<Contour>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kMm = 1e6;

std::int32_t nm(double v) { return static_cast<std::int32_t>(std::llround(v)); }

// --- stroke font: 4 x 6 units, y down, one string of polylines per glyph -----
// Each polyline is a run of two-digit points "xy" separated by spaces;
// polylines are separated by '|'.
const char *glyph(char c) {
    switch (c) {
    case '0': return "10 30 41 45 36 16 05 01 10|05 41";
    case '1': return "11 20 26|16 36";
    case '2': return "01 10 30 41 42 06 46";
    case '3': return "01 10 30 41 42 33 13|33 44 45 36 16 05";
    case '4': return "36 30 04 44";
    case '5': return "40 00 03 33 44 45 36 06";
    case '6': return "41 30 10 01 05 16 36 45 44 33 03";
    case '7': return "00 40 41 16";
    case '8': return "13 02 01 10 30 41 42 33 13 04 05 16 36 45 44 33";
    case '9': return "05 16 36 45 41 30 10 01 02 13 43";
    case 'A': return "06 02 20 42 46|03 43";
    case 'B': return "00 30 41 42 33 03|33 44 45 36 06 00";
    case 'C': return "41 30 10 01 05 16 36 45";
    case 'D': return "00 30 41 45 36 06 00";
    case 'E': return "40 00 06 46|03 33";
    case 'F': return "40 00 06|03 33";
    case 'G': return "41 30 10 01 05 16 36 45 43 23";
    case 'H': return "00 06|40 46|03 43";
    case 'I': return "10 30|20 26|16 36";
    case 'J': return "40 45 36 16 05";
    case 'K': return "00 06|40 03 46";
    case 'L': return "00 06 46";
    case 'M': return "06 00 23 40 46";
    case 'N': return "06 00 46 40";
    case 'O': return "10 30 41 45 36 16 05 01 10";
    case 'P': return "06 00 30 41 42 33 03";
    case 'Q': return "10 30 41 45 36 16 05 01 10|24 46";
    case 'R': return "06 00 30 41 42 33 03|23 46";
    case 'S': return "41 30 10 01 02 13 33 44 45 36 16 05";
    case 'T': return "00 40|20 26";
    case 'U': return "00 05 16 36 45 40";
    case 'V': return "00 26 40";
    case 'W': return "00 16 23 36 40";
    case 'X': return "00 46|40 06";
    case 'Y': return "00 23 40|23 26";
    case 'Z': return "00 40 06 46";
    case '-': return "13 33";
    case '+': return "13 33|22 24";
    case '.': return "25 26";
    case '/': return "06 40";
    default: return "";
    }
}

// A stadium around segment (x0, y0)-(x1, y1) with half width r, as
// TransformOvalToPolygon makes: two half circles of `steps` segments.
Contour oval(double x0, double y0, double x1, double y1, double r, int steps) {
    Contour c;
    const double a = std::atan2(y1 - y0, x1 - x0);
    for (int k = 0; k <= steps; ++k) { // around (x1, y1): from a - 90 deg to a + 90 deg
        const double t = a - kPi / 2 + kPi * k / steps;
        c.emplace_back(nm(x1 + r * std::cos(t)), nm(y1 + r * std::sin(t)));
    }
    for (int k = 0; k <= steps; ++k) { // around (x0, y0)
        const double t = a + kPi / 2 + kPi * k / steps;
        c.emplace_back(nm(x0 + r * std::cos(t)), nm(y0 + r * std::sin(t)));
    }
    return c;
}

Contour circle(double cx, double cy, double r, int n) {
    Contour c;
    for (int k = 0; k < n; ++k) {
        const double t = 2 * kPi * k / n;
        c.emplace_back(nm(cx + r * std::cos(t)), nm(cy + r * std::sin(t)));
    }
    return c;
}

// A rounded rectangle centred at (cx, cy), w x h, rotated by rot (deg,
// counter-clockwise on screen), corner radius rr; clockwise or not as
// `reverse` asks (the writer does not care; KiCad does not either).
Contour round_rect(double cx, double cy, double w, double h, double rr, double rot, bool reverse = false) {
    Contour c;
    const double ca = std::cos(rot * kPi / 180), sa = std::sin(rot * kPi / 180);
    const double hx = w / 2 - rr, hy = h / 2 - rr;
    const double corners[4][2] = {{hx, -hy}, {hx, hy}, {-hx, hy}, {-hx, -hy}};
    const double start[4] = {-kPi / 2, 0, kPi / 2, kPi};
    for (int q = 0; q < 4; ++q)
        for (int k = 0; k <= 4; ++k) {
            const double t = start[q] + (kPi / 2) * k / 4;
            const double lx = corners[q][0] + rr * std::cos(t), ly = corners[q][1] + rr * std::sin(t);
            c.emplace_back(nm(cx + lx * ca + ly * sa), nm(cy - lx * sa + ly * ca));
        }
    if (reverse) std::reverse(c.begin(), c.end());
    return c;
}

// A polyline through points given in a local frame (mm), placed at (cx,
// cy) mm and rotated by rot degrees.
Contour placed(const std::vector<std::pair<double, double>> &local, double cx, double cy, double rot) {
    Contour c;
    const double ca = std::cos(rot * kPi / 180), sa = std::sin(rot * kPi / 180);
    for (const auto &p : local)
        c.emplace_back(nm((cx + p.first * ca + p.second * sa) * kMm), nm((cy - p.first * sa + p.second * ca) * kMm));
    return c;
}

// Deterministic generator (a 64-bit LCG).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 33);
    }
    double uniform(double a, double b) { return a + (b - a) * (next() / 2147483648.0); }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
};

struct BoardBuilder {
    WBrd b;
    // layer indices
    int f_cu = 0, in1 = 0, in2 = 0, b_cu = 0, f_mask = 0, b_mask = 0, f_paste = 0, b_paste = 0, f_silk = 0,
        b_silk = 0, f_crt = 0, b_crt = 0, f_fab = 0, b_fab = 0, edge = 0, dwgs = 0;

    int layer(std::int32_t id, std::int32_t type, const std::string &name) {
        b.layers.push_back({id, type, name});
        return static_cast<int>(b.layers.size() - 1);
    }
    void polyset(int layer_index, std::int32_t net, std::int32_t fp, std::uint32_t role, std::uint32_t sub,
                 std::vector<Poly> polys) {
        WPolyset p;
        p.layer_index = layer_index;
        p.net = net;
        p.fp = fp;
        p.role = role;
        p.sub = sub;
        p.polys = std::move(polys);
        b.order.emplace_back(1u, static_cast<std::uint32_t>(b.polysets.size()));
        b.polysets.push_back(std::move(p));
    }
    void hole(double x, double y, double w, double h, double rot, std::uint32_t plated, std::int32_t fp,
              std::int32_t net) {
        WHole hl;
        hl.x = nm(x * kMm);
        hl.y = nm(y * kMm);
        hl.w = nm(w * kMm);
        hl.h = nm(h * kMm);
        hl.rot_deg = rot;
        hl.plated = plated;
        hl.shape = (w == h) ? 0 : 1;
        hl.fp = fp;
        hl.net = net;
        b.order.emplace_back(2u, static_cast<std::uint32_t>(b.holes.size()));
        b.holes.push_back(hl);
    }
    void text(int layer_index, std::int32_t fp, double x, double y, double h, double angle, const std::string &s) {
        std::vector<Poly> polys;
        text_polygons(s, x * kMm, y * kMm, h * kMm, angle,
                      layer_index == b_silk || layer_index == b_fab || layer_index == b_cu, polys);
        polyset(layer_index, -1, fp, 2, 0, std::move(polys));
        WText t;
        t.layer_index = layer_index;
        t.fp = fp;
        t.x = nm(x * kMm);
        t.y = nm(y * kMm);
        t.angle_deg = angle;
        t.text = s;
        b.order.emplace_back(6u, static_cast<std::uint32_t>(b.texts.size()));
        b.texts.push_back(t);
    }
    void stroke(int layer_index, std::int32_t fp, double width_mm, std::vector<Contour> lines, std::uint32_t role = 1) {
        std::vector<Poly> polys;
        polys.push_back(Poly(lines.begin(), lines.end()));
        polyset(layer_index, -1, fp, role, static_cast<std::uint32_t>(std::llround(width_mm * kMm)), std::move(polys));
    }
    void track(int layer_index, std::int32_t net, double x0, double y0, double x1, double y1, double w) {
        WTrack t;
        t.layer_index = layer_index;
        t.net = net;
        t.x0 = nm(x0 * kMm);
        t.y0 = nm(y0 * kMm);
        t.x1 = nm(x1 * kMm);
        t.y1 = nm(y1 * kMm);
        t.w = nm(w * kMm);
        b.order.emplace_back(4u, static_cast<std::uint32_t>(b.tracks.size()));
        b.tracks.push_back(t);
    }
    void via(double x, double y, std::int32_t net) {
        WBVia v;
        v.x = nm(x * kMm);
        v.y = nm(y * kMm);
        v.dia = nm(0.6 * kMm);
        v.drill = nm(0.3 * kMm);
        v.net = net;
        v.top = f_cu;
        v.bottom = b_cu;
        b.order.emplace_back(5u, static_cast<std::uint32_t>(b.vias.size()));
        b.vias.push_back(v);
    }
};

// One pad: its copper shape, and mask and paste (SMD) or the other side's
// copper and mask and a plated hole (through hole).
void pad(BoardBuilder &bb, std::int32_t fp, std::int32_t net, const std::string &number, double x, double y,
         double w, double h, double rot, bool back, bool tht) {
    const int cu = back ? bb.b_cu : bb.f_cu, mask = back ? bb.b_mask : bb.f_mask, paste = back ? bb.b_paste : bb.f_paste;
    const double rr = 0.25 * std::min(w, h);
    auto shape = [&](double grow) {
        std::vector<Poly> polys;
        if (tht)
            polys.push_back(Poly{circle(x * kMm, y * kMm, (w / 2 + grow) * kMm, 32)});
        else
            polys.push_back(
                Poly{round_rect(x * kMm, y * kMm, (w + 2 * grow) * kMm, (h + 2 * grow) * kMm, (rr + grow) * kMm, rot)});
        return polys;
    };
    bb.polyset(cu, net, fp, 0, 0, shape(0));
    bb.polyset(mask, net, fp, 0, 0, shape(0.05));
    if (tht) {
        bb.polyset(bb.b_cu, net, fp, 0, 0, shape(0));
        bb.polyset(bb.b_mask, net, fp, 0, 0, shape(0.05));
        bb.hole(x, y, 0.6, 0.6, 0, 1, fp, net);
    } else {
        bb.polyset(paste, net, fp, 0, 0, shape(-0.02));
    }
    WPadInfo pi;
    pi.fp = fp;
    pi.net = net;
    pi.x = nm(x * kMm);
    pi.y = nm(y * kMm);
    pi.attrib = tht ? 0 : 1;
    pi.number = number;
    bb.b.order.emplace_back(3u, static_cast<std::uint32_t>(bb.b.padinfos.size()));
    bb.b.padinfos.push_back(pi);
}

} // namespace

void text_polygons(const std::string &text, double x_nm, double y_nm, double height_nm, double angle_deg,
                   bool mirror, std::vector<Poly> &polys) {
    const double unit = height_nm / 6.0; // glyph cell 4 x 6 units, advance 6
    const double pen = height_nm * 0.15;
    const double width = (static_cast<double>(text.size()) * 6.0 - 2.0) * unit;
    const double ca = std::cos(angle_deg * kPi / 180), sa = std::sin(angle_deg * kPi / 180);
    auto place = [&](double lx, double ly, double &ox, double &oy) {
        // local frame: origin at the text centre, x right, y down
        if (mirror) lx = -lx;
        ox = x_nm + lx * ca + ly * sa;
        oy = y_nm - lx * sa + ly * ca;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        const char *g = glyph(c);
        const double gx = static_cast<double>(i) * 6.0 * unit - width / 2;
        std::vector<std::pair<double, double>> line;
        for (const char *p = g;; ++p) {
            if (*p >= '0' && *p <= '9' && p[1] >= '0' && p[1] <= '9') {
                line.emplace_back(gx + (p[0] - '0') * unit, (p[1] - '0') * unit - 3.0 * unit);
                ++p;
                continue;
            }
            if (*p == '|' || *p == '\0') {
                for (std::size_t k = 0; k + 1 < line.size(); ++k) {
                    double ax, ay, bx, by;
                    place(line[k].first, line[k].second, ax, ay);
                    place(line[k + 1].first, line[k + 1].second, bx, by);
                    polys.push_back(Poly{oval(ax, ay, bx, by, pen / 2, 6)});
                }
                line.clear();
                if (*p == '\0') break;
            }
        }
    }
}

WBrd stress_board(const std::string &label, bool routed) {
    BoardBuilder bb;
    WBrd &b = bb.b;
    b.thickness_mm = 1.6;
    b.origin_x_nm = 100000000;
    b.origin_y_nm = 50000000;
    b.label = label;
    // KiCad 10 ids and GetStandardLayerName names.
    bb.f_cu = bb.layer(0, 0, "F.Cu");
    bb.in1 = bb.layer(4, 1, "In1.Cu");
    bb.in2 = bb.layer(6, 1, "In2.Cu");
    bb.b_cu = bb.layer(2, 0, "B.Cu");
    bb.f_mask = bb.layer(1, -1, "F.Mask");
    bb.b_mask = bb.layer(3, -1, "B.Mask");
    bb.f_paste = bb.layer(13, -1, "F.Paste");
    bb.b_paste = bb.layer(15, -1, "B.Paste");
    bb.f_silk = bb.layer(5, -1, "F.Silkscreen");
    bb.b_silk = bb.layer(7, -1, "B.Silkscreen");
    bb.f_crt = bb.layer(31, -1, "F.Courtyard");
    bb.b_crt = bb.layer(29, -1, "B.Courtyard");
    bb.f_fab = bb.layer(35, -1, "F.Fab");
    bb.b_fab = bb.layer(33, -1, "B.Fab");
    bb.edge = bb.layer(25, -1, "Edge.Cuts");
    bb.dwgs = bb.layer(17, -1, "User.Drawings");
    b.stackup = {{0.035, std::numeric_limits<double>::quiet_NaN(), "F.Cu"}, {0.2, 4.1, "prepreg"},
                 {0.035, std::numeric_limits<double>::quiet_NaN(), "In1.Cu"}, {1.065, 4.5, "core"},
                 {0.035, std::numeric_limits<double>::quiet_NaN(), "In2.Cu"}, {0.2, 4.1, "prepreg"},
                 {0.035, std::numeric_limits<double>::quiet_NaN(), "B.Cu"}};
    b.nets.push_back({1, "GND"});
    b.nets.push_back({2, "+3V3"});
    for (int n = 3; n <= 120; ++n) b.nets.push_back({n, "/N" + std::to_string(n)});

    const double X0 = 100, Y0 = 50, W = 100, H = 72;
    Rng rng(20261123);

    // Board outline: a rounded rectangle on Edge.Cuts, closed polyline.
    {
        Contour c = round_rect((X0 + W / 2) * kMm, (Y0 + H / 2) * kMm, W * kMm, H * kMm, 3 * kMm, 0);
        c.push_back(c.front());
        bb.stroke(bb.edge, -1, 0.05, {c}, 5);
    }
    // Mounting holes, non-plated, and their courtyards on User.Drawings.
    const double mh[4][2] = {{X0 + 4, Y0 + 4}, {X0 + W - 4, Y0 + 4}, {X0 + 4, Y0 + H - 4}, {X0 + W - 4, Y0 + H - 4}};
    for (const auto &m : mh) {
        bb.hole(m[0], m[1], 3.2, 3.2, 0, 0, -1, -1);
        bb.stroke(bb.dwgs, -1, 0.1, {circle(m[0] * kMm, m[1] * kMm, 3.0 * kMm, 48)});
    }
    // A non-plated slot.
    bb.hole(X0 + W / 2, Y0 + H - 3, 3.0, 1.2, 0, 0, -1, -1);

    // 120 footprints on a 12 x 10 grid: 30 SOIC-8, 10 five-pin headers, 80
    // 0603 passives of which 20 on the back.
    const int cols = 12, rows = 10;
    const double cw = 90.0 / cols, ch = 58.0 / rows;
    int npass = 0, nsoic = 0, nconn = 0;
    std::int32_t net = 3;
    auto next_net = [&]() {
        const std::int32_t n = net;
        net = net >= 120 ? 3 : net + 1;
        return n;
    };
    for (int k = 0; k < cols * rows; ++k) {
        const double cx = X0 + 5 + cw * (k % cols + 0.5), cy = Y0 + 8 + ch * (k / cols + 0.5);
        WFootprint f;
        const auto fp = static_cast<std::int32_t>(b.footprints.size());
        if (k % 4 == 0) { // SOIC-8
            ++nsoic;
            const double rot = (k / 4) % 3 == 1 ? 90 : 0;
            f = {rot, nm(cx * kMm), nm(cy * kMm), 0, "U" + std::to_string(nsoic), "LM358", "Package_SO:SOIC-8"};
            b.footprints.push_back(f);
            for (int p = 0; p < 8; ++p) {
                const double lx = p < 4 ? -2.475 : 2.475, ly = p < 4 ? -1.905 + 1.27 * p : 1.905 - 1.27 * (p - 4);
                const double ca = std::cos(rot * kPi / 180), sa = std::sin(rot * kPi / 180);
                pad(bb, fp, p == 3 ? 1 : next_net(), std::to_string(p + 1), cx + lx * ca + ly * sa,
                    cy - lx * sa + ly * ca, 1.5, 0.6, rot, false, false);
            }
            bb.stroke(bb.f_crt, fp, 0.05,
                      {placed({{-3.7, -2.7}, {3.7, -2.7}, {3.7, 2.7}, {-3.7, 2.7}, {-3.7, -2.7}}, cx, cy, rot)});
            bb.stroke(bb.f_fab, fp, 0.1,
                      {placed({{-1.95, -1.45}, {1.95, -1.45}, {1.95, 2.45}, {-0.95, 2.45}, {-1.95, 1.45}, {-1.95, -1.45}},
                              cx, cy, rot + 90)});
            bb.stroke(bb.f_silk, fp, 0.12,
                      {placed({{-1.95, -2.56}, {1.95, -2.56}}, cx, cy, rot),
                       placed({{-1.95, 2.56}, {1.95, 2.56}}, cx, cy, rot)});
            // pin-1 marker: a filled triangle on silk
            bb.polyset(bb.f_silk, -1, fp, 1, 0, {Poly{placed({{-3.5, -2.3}, {-3.1, -2.7}, {-3.9, -2.7}}, cx, cy, rot)}});
            bb.text(bb.f_silk, fp, cx, cy - 3.4, 0.8, 0, f.ref);
            bb.text(bb.f_fab, fp, cx, cy, 0.6, rot, f.value);
        } else if (k % 12 == 2) { // five-pin header, through hole
            ++nconn;
            f = {0, nm(cx * kMm), nm(cy * kMm), 0, "J" + std::to_string(nconn), "Conn_01x05", "Connector:Header_1x05"};
            b.footprints.push_back(f);
            for (int p = 0; p < 5; ++p)
                pad(bb, fp, p == 0 ? 1 : next_net(), std::to_string(p + 1), cx - 3.0 + 1.5 * p, cy, 1.1, 1.1, 0,
                    false, true);
            bb.stroke(bb.f_silk, fp, 0.12,
                      {placed({{-3.8, -1.0}, {3.8, -1.0}, {3.8, 1.0}, {-3.8, 1.0}, {-3.8, -1.0}}, cx, cy, 0)});
            bb.stroke(bb.f_crt, fp, 0.05,
                      {placed({{-4.0, -1.2}, {4.0, -1.2}, {4.0, 1.2}, {-4.0, 1.2}, {-4.0, -1.2}}, cx, cy, 0)});
            bb.text(bb.f_silk, fp, cx, cy - 1.9, 0.8, 0, f.ref);
        } else { // 0603 passive
            ++npass;
            const bool back = npass % 4 == 0;
            const double rot = npass % 3 == 0 ? 90 : 0;
            const bool cap = npass % 2 == 0;
            f = {rot, nm(cx * kMm), nm(cy * kMm), back ? 1u : 0u, (cap ? "C" : "R") + std::to_string(npass),
                 cap ? "100n" : "10k", cap ? "Capacitor_SMD:C_0603" : "Resistor_SMD:R_0603"};
            b.footprints.push_back(f);
            const double ca = std::cos(rot * kPi / 180), sa = std::sin(rot * kPi / 180);
            for (int p = 0; p < 2; ++p) {
                const double lx = p == 0 ? -0.8 : 0.8;
                pad(bb, fp, p == 0 ? next_net() : (npass % 5 == 0 ? 1 : next_net()), std::to_string(p + 1),
                    cx + lx * ca, cy - lx * sa, 0.9, 0.95, rot, back, false);
            }
            const int silk = back ? bb.b_silk : bb.f_silk, crt = back ? bb.b_crt : bb.f_crt;
            const int fab = back ? bb.b_fab : bb.f_fab;
            bb.stroke(silk, fp, 0.12,
                      {placed({{-0.2, -0.7}, {0.2, -0.7}}, cx, cy, rot), placed({{-0.2, 0.7}, {0.2, 0.7}}, cx, cy, rot)});
            bb.stroke(crt, fp, 0.05,
                      {placed({{-1.5, -0.75}, {1.5, -0.75}, {1.5, 0.75}, {-1.5, 0.75}, {-1.5, -0.75}}, cx, cy, rot)});
            bb.stroke(fab, fp, 0.1,
                      {placed({{-0.8, -0.4}, {0.8, -0.4}, {0.8, 0.4}, {-0.8, 0.4}, {-0.8, -0.4}}, cx, cy, rot)});
            bb.text(silk, fp, cx, cy - 1.45, 0.7, 0, f.ref);
        }
    }

    // Board texts and a silkscreen logo.
    bb.text(bb.f_silk, -1, X0 + W / 2, Y0 + 3.5, 2.0, 0, "VISUAL GRIDROUTER 2026");
    bb.text(bb.b_silk, -1, X0 + W / 2, Y0 + 3.5, 2.0, 0, "BOTTOM SIDE");
    bb.stroke(bb.f_silk, -1, 0.15, {circle((X0 + 12) * kMm, (Y0 + 3.5) * kMm, 1.6 * kMm, 40)});
    bb.stroke(bb.f_silk, -1, 0.15,
              {placed({{-1.1, 1.1}, {1.1, -1.1}}, X0 + 12, Y0 + 3.5, 0), placed({{-1.1, -1.1}, {1.1, 1.1}}, X0 + 12, Y0 + 3.5, 0)});
    // Arrow pointing right and down on User.Drawings: catches a mirrored image.
    bb.stroke(bb.dwgs, -1, 0.2, {placed({{0, 0}, {8, 0}, {6, -1.5}}, X0 + W - 20, Y0 + H - 3, 0),
                                 placed({{0, 0}, {0, 2.5}}, X0 + W - 20, Y0 + H - 3, 0)});

    // Keep-out on both outer layers.
    {
        const double kx = X0 + W - 11, ky = Y0 + 2.5;
        Contour c = placed({{0, 0}, {6, 0}, {6, 4}, {0, 4}}, kx, ky, 0);
        bb.polyset(bb.f_cu, -1, -1, 6, 0, {Poly{c}});
        bb.polyset(bb.b_cu, -1, -1, 6, 0, {Poly{c}});
    }

    if (routed) {
        // Two full-board GND fills, each with 400 clearance holes on a
        // jittered 20 x 20 grid: circles and rounded rectangles.
        for (int z = 0; z < 2; ++z) {
            const int layer = z == 0 ? bb.in1 : bb.b_cu;
            Poly poly;
            poly.push_back(round_rect((X0 + W / 2) * kMm, (Y0 + H / 2) * kMm, (W - 1) * kMm, (H - 1) * kMm,
                                      2.5 * kMm, 0));
            const double gx = (W - 4) / 20, gy = (H - 4) / 20;
            for (int j = 0; j < 20; ++j)
                for (int i = 0; i < 20; ++i) {
                    const double hx = X0 + 2 + gx * (i + 0.5) + rng.uniform(-0.15, 0.15) * gx;
                    const double hy = Y0 + 2 + gy * (j + 0.5) + rng.uniform(-0.15, 0.15) * gy;
                    const double r = rng.uniform(0.35, 0.3 * std::min(gx, gy));
                    if ((i + j + z) % 3 == 0)
                        poly.push_back(round_rect(hx * kMm, hy * kMm, 2.4 * r * kMm, 1.6 * r * kMm, 0.3 * r * kMm,
                                                  (i % 2) * 90.0, true));
                    else
                        poly.push_back(circle(hx * kMm, hy * kMm, r * kMm, 32));
                }
            bb.polyset(layer, 1, -1, 3, 0, {poly});
            Contour outline = round_rect((X0 + W / 2) * kMm, (Y0 + H / 2) * kMm, (W - 1) * kMm, (H - 1) * kMm,
                                         2.5 * kMm, 0);
            outline.push_back(outline.front());
            bb.stroke(layer, -1, 0.05, {outline}, 4);
        }
        // Tracks: 220 random 45-degree routes of 5 to 9 segments on F.Cu,
        // B.Cu and In2.Cu; vias at 80 route ends.
        const double widths[4] = {0.2, 0.25, 0.4, 0.5};
        int nvia = 0;
        for (int r = 0; r < 220; ++r) {
            const int pick = rng.below(20);
            const int layer = pick < 12 ? bb.f_cu : pick < 17 ? bb.b_cu : bb.in2;
            const double w = widths[rng.below(10) < 7 ? 0 : 1 + rng.below(3)];
            const std::int32_t rn = next_net();
            double x = rng.uniform(X0 + 6, X0 + W - 6), y = rng.uniform(Y0 + 6, Y0 + H - 6);
            const int nseg = 5 + rng.below(5);
            int dir = rng.below(8);
            for (int s = 0; s < nseg; ++s) {
                dir = (dir + 8 + rng.below(3) - 1) % 8;
                const double len = rng.uniform(1.0, 6.0);
                const double dx = std::cos(dir * kPi / 4), dy = std::sin(dir * kPi / 4);
                double nx = std::min(X0 + W - 3, std::max(X0 + 3, x + dx * len));
                double ny = std::min(Y0 + H - 3, std::max(Y0 + 3, y + dy * len));
                bb.track(layer, rn, x, y, nx, ny, w);
                x = nx;
                y = ny;
            }
            if (nvia < 80 && r % 11 != 5) {
                bb.via(x, y, rn);
                if (nvia < 40) bb.hole(x, y, 0.3, 0.3, 0, 1, -1, rn); // the same drill written as a HOLE
                ++nvia;
            }
        }
    }
    (void)npass;
    return b;
}

std::vector<unsigned char> build_stress_trace() {
    Writer w;
    WMark m;
    m.level = 0;
    m.text = "stress board";
    w.chunk("MARK", encode(m));
    w.chunk("BRD ", encode(stress_board("placed", false)));
    w.chunk("BRD ", encode(stress_board("final", true)));
    WEnd e;
    w.chunk("END ", encode(e));
    return w.out;
}

} // namespace synth

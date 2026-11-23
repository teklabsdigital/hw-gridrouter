// test_display.cpp: M2, the board in the core: theme, layer names and draw
// order, the display list, visibility, board selection and the camera.
// No rendering: everything is checked on the data the core hands out.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "camera.hpp"
#include "display.hpp"
#include "scene.hpp"
#include "theme.hpp"
#include "trace.hpp"

#include "rv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct SessionGuard {
    rv_session *s = nullptr;
    SessionGuard() { rv_create(&s); }
    ~SessionGuard() { rv_destroy(s); }
};

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// A trace holding only the given boards (and an END).
std::vector<unsigned char> boards_trace(const std::vector<synth::WBrd> &boards, bool with_end = true) {
    synth::Writer w;
    for (const synth::WBrd &b : boards) w.chunk("BRD ", synth::encode(b));
    if (with_end) w.chunk("END ", synth::encode(synth::WEnd()));
    return w.out;
}

// The sample's small board (synth.cpp sample_board): F.Cu, B.Cu, Edge.Cuts,
// F.SilkS; a pad polygon with a hole, an outline, a plated hole, a slot,
// a track, a via and a text.
synth::WBrd sample_board() {
    synth::Sample s = synth::build_sample();
    return s.brds.at(0);
}

rv::DisplayList build(const synth::WBrd &b) {
    auto t = rvtest::open_bytes(boards_trace({b}));
    auto geo = t->board_geometry(0);
    return rv::build_display_list(t->boards()[0], *geo, rv::Theme());
}

std::uint64_t count_prim(const rv::DisplayList &d, rv::Prim p) {
    std::uint64_t n = 0;
    for (const rv::Batch &b : d.batches)
        if (b.prim == p) n += b.count;
    return n;
}

} // namespace

// --- theme and layer names ---------------------------------------------------

TEST(theme_layer_names_old_and_new) {
    using rv::classify_layer;
    using rv::LayerKind;
    CHECK(classify_layer("F.Cu").kind == LayerKind::Copper);
    CHECK_EQ(classify_layer("F.Cu").number, 0);
    CHECK_EQ(classify_layer("B.Cu").number, 1000);
    CHECK_EQ(classify_layer("In1.Cu").number, 1);
    CHECK_EQ(classify_layer("In30.Cu").number, 30);
    CHECK(classify_layer("In.Cu").kind == LayerKind::Other);
    CHECK(classify_layer("Inx.Cu").kind == LayerKind::Other);
    CHECK(classify_layer("F.SilkS").kind == LayerKind::Silk);
    CHECK(classify_layer("F.Silkscreen").kind == LayerKind::Silk);
    CHECK(classify_layer("B.CrtYd").kind == LayerKind::Courtyard);
    CHECK(classify_layer("B.Courtyard").side == rv::Side::Back);
    CHECK(classify_layer("F.Adhes").kind == LayerKind::Adhesive);
    CHECK(classify_layer("F.Adhesive").kind == LayerKind::Adhesive);
    CHECK(classify_layer("Dwgs.User").kind == LayerKind::User);
    CHECK_EQ(classify_layer("User.Drawings").number, classify_layer("Dwgs.User").number);
    CHECK_EQ(classify_layer("User.Eco2").number, classify_layer("Eco2.User").number);
    CHECK_EQ(classify_layer("User.12").number, 112);
    CHECK(classify_layer("Edge.Cuts").kind == LayerKind::EdgeCuts);
    CHECK(classify_layer("Margin").kind == LayerKind::Margin);
    CHECK(classify_layer("Rescue").kind == LayerKind::Other);
    CHECK(classify_layer("").kind == LayerKind::Other);
}

// Colours from KiCad 10.0.8's builtin default theme.
TEST(theme_kicad_default_colours) {
    const rv::Theme t;
    CHECK_EQ(t.background, rv::rgba(0, 16, 35));
    CHECK_EQ(t.layer_color("F.Cu"), 0xC83434FFu);
    CHECK_EQ(t.layer_color("B.Cu"), 0x4D7FC4FFu);
    CHECK_EQ(t.layer_color("In1.Cu"), 0x7FC87FFFu);
    CHECK_EQ(t.layer_color("In2.Cu"), 0xCE7D2CFFu);
    CHECK_EQ(t.layer_color("In30.Cu"), 0xF2EDA1FFu);
    CHECK_EQ(t.layer_color("F.SilkS"), 0xF2EDA1FFu);
    CHECK_EQ(t.layer_color("F.Silkscreen"), 0xF2EDA1FFu);
    CHECK_EQ(t.layer_color("B.Silkscreen"), 0xE8B2A7FFu);
    CHECK_EQ(t.layer_color("F.Mask"), rv::rgba(216, 100, 255, 0.4));
    CHECK_EQ(t.layer_color("F.Mask") & 0xFF, 102u);
    CHECK_EQ(t.layer_color("B.Paste"), rv::rgba(0, 194, 194, 0.9));
    CHECK_EQ(t.layer_color("Edge.Cuts"), 0xD0D2CDFFu);
    CHECK_EQ(t.layer_color("F.Courtyard"), 0xFF26E2FFu);
    CHECK_EQ(t.layer_color("B.Fab"), 0x585D84FFu);
    CHECK_EQ(t.layer_color("User.1"), 0xC2C2C2FFu);
    CHECK_EQ(t.layer_color("User.9"), 0xE8B2A7FFu);
    CHECK_EQ(t.layer_color("User.10"), 0x5994DCFFu);
    CHECK_EQ(t.layer_color("User.13"), 0xC2C2C2FFu);
    CHECK_EQ(t.via_hole, 0xE3B72EFFu);
    CHECK_EQ(t.plated_hole, 0xC2C200FFu);
    CHECK_EQ(t.non_plated_hole, 0x1AC4D2FFu);
    CHECK_EQ(rv::with_opacity(0xC83434FFu, 0.6), 0xC8343499u);
    CHECK_EQ(rv::with_opacity(0xC83434FFu, 1.0), 0xC83434FFu);
    CHECK_EQ(rv::with_opacity(0xC83434FFu, 0.0), 0xC8343400u);
}

TEST(theme_default_visibility) {
    using rv::classify_layer;
    using rv::default_layer_visible;
    CHECK(default_layer_visible(classify_layer("F.Cu")));
    CHECK(default_layer_visible(classify_layer("F.Silkscreen")));
    CHECK(default_layer_visible(classify_layer("Edge.Cuts")));
    CHECK(default_layer_visible(classify_layer("F.Fab")));
    CHECK(!default_layer_visible(classify_layer("F.Mask")));
    CHECK(!default_layer_visible(classify_layer("B.Paste")));
    CHECK(!default_layer_visible(classify_layer("F.CrtYd")));
    CHECK(!default_layer_visible(classify_layer("B.Courtyard")));
}

// Bottom to top as KiCad shows a board with F.Cu active, silkscreen and the
// other front technical layers above copper, holes and Edge.Cuts on top.
TEST(draw_order_bottom_to_top) {
    const char *order[] = {"B.CrtYd", "B.Fab",  "B.SilkS", "B.Paste", "B.Adhes",      "B.Mask",
                           "B.Cu",    "In30.Cu", "In2.Cu", "In1.Cu",  "F.Cu",         "F.Mask",
                           "F.Adhes", "F.Paste", "F.SilkS", "F.Fab",  "F.CrtYd",      "User.2",
                           "User.1",  "Eco2.User", "Dwgs.User", "Margin"};
    std::uint32_t last = 0;
    for (const char *name : order) {
        const std::uint32_t r = rv::draw_rank(rv::classify_layer(name));
        if (!(r > last)) std::cerr << "    " << name << " rank " << r << " not above " << last << "\n";
        CHECK(r > last);
        last = r;
    }
    CHECK(rv::kRankHoles > last);
    CHECK(rv::draw_rank(rv::classify_layer("Edge.Cuts")) > rv::kRankHoles);
    CHECK(rv::kRankOverlay > rv::draw_rank(rv::classify_layer("Edge.Cuts")));
    // Appearance panel: copper front to back first.
    CHECK(rv::panel_rank(rv::classify_layer("F.Cu")) < rv::panel_rank(rv::classify_layer("In1.Cu")));
    CHECK(rv::panel_rank(rv::classify_layer("In2.Cu")) < rv::panel_rank(rv::classify_layer("B.Cu")));
    CHECK(rv::panel_rank(rv::classify_layer("B.Cu")) < rv::panel_rank(rv::classify_layer("F.Adhes")));
    CHECK(rv::panel_rank(rv::classify_layer("Edge.Cuts")) < rv::panel_rank(rv::classify_layer("F.CrtYd")));
}

// --- the display list --------------------------------------------------------------

TEST(display_list_of_the_sample_board) {
    const rv::DisplayList d = build(sample_board());
    // layer table in panel order: F.Cu, B.Cu, F.SilkS, Edge.Cuts
    CHECK_EQ(d.layers.size(), std::size_t(4));
    if (d.layers.size() == 4) {
        CHECK_EQ(d.layers[0].name, std::string("F.Cu"));
        CHECK_EQ(d.layers[1].name, std::string("B.Cu"));
        CHECK_EQ(d.layers[2].name, std::string("F.SilkS"));
        CHECK_EQ(d.layers[3].name, std::string("Edge.Cuts"));
        CHECK_EQ(d.layers[0].items, 3u); // pad polyset, track, via; the TEXT record is not drawn
        CHECK_EQ(d.layers[1].items, 1u); // the via
    }
    struct Want {
        rv::Prim prim;
        std::uint32_t cls, layer, order, count;
        rv::Rgba color;
        float width;
    };
    const std::uint32_t fcu = rv::draw_rank(rv::classify_layer("F.Cu")) * 256;
    const std::uint32_t bcu = rv::draw_rank(rv::classify_layer("B.Cu")) * 256;
    const std::uint32_t holes = rv::kRankHoles * 256;
    const std::uint32_t edge = rv::draw_rank(rv::classify_layer("Edge.Cuts")) * 256;
    const std::uint32_t NONE = rv::kNone32;
    const Want want[] = {
        {rv::Prim::Disc, 3, 1, bcu + rv::sub::kVia, 1, 0x4D7FC4FFu, 0},
        {rv::Prim::Segs, 1, 0, fcu + rv::sub::kTrack, 1, 0xC83434FFu, 0.2f},
        {rv::Prim::Fill, 2, 0, fcu + rv::sub::kPad, 2, 0xC83434FFu, 0},
        {rv::Prim::Disc, 3, 0, fcu + rv::sub::kVia, 1, 0xC83434FFu, 0},
        {rv::Prim::Ring, 4, NONE, holes + rv::sub::kHoleWall, 1, 0xECECECFFu, 0.02f},
        {rv::Prim::Disc, 4, NONE, holes + rv::sub::kPadHole, 1, 0xC2C200FFu, 0},
        {rv::Prim::Disc, 4, NONE, holes + rv::sub::kViaHole, 1, 0xE3B72EFFu, 0},
        {rv::Prim::Segs, 4, NONE, holes + rv::sub::kNonPlated, 1, 0x1AC4D2FFu, 0.6f},
        {rv::Prim::Stroke, 9, 2, edge + rv::sub::kOutline, 1, 0xD0D2CDFFu, 0.1f},
    };
    CHECK_EQ(d.batches.size(), sizeof want / sizeof want[0]);
    for (std::size_t k = 0; k < d.batches.size() && k < sizeof want / sizeof want[0]; ++k) {
        const rv::Batch &b = d.batches[k];
        const Want &w = want[k];
        const bool ok = b.prim == w.prim && b.cls == w.cls && b.layer == w.layer && b.order == w.order &&
                        b.count == w.count && b.color == w.color && near(b.width, w.width, 1e-6);
        if (!ok)
            std::cerr << "    batch " << k << ": prim " << static_cast<int>(b.prim) << " cls " << b.cls << " layer "
                      << b.layer << " order 0x" << std::hex << b.order << " color " << b.color << std::dec
                      << " count " << b.count << " width " << b.width << "\n";
        CHECK(ok);
        CHECK_EQ(b.key, static_cast<std::uint32_t>(k));
    }
    // Sorted bottom to top.
    for (std::size_t k = 1; k < d.batches.size(); ++k) CHECK(d.batches[k - 1].order <= d.batches[k].order);
    // Extent: the board outline (0, 0)-(5000, 4000) nm.
    CHECK(d.has_extent);
    CHECK(near(d.extent[0], 0) && near(d.extent[1], 0) && near(d.extent[2], 0.005) && near(d.extent[3], 0.004));
    // The slot: 0.6 x 1.2 mm at (102, 52) mm rotated 45 deg: a 0.6 mm wide
    // segment along the rotated y axis (sin 45, cos 45), half length 0.3.
    const rv::Batch &slot = d.batches[7];
    const float *s = &d.segs[4 * slot.first];
    const double h = 0.3 * std::sqrt(0.5);
    CHECK(near(s[0], 102 - h, 1e-4) && near(s[1], 52 - h, 1e-4) && near(s[2], 102 + h, 1e-4) &&
          near(s[3], 52 + h, 1e-4));
    // The via: a disc of radius 0.3 at (101.5, 51.5) on both copper layers.
    const float *c = &d.circles[3 * d.batches[0].first];
    CHECK(near(c[0], 101.5, 1e-4) && near(c[1], 51.5, 1e-4) && near(c[2], 0.3, 1e-6));
    // Via hole wall: ring of radius drill/2 + wall/2.
    const float *ring = &d.circles[3 * d.batches[4].first];
    CHECK(near(ring[2], 0.15 + 0.01, 1e-6));
    // Bounding boxes include the line width.
    const rv::Batch &track = d.batches[1];
    CHECK(near(track.bbox[0], 100 - 0.1, 1e-4) && near(track.bbox[2], 101 + 0.1, 1e-4));
}

// Even-odd polygons: outline positive, holes negative, whatever order the
// file gave; a repeated closing point is dropped; degenerate contours go.
TEST(display_list_orients_contours) {
    synth::WBrd b = sample_board();
    synth::WPolyset p;
    p.layer_index = 0;
    p.role = 1;
    // Outline given clockwise in the other sense (negative), with its first
    // point repeated; one hole given positive, one negative, one degenerate.
    p.polys = {{{{0, 0}, {0, 1000000}, {1000000, 1000000}, {1000000, 0}, {0, 0}},
                {{100000, 100000}, {200000, 100000}, {200000, 200000}},
                {{500000, 500000}, {500000, 600000}, {600000, 600000}},
                {{700000, 700000}, {800000, 800000}}}};
    b.polysets.push_back(p);
    b.order.emplace_back(1u, static_cast<std::uint32_t>(b.polysets.size() - 1));
    const rv::DisplayList d = build(b);
    const rv::Batch *g = nullptr;
    for (const rv::Batch &x : d.batches)
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::Graphics) && x.prim == rv::Prim::Fill) g = &x;
    CHECK(g != nullptr);
    if (g == nullptr) return;
    CHECK_EQ(g->count, 1u);
    const std::uint32_t c0 = d.polys[2 * g->first], nc = d.polys[2 * g->first + 1];
    CHECK_EQ(nc, 3u); // the two-point contour is dropped
    for (std::uint32_t c = c0; c < c0 + nc; ++c) {
        const std::uint32_t start = d.contours[2 * c], n = d.contours[2 * c + 1];
        const double a = rv::contour_area2(&d.xy[2 * start], n);
        if (c == c0) {
            CHECK_EQ(n, 4u); // closing point dropped
            CHECK(a > 0);
        } else {
            CHECK_EQ(n, 3u);
            CHECK(a < 0);
        }
    }
    // The sample pad: its hole triangle is given positive and comes out negative.
    for (const rv::Batch &x : d.batches)
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::Pads)) {
            const std::uint32_t pc = d.polys[2 * x.first];
            CHECK_EQ(d.polys[2 * x.first + 1], 2u);
            CHECK(rv::contour_area2(&d.xy[2 * d.contours[2 * pc]], d.contours[2 * pc + 1]) > 0);
            CHECK(rv::contour_area2(&d.xy[2 * d.contours[2 * pc + 2]], d.contours[2 * pc + 3]) < 0);
        }
}

TEST(display_list_roles_and_outlines) {
    synth::WBrd b = sample_board();
    auto add = [&](std::int32_t layer, std::uint32_t role, std::uint32_t sub, std::int32_t fp) {
        synth::WPolyset p;
        p.layer_index = layer;
        p.role = role;
        p.sub = sub;
        p.fp = fp;
        p.net = 1;
        p.polys = {{{{0, 0}, {2000000, 0}, {2000000, 2000000}, {0, 2000000}}}};
        b.polysets.push_back(p);
        b.order.emplace_back(1u, static_cast<std::uint32_t>(b.polysets.size() - 1));
    };
    add(0, 3, 0, -1);      // zone fill: alpha 0.6
    add(0, 4, 0, -1);      // zone outline without a width: hairline, closed
    add(1, 6, 0, -1);      // rule area on B.Cu: outline and hatching
    add(3, 2, 0, 0);       // text glyphs on F.SilkS
    add(3, 1, 150000, 1);  // footprint graphic stroke 0.15 mm
    add(3, 1, 0, -1);      // board graphic fill
    const rv::DisplayList d = build(b);
    std::map<std::uint32_t, int> by_class;
    for (const rv::Batch &x : d.batches) by_class[x.cls] += 1;
    CHECK_EQ(by_class[static_cast<std::uint32_t>(rv::ObjClass::Zones)], 2);
    CHECK_EQ(by_class[static_cast<std::uint32_t>(rv::ObjClass::RuleAreas)], 2);
    CHECK_EQ(by_class[static_cast<std::uint32_t>(rv::ObjClass::Text)], 1);
    CHECK_EQ(by_class[static_cast<std::uint32_t>(rv::ObjClass::FpGraphics)], 1);
    CHECK_EQ(by_class[static_cast<std::uint32_t>(rv::ObjClass::Graphics)], 1);
    for (const rv::Batch &x : d.batches) {
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::Zones) && x.prim == rv::Prim::Fill) {
            CHECK_EQ(x.color, 0xC8343499u);
            CHECK_EQ(x.order & 0xFF, rv::sub::kZone);
        }
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::Zones) && x.prim == rv::Prim::Stroke) {
            CHECK(near(x.width, 0.05, 1e-6));
            CHECK_EQ(d.contours[2 * x.first + 1], 5u); // closed: first point repeated
        }
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::RuleAreas) && x.prim == rv::Prim::Segs) {
            CHECK(x.count > 0);
            // every hatch segment lies inside the 2 x 2 mm square
            for (std::uint32_t k = x.first; k < x.first + x.count; ++k)
                for (int e = 0; e < 4; ++e) CHECK(d.segs[4 * k + e] >= -1e-4f && d.segs[4 * k + e] <= 2.0001f);
            CHECK_EQ(x.layer, 1u);
        }
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::FpGraphics)) {
            CHECK(x.prim == rv::Prim::Stroke);
            CHECK(near(x.width, 0.15, 1e-6));
        }
        if (x.cls == static_cast<std::uint32_t>(rv::ObjClass::Text)) {
            CHECK(x.prim == rv::Prim::Fill);
            CHECK_EQ(x.order & 0xFF, rv::sub::kText);
        }
    }
}

// The stress board: counts that the generator fixes, the drill written both
// as a VIA and a HOLE drawn once, zone holes oriented.
TEST(display_list_of_the_stress_board) {
    const rv::DisplayList d = build(synth::stress_board("final", true));
    CHECK_EQ(d.batches.size(), std::size_t(49));
    CHECK_EQ(count_prim(d, rv::Prim::Fill), std::uint64_t(5000));
    CHECK_EQ(count_prim(d, rv::Prim::Stroke), std::uint64_t(474));
    CHECK_EQ(count_prim(d, rv::Prim::Segs), std::uint64_t(1572));
    CHECK_EQ(count_prim(d, rv::Prim::Disc), std::uint64_t(454));
    CHECK_EQ(count_prim(d, rv::Prim::Ring), std::uint64_t(80));
    CHECK_EQ(d.xy.size() / 2, std::size_t(104874));
    CHECK_EQ(d.layers.size(), std::size_t(16));
    // 80 via holes, not 120: 40 vias' drills are also written as HOLE records.
    std::uint32_t via_holes = 0;
    for (const rv::Batch &b : d.batches)
        if (b.order == rv::order_key(rv::kRankHoles, rv::sub::kViaHole)) via_holes += b.count;
    CHECK_EQ(via_holes, 80u);
    // Each zone fill: one polygon, outline positive, 400 holes negative.
    int zones = 0;
    for (const rv::Batch &b : d.batches) {
        if (b.cls != static_cast<std::uint32_t>(rv::ObjClass::Zones) || b.prim != rv::Prim::Fill) continue;
        ++zones;
        CHECK_EQ(b.count, 1u);
        const std::uint32_t c0 = d.polys[2 * b.first], nc = d.polys[2 * b.first + 1];
        CHECK_EQ(nc, 401u);
        int pos = 0, neg = 0;
        for (std::uint32_t c = c0; c < c0 + nc; ++c)
            (rv::contour_area2(&d.xy[2 * d.contours[2 * c]], d.contours[2 * c + 1]) > 0 ? pos : neg) += 1;
        CHECK_EQ(pos, 1);
        CHECK_EQ(neg, 400);
    }
    CHECK_EQ(zones, 2);
    // Extent: the 100 x 72 mm outline at (100, 50) mm.
    CHECK(near(d.extent[0], 100, 1e-3) && near(d.extent[1], 50, 1e-3) && near(d.extent[2], 200, 1e-3) &&
          near(d.extent[3], 122, 1e-3));
    // Every primitive index of every batch is in range.
    for (const rv::Batch &b : d.batches) {
        switch (b.prim) {
        case rv::Prim::Fill:
            CHECK(2 * (b.first + b.count) <= d.polys.size());
            for (std::uint32_t p = b.first; p < b.first + b.count; ++p)
                CHECK(2 * (d.polys[2 * p] + d.polys[2 * p + 1]) <= d.contours.size());
            break;
        case rv::Prim::Stroke: CHECK(2 * (b.first + b.count) <= d.contours.size()); break;
        case rv::Prim::Segs: CHECK(4 * (b.first + b.count) <= d.segs.size()); break;
        case rv::Prim::Disc:
        case rv::Prim::Ring: CHECK(3 * (b.first + b.count) <= d.circles.size()); break;
        }
    }
    for (std::size_t c = 0; c < d.contours.size(); c += 2) CHECK(2 * (d.contours[c] + d.contours[c + 1]) <= d.xy.size());
}

// --- the scene: visibility and board selection ------------------------------------

TEST(scene_visibility_refilters_without_rebuilding) {
    auto t = rvtest::open_bytes(boards_trace({sample_board()}));
    rv::Scene sc;
    const std::vector<rv::Batch> all = sc.visible(t.get());
    CHECK_EQ(all.size(), std::size_t(9));
    const std::uint32_t geo = sc.geometry_generation(), gen = sc.generation();
    // Hide F.Cu: the F.Cu batches go, the B.Cu via stays.
    sc.set_layer_visible("F.Cu", false);
    std::vector<rv::Batch> v = sc.visible(t.get());
    CHECK_EQ(v.size(), std::size_t(6));
    for (const rv::Batch &b : v) CHECK(b.layer != 0u);
    CHECK_EQ(sc.geometry_generation(), geo);
    CHECK(sc.generation() != gen);
    // Keys are stable: the same key names the same batch.
    for (const rv::Batch &b : v) CHECK_EQ(all[b.key].order, b.order);
    sc.set_layer_visible("F.Cu", true);
    CHECK_EQ(sc.visible(t.get()).size(), std::size_t(9));
    // Hide vias: via discs, the via hole and its wall go; the pad hole stays.
    sc.set_class_visible(static_cast<std::uint32_t>(rv::ObjClass::Vias), false);
    v = sc.visible(t.get());
    CHECK_EQ(v.size(), std::size_t(5));
    bool pad_hole = false;
    for (const rv::Batch &b : v) {
        CHECK(b.cls != static_cast<std::uint32_t>(rv::ObjClass::Vias));
        CHECK(b.order != rv::order_key(rv::kRankHoles, rv::sub::kViaHole));
        CHECK(b.order != rv::order_key(rv::kRankHoles, rv::sub::kHoleWall));
        if (b.order == rv::order_key(rv::kRankHoles, rv::sub::kPadHole)) pad_hole = true;
    }
    CHECK(pad_hole);
    sc.set_class_visible(static_cast<std::uint32_t>(rv::ObjClass::Vias), true);
    // Hide pads: the pad and its hole go, the slot (no footprint) stays.
    sc.set_class_visible(static_cast<std::uint32_t>(rv::ObjClass::Pads), false);
    v = sc.visible(t.get());
    CHECK_EQ(v.size(), std::size_t(7));
    sc.set_class_visible(static_cast<std::uint32_t>(rv::ObjClass::Pads), true);
    // Hide holes: all four hole batches go.
    sc.set_class_visible(static_cast<std::uint32_t>(rv::ObjClass::Holes), false);
    CHECK_EQ(sc.visible(t.get()).size(), std::size_t(5));
    CHECK_EQ(sc.geometry_generation(), geo);
    bool threw = false;
    try {
        sc.set_class_visible(rv::kClassCount, false);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);
}

TEST(scene_default_visibility_hides_mask_paste_courtyard) {
    auto t = rvtest::open_bytes(boards_trace({synth::stress_board("final", true)}));
    rv::Scene sc;
    const rv::DisplayList &full = sc.full(t.get());
    std::vector<rv::Batch> v = sc.visible(t.get());
    CHECK_EQ(v.size(), std::size_t(43));
    for (const rv::Batch &b : v) {
        if (b.layer == rv::kNone32) continue;
        const rv::LayerKind k = rv::classify_layer(t->boards()[0].layers[b.layer].name).kind;
        CHECK(k != rv::LayerKind::Mask && k != rv::LayerKind::Paste && k != rv::LayerKind::Courtyard);
    }
    for (const rv::LayerEntry &le : full.layers) sc.set_layer_visible(le.name, true);
    CHECK_EQ(sc.visible(t.get()).size(), full.batches.size());
}

TEST(scene_board_selection_and_refresh) {
    rvtest::TempDir dir;
    CHECK(dir.ok());
    const std::string path = dir.path("boards.grtrace");
    // First only the "placed" board, without END.
    std::vector<unsigned char> one = boards_trace({synth::stress_board("placed", false)}, false);
    CHECK(synth::write_file(path, one));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    uint32_t sel = 7;
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, 0u);
    rv_display_list d;
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    const uint32_t geo0 = d.geometry_generation;
    for (uint32_t k = 0; k < d.batch_count; ++k) CHECK(d.batches[k].cls != RV_OBJ_TRACKS);
    // Append the "final" board: following the latest, the list switches.
    std::vector<unsigned char> two = boards_trace({synth::stress_board("placed", false),
                                                   synth::stress_board("final", true)});
    CHECK(rvtest::append_file(path, two.data() + one.size(), two.size() - one.size()));
    uint32_t added = 0;
    CHECK_EQ(rv_refresh(g.s, &added), RV_OK);
    CHECK_EQ(added, 2u);
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, 1u);
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK(d.geometry_generation != geo0);
    CHECK_EQ(d.board, 1u);
    bool tracks = false;
    for (uint32_t k = 0; k < d.batch_count; ++k) tracks = tracks || d.batches[k].cls == RV_OBJ_TRACKS;
    CHECK(tracks);
    // Select the placed board explicitly, then one that does not exist yet.
    CHECK_EQ(rv_board_select(g.s, 0), RV_OK);
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK_EQ(d.board, 0u);
    CHECK_EQ(rv_board_select(g.s, 5), RV_OK);
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, RV_NONE);
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK_EQ(d.batch_count, 0u);
    CHECK_EQ(d.board, RV_NONE);
    uint32_t nl = 9;
    CHECK_EQ(rv_layer_count(g.s, &nl), RV_OK);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(rv_board_select(g.s, RV_NONE), RV_OK);
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, 1u);
    // Boards
    uint32_t nb = 0;
    CHECK_EQ(rv_board_count(g.s, &nb), RV_OK);
    CHECK_EQ(nb, 2u);
    rv_board_info bi;
    CHECK_EQ(rv_board(g.s, 1, &bi), RV_OK);
    CHECK_STREQ(bi.label, "final");
    CHECK_EQ(bi.footprint_count, 120u);
    CHECK(near(bi.origin_x_mm, 100) && near(bi.origin_y_mm, 50));
    CHECK_EQ(rv_board(g.s, 2, &bi), RV_E_INVALID_ARGUMENT);
    // Reopening follows the latest board again.
    CHECK_EQ(rv_board_select(g.s, 0), RV_OK);
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, 1u);
}

// --- the C interface -----------------------------------------------------------------

TEST(api_board_calls_without_a_trace) {
    SessionGuard g;
    uint32_t n = 9;
    CHECK_EQ(rv_board_count(g.s, &n), RV_OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(rv_layer_count(g.s, &n), RV_OK);
    CHECK_EQ(n, 0u);
    uint32_t sel = 0;
    CHECK_EQ(rv_board_selected(g.s, &sel), RV_OK);
    CHECK_EQ(sel, RV_NONE);
    rv_display_list d;
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK_EQ(d.batch_count, 0u);
    CHECK(d.batches == nullptr);
    CHECK_EQ(d.has_extent, 0u);
    CHECK_EQ(d.background, 0x001023FFu);
    CHECK_EQ(rv_class_count(g.s, &n), RV_OK);
    CHECK_EQ(n, 11u);
    const char *name = nullptr;
    uint32_t vis = 0;
    CHECK_EQ(rv_class(g.s, RV_OBJ_ZONES, &name, &vis), RV_OK);
    CHECK_STREQ(name, "Zones");
    CHECK_EQ(vis, 1u);
    CHECK_EQ(rv_class(g.s, RV_OBJ_OUTLINE, &name, &vis), RV_OK);
    CHECK_STREQ(name, "Board outline");
    CHECK_EQ(rv_class(g.s, RV_OBJ_ROUTED, &name, &vis), RV_OK);
    CHECK_STREQ(name, "Routed copper");
    CHECK_EQ(vis, 1u);
    CHECK_EQ(rv_class(g.s, 11, &name, &vis), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_class_set_visible(g.s, 11, 0), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_layer_set_visible(g.s, nullptr, 0), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_layer_set_visible(g.s, "F.Cu", 0), RV_OK); // kept for later boards
    rv_layer_info li;
    CHECK_EQ(rv_layer(g.s, 0, &li), RV_E_INVALID_ARGUMENT);
    uint32_t key = 0;
    CHECK_EQ(rv_order_key(g.s, RV_NONE, RV_SUB_VIA_HOLE, &key), RV_OK);
    CHECK_EQ(key, rv::kRankHoles * 256 + 0x28);
    CHECK_EQ(rv_order_key(g.s, 0, RV_SUB_TRACK, &key), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_order_key(g.s, RV_NONE, 256, &key), RV_E_INVALID_ARGUMENT);
    // The camera works without a board.
    CHECK_EQ(rv_view_set_size(g.s, 800, 600, 2), RV_OK);
    CHECK_EQ(rv_view_fit(g.s), RV_OK);
    rv_view v;
    CHECK_EQ(rv_view_get(g.s, &v), RV_OK);
    CHECK(near(v.scale, 10));
    CHECK(near(v.backing, 2));
    double bx = 1, by = 1;
    CHECK_EQ(rv_view_to_board(g.s, 400, 300, &bx, &by), RV_OK);
    CHECK(near(bx, 0) && near(by, 0));
    // NULL outputs and sessions
    CHECK_EQ(rv_get_display_list(g.s, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_get_display_list(nullptr, &d), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_view_get(g.s, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_view_to_board(g.s, 0, 0, nullptr, &by), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_board_to_view(g.s, 0, 0, &bx, nullptr), RV_E_INVALID_ARGUMENT);
    const char *text = "x";
    CHECK_EQ(rv_view_status(g.s, 0, 0, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_view_status(nullptr, 0, 0, &text), RV_E_INVALID_ARGUMENT);
    CHECK(text == nullptr);
}

TEST(api_display_list_matches_the_core) {
    rvtest::TempDir dir;
    const std::string path = dir.path("stress.grtrace");
    CHECK(synth::write_file(path, synth::build_stress_trace()));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    rv_display_list d;
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK_EQ(d.batch_count, 43u);
    CHECK_EQ(d.board, 1u);
    CHECK_EQ(d.has_extent, 1u);
    CHECK(near(d.extent[2], 200, 1e-3));
    CHECK_EQ(d.point_count, 104874u); // all geometry, hidden layers included
    for (uint32_t k = 1; k < d.batch_count; ++k) CHECK(d.batches[k - 1].order <= d.batches[k].order);
    // The same pointers come back while nothing changes.
    rv_display_list again;
    CHECK_EQ(rv_get_display_list(g.s, &again), RV_OK);
    CHECK(again.batches == d.batches && again.xy == d.xy && again.generation == d.generation);
    // Layers through the interface.
    uint32_t nl = 0;
    CHECK_EQ(rv_layer_count(g.s, &nl), RV_OK);
    CHECK_EQ(nl, 16u);
    rv_layer_info li;
    CHECK_EQ(rv_layer(g.s, 0, &li), RV_OK);
    CHECK_STREQ(li.name, "F.Cu");
    CHECK_EQ(li.kind, RV_LAYER_COPPER);
    CHECK_EQ(li.side, RV_SIDE_FRONT);
    CHECK_EQ(li.rgba, 0xC83434FFu);
    CHECK_EQ(li.visible, 1u);
    CHECK_EQ(li.type, 0);
    bool mask_hidden = false;
    for (uint32_t k = 0; k < nl; ++k) {
        CHECK_EQ(rv_layer(g.s, k, &li), RV_OK);
        if (std::strcmp(li.name, "F.Mask") == 0) mask_hidden = li.visible == 0 && li.kind == RV_LAYER_MASK;
    }
    CHECK(mask_hidden);
    // Showing F.Mask adds its batch; the camera does not change the list.
    CHECK_EQ(rv_layer_set_visible(g.s, "F.Mask", 1), RV_OK);
    CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
    CHECK_EQ(d.batch_count, 44u);
    const uint32_t gen = d.generation;
    CHECK_EQ(rv_view_set_size(g.s, 1600, 1000, 2), RV_OK);
    CHECK_EQ(rv_view_fit(g.s), RV_OK);
    CHECK_EQ(rv_view_zoom(g.s, 3, 100, 100), RV_OK);
    CHECK_EQ(rv_view_pan(g.s, 10, -20), RV_OK);
    CHECK_EQ(rv_get_display_list(g.s, &again), RV_OK);
    CHECK_EQ(again.generation, gen);
    CHECK(again.batches == d.batches);
    // Order key of a layer's reserved wavefront slot sits under its zones.
    uint32_t key = 0;
    CHECK_EQ(rv_order_key(g.s, 1, RV_SUB_WAVEFRONT, &key), RV_OK); // In1.Cu
    bool below_zone = false;
    for (uint32_t k = 0; k < d.batch_count; ++k)
        if (d.batches[k].layer == 1 && d.batches[k].cls == RV_OBJ_ZONES && d.batches[k].prim == RV_FILL)
            below_zone = key < d.batches[k].order;
    CHECK(below_zone);
    // Status text
    const char *text = nullptr;
    CHECK_EQ(rv_view_fit(g.s), RV_OK);
    CHECK_EQ(rv_view_status(g.s, 800, 500, &text), RV_OK);
    CHECK_STREQ(text, "X 150.0000 mm   Y 86.0000 mm   zoom 100%");
}

// --- the camera -------------------------------------------------------------------------

TEST(camera_round_trips_and_zoom_about_a_point) {
    rv::Camera c;
    c.set_size(1600, 1000, 2);
    const std::array<float, 4> e{{100, 50, 200, 122}};
    c.fit(e, true);
    // Fit: centred, the whole extent inside with the margin.
    double vx, vy;
    c.to_view(150, 86, vx, vy);
    CHECK(near(vx, 800, 1e-6) && near(vy, 500, 1e-6));
    c.to_view(100, 50, vx, vy);
    CHECK(vx >= 0 && vy >= 0);
    c.to_view(200, 122, vx, vy);
    CHECK(vx <= 1600 && vy <= 1000);
    CHECK(near(c.scale(), std::min(1600 * 0.92 / 100, 1000 * 0.92 / 72), 1e-9));
    CHECK(near(c.fit_scale(e, true), c.scale()));
    // Round trips.
    for (double x : {0.0, 13.5, 799.25, 1600.0})
        for (double y : {0.0, 77.0, 999.0}) {
            double bx, by, x2, y2;
            c.to_board(x, y, bx, by);
            c.to_view(bx, by, x2, y2);
            CHECK(near(x, x2, 1e-9) && near(y, y2, 1e-9));
        }
    // Zoom about a point keeps the board point under it.
    double bx0, by0;
    c.to_board(300, 200, bx0, by0);
    c.zoom(4, 300, 200);
    double bx1, by1;
    c.to_board(300, 200, bx1, by1);
    CHECK(near(bx0, bx1, 1e-9) && near(by0, by1, 1e-9));
    CHECK(near(c.scale(), 4 * c.fit_scale(e, true), 1e-9));
    // tx, ty are the transform: view = board * scale + t.
    c.to_view(123, 45, vx, vy);
    CHECK(near(vx, 123 * c.scale() + c.tx(), 1e-9) && near(vy, 45 * c.scale() + c.ty(), 1e-9));
    // Pan moves content with the drag.
    c.to_view(150, 86, vx, vy);
    c.pan(25, -10);
    double vx2, vy2;
    c.to_view(150, 86, vx2, vy2);
    CHECK(near(vx2 - vx, 25, 1e-9) && near(vy2 - vy, -10, 1e-9));
    // Limits.
    c.zoom(1e9, 0, 0);
    CHECK(near(c.scale(), rv::Camera::kMaxScale));
    c.zoom(1e-12, 0, 0);
    CHECK(near(c.scale(), rv::Camera::kMinScale));
    // Resizing keeps the top-left board point.
    double tlx, tly;
    c.to_board(0, 0, tlx, tly);
    c.set_size(800, 600, 1);
    double tlx2, tly2;
    c.to_board(0, 0, tlx2, tly2);
    CHECK(near(tlx, tlx2) && near(tly, tly2));
    CHECK(near(c.backing(), 1));
}

TEST(camera_fit_before_size_and_bad_arguments) {
    rv::Camera c;
    const std::array<float, 4> e{{0, 0, 10, 10}};
    c.fit(e, true); // no size yet: remembered
    c.set_size(200, 100, 1);
    double vx, vy;
    c.to_view(5, 5, vx, vy);
    CHECK(near(vx, 100) && near(vy, 50));
    CHECK(near(c.scale(), 100 * 0.92 / 10));
    int threw = 0;
    auto expect_throw = [&](auto f) {
        try {
            f();
        } catch (const std::invalid_argument &) {
            ++threw;
        }
    };
    expect_throw([&] { c.set_size(-1, 10, 1); });
    expect_throw([&] { c.set_size(10, 10, 0); });
    expect_throw([&] { c.set_size(std::nan(""), 10, 1); });
    expect_throw([&] { c.zoom(0, 0, 0); });
    expect_throw([&] { c.zoom(-2, 0, 0); });
    expect_throw([&] { c.zoom(2, std::nan(""), 0); });
    expect_throw([&] { c.pan(std::nan(""), 0); });
    CHECK_EQ(threw, 7);
    CHECK(near(c.scale(), 100 * 0.92 / 10)); // unchanged by the failures
    SessionGuard g;
    CHECK_EQ(rv_view_set_size(g.s, -5, 1, 1), RV_E_INVALID_ARGUMENT);
    CHECK(std::strlen(rv_last_error(g.s)) > 0);
    CHECK_EQ(rv_view_zoom(g.s, 0, 0, 0), RV_E_INVALID_ARGUMENT);
}

// The recorder's sample: both boards draw, the final one with tracks and
// a via; the placed one without.
TEST(display_list_of_the_real_sample) {
    std::string here = __FILE__;
    const std::size_t slash = here.find_last_of("/\\");
    here = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
    const std::string path = here + "/data/small.grtrace";
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::cout << "    skipped: sample not present (" << path << ")\n";
        CHECK(true);
        return;
    }
    std::fclose(f);
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    uint32_t nb = 0;
    CHECK_EQ(rv_board_count(g.s, &nb), RV_OK);
    CHECK(nb >= 1);
    for (uint32_t k = 0; k < nb; ++k) {
        CHECK_EQ(rv_board_select(g.s, k), RV_OK);
        rv_display_list d;
        CHECK_EQ(rv_get_display_list(g.s, &d), RV_OK);
        CHECK(d.batch_count > 0);
        CHECK_EQ(d.has_extent, 1u);
        bool outline = false;
        for (uint32_t i = 0; i < d.batch_count; ++i) outline = outline || d.batches[i].cls == RV_OBJ_OUTLINE;
        CHECK(outline);
        rv_board_info bi;
        CHECK_EQ(rv_board(g.s, k, &bi), RV_OK);
        std::cout << "    sample board " << k << " \"" << bi.label << "\": " << d.batch_count << " batches\n";
    }
}

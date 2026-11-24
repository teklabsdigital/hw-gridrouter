// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// synth.cpp: the synthetic .grtrace writer (see synth.hpp).  Every layout
// here is taken from the field tables of docs/TRACE_FORMAT.md section 7.

#include "synth.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace synth {

// --- Bytes -------------------------------------------------------------------

Bytes &Bytes::u8(std::uint8_t v) {
    b.push_back(v);
    return *this;
}

Bytes &Bytes::u16(std::uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>(v >> 8));
    return *this;
}

Bytes &Bytes::u32(std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<unsigned char>((v >> (8 * k)) & 0xFF));
    return *this;
}

Bytes &Bytes::i32(std::int32_t v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    return u32(u);
}

Bytes &Bytes::u64(std::uint64_t v) {
    for (int k = 0; k < 8; ++k) b.push_back(static_cast<unsigned char>((v >> (8 * k)) & 0xFF));
    return *this;
}

Bytes &Bytes::i64(std::int64_t v) {
    std::uint64_t u;
    std::memcpy(&u, &v, 8);
    return u64(u);
}

Bytes &Bytes::f64(double v) {
    std::uint64_t u;
    std::memcpy(&u, &v, 8);
    return u64(u);
}

Bytes &Bytes::chars(const char *s, std::size_t n) {
    b.insert(b.end(), s, s + n);
    return *this;
}

Bytes &Bytes::str(const std::string &s) {
    u32(static_cast<std::uint32_t>(s.size()));
    chars(s.data(), s.size());
    while (b.size() % 4 != 0) b.push_back(0);
    return *this;
}

Bytes &Bytes::zeros(std::size_t n) {
    b.insert(b.end(), n, 0);
    return *this;
}

Bytes &Bytes::align8() {
    while (b.size() % 8 != 0) b.push_back(0);
    return *this;
}

Bytes &Bytes::append(const Bytes &other) {
    b.insert(b.end(), other.b.begin(), other.b.end());
    return *this;
}

// --- payloads --------------------------------------------------------------------

Bytes encode(const WRun &x) {
    Bytes p;
    p.i32(x.nx).i32(x.ny).i32(x.nd).i32(x.block).i32(x.via_cost).i32(x.rip_cost).i32(x.max_rips);
    p.i32(x.h_max).i32(x.h_min).i32(x.h_off);
    p.i32(x.r_track[0]).i32(x.r_track[1]).i32(x.r_wide[0]).i32(x.r_wide[1]);
    p.i32(x.r_via[0]).i32(x.r_via[1]).i32(x.r_via_space).u32(x.flags);
    // offset 72: twelve f64
    p.f64(x.cell).f64(x.origin_x).f64(x.origin_y).f64(x.track).f64(x.track_wide);
    p.f64(x.pad_clear[0]).f64(x.pad_clear[1]).f64(x.pad_novia);
    p.f64(x.edge_margin[0]).f64(x.edge_margin[1]).f64(x.via_dia).f64(x.via_drill);
    // offset 168
    for (std::int32_t v : x.dirs) p.i32(v);
    for (std::int32_t v : x.step) p.i32(v);
    for (std::int32_t v : x.turn) p.i32(v);
    p.str(x.producer).str(x.layer_name[0]).str(x.layer_name[1]);
    return p;
}

Bytes encode(const WNets &x) {
    Bytes p;
    p.u32(static_cast<std::uint32_t>(x.entries.size()));
    for (const auto &e : x.entries) p.i32(e.first).str(e.second);
    return p;
}

Bytes encode(const WPad &x) {
    Bytes p;
    p.i32(x.net).u32(x.layers).f64(x.power).f64(x.cx).f64(x.cy);
    p.f64(x.x0).f64(x.y0).f64(x.x1).f64(x.y1).f64(x.d_t).f64(x.d_w).f64(x.d_novia);
    return p;
}

Bytes encode(const WRect &x) {
    Bytes p;
    p.u32(x.op).i32(x.net).u32(x.layers).u32(x.levels);
    p.f64(x.x0).f64(x.y0).f64(x.x1).f64(x.y1).f64(x.d_t).f64(x.d_w);
    return p;
}

Bytes encode(const WDisc &x) {
    Bytes p;
    p.i32(x.net).i32(x.layer).i32(x.i).i32(x.j).i32(x.r_t).i32(x.r_w);
    return p;
}

Bytes encode(const WVia &x) {
    Bytes p;
    p.u32(x.copper_id).i32(x.net).i32(x.i).i32(x.j).i32(x.power).u32(x.added).u32(x.ctx).u32(x.route_id);
    return p;
}

Bytes encode_snap() {
    Bytes p;
    p.u32(0);
    return p;
}

Bytes encode(const WGrds &x) {
    Bytes p;
    p.u32(x.which).u32(x.nc).str(x.label);
    for (std::int32_t v : x.t0) p.i32(v);
    for (std::int32_t v : x.t1) p.i32(v);
    for (std::int32_t v : x.w0) p.i32(v);
    for (std::int32_t v : x.w1) p.i32(v);
    for (std::uint8_t v : x.novia) p.u8(v);
    return p;
}

Bytes encode(const WRnb &x) {
    Bytes p;
    p.u32(x.route_id).i32(x.net).u32(x.wide).i32(x.power);
    p.u32(static_cast<std::uint32_t>(x.pads.size())).u32(0);
    for (const WRnbPad &pad : x.pads) p.f64(pad.cx).f64(pad.cy).u32(pad.layers).u32(pad.ncells);
    p.str(x.name);
    for (const WRnbPad &pad : x.pads) p.str(pad.label);
    return p;
}

Bytes encode(const WRne &x) {
    Bytes p;
    p.u32(x.route_id).i32(x.net).i32(x.rips_after);
    p.u32(static_cast<std::uint32_t>(x.ripped.size())).u32(static_cast<std::uint32_t>(x.failed.size()));
    p.u32(x.status);
    for (const std::string &s : x.ripped) p.str(s);
    for (const std::string &s : x.failed) p.str(s);
    return p;
}

Bytes encode(const WSrch &x) {
    Bytes p;
    p.u32(x.search_id).u32(x.route_id).u32(x.ctx).i32(x.net).u32(x.flags);
    p.i32(x.gi).i32(x.gj).i32(x.rips_at_call);
    p.i64(x.max_cost).i64(x.cost).i64(x.expanded).f64(x.duration_s);
    p.u32(static_cast<std::uint32_t>(x.src.size()));
    p.u32(static_cast<std::uint32_t>(x.tgt.size()));
    p.u32(static_cast<std::uint32_t>(x.path.size()));
    p.u32(static_cast<std::uint32_t>(x.trace_cell.size()));
    p.u32(x.ntrace_total).u32(x.repeat_of).u32(x.enc).u32(0);
    for (std::uint32_t c : x.src) p.u32(c);
    for (std::uint32_t c : x.tgt) p.u32(c);
    for (std::uint32_t c : x.path) p.u32(c);
    if (x.enc == 0) {
        for (std::uint32_t c : x.trace_cell) p.u32(c);
        if (x.flags & (1u << 7))
            for (std::int32_t d : x.trace_d) p.i32(d);
    } else {
        for (unsigned char ch : x.enc_data) p.u8(ch);
    }
    return p;
}

Bytes encode(const WCmit &x) {
    Bytes p;
    p.u32(x.copper_id).i32(x.net).u32(x.flags).i32(x.power).u32(x.ctx).u32(x.route_id);
    p.u32(static_cast<std::uint32_t>(x.path.size()));
    p.u32(static_cast<std::uint32_t>(x.stubs.size()));
    p.u32(static_cast<std::uint32_t>(x.vias.size()));
    p.u32(0).f64(x.width_mm);
    for (const WStub &s : x.stubs) p.u32(s.layer).u32(0).f64(s.x0).f64(s.y0).f64(s.x1).f64(s.y1).f64(s.w);
    for (const auto &v : x.vias) p.i32(v.first).i32(v.second);
    for (std::uint32_t c : x.path) p.u32(c);
    return p;
}

Bytes encode(const WRbld &x) {
    Bytes p;
    p.u32(x.route_id).u32(static_cast<std::uint32_t>(x.names.size()));
    for (std::int32_t n : x.nets) p.i32(n);
    for (const std::string &s : x.names) p.str(s);
    return p;
}

Bytes encode(const WRip &x) {
    Bytes p;
    p.i32(x.net).u32(x.ctx).u32(x.route_id).u32(static_cast<std::uint32_t>(x.removed.size()));
    for (std::uint32_t id : x.removed) p.u32(id);
    return p;
}

Bytes encode(const WDrop &x) {
    Bytes p;
    p.i32(x.net).u32(x.ok).i64(x.max_cost).str(x.name).str(x.pad_label);
    return p;
}

Bytes encode(const WMark &x) {
    Bytes p;
    p.u32(x.level).str(x.text);
    return p;
}

Bytes encode(const WRout &x) {
    Bytes p;
    p.u32(static_cast<std::uint32_t>(x.segs.size())).u32(static_cast<std::uint32_t>(x.vias.size()));
    for (const WRoutSeg &s : x.segs) p.u32(s.layer).i32(s.net).f64(s.x0).f64(s.y0).f64(s.x1).f64(s.y1).f64(s.w);
    for (const WRoutVia &v : x.vias) p.f64(v.x).f64(v.y).i32(v.net).u32(0);
    return p;
}

namespace {

Bytes record_body(const WBrd &x, std::uint32_t kind, std::uint32_t index) {
    Bytes r;
    switch (kind) {
    case 1: {
        const WPolyset &s = x.polysets[index];
        r.i32(s.layer_index).i32(s.net).i32(s.fp).u32(s.role).u32(s.sub);
        r.u32(static_cast<std::uint32_t>(s.polys.size()));
        for (const auto &poly : s.polys) {
            r.u32(static_cast<std::uint32_t>(poly.size()));
            for (const auto &contour : poly) {
                r.u32(static_cast<std::uint32_t>(contour.size()));
                for (const auto &pt : contour) r.i32(pt.first).i32(pt.second);
            }
        }
        break;
    }
    case 2: {
        const WHole &h = x.holes[index];
        r.i32(h.x).i32(h.y).i32(h.w).i32(h.h).f64(h.rot_deg).u32(h.plated).u32(h.shape).i32(h.fp).i32(h.net);
        break;
    }
    case 3: {
        const WPadInfo &p = x.padinfos[index];
        r.i32(p.fp).i32(p.net).i32(p.x).i32(p.y).u32(p.attrib).str(p.number);
        break;
    }
    case 4: {
        const WTrack &t = x.tracks[index];
        r.i32(t.layer_index).i32(t.net).i32(t.x0).i32(t.y0).i32(t.x1).i32(t.y1).i32(t.w);
        break;
    }
    case 5: {
        const WBVia &v = x.vias[index];
        r.i32(v.x).i32(v.y).i32(v.dia).i32(v.drill).i32(v.net).i32(v.top).i32(v.bottom);
        break;
    }
    case 6: {
        const WText &t = x.texts[index];
        r.i32(t.layer_index).i32(t.fp).i32(t.x).i32(t.y).f64(t.angle_deg).str(t.text);
        break;
    }
    default:
        for (std::uint32_t k = 0; k < index; ++k) r.u8(static_cast<std::uint8_t>(0xA0 + k));
        break;
    }
    r.align8(); // bodies are padded to 8; nbytes includes the padding
    return r;
}

} // namespace

Bytes encode(const WBrd &x) {
    Bytes p;
    p.f64(x.thickness_mm).i32(x.origin_x_nm).i32(x.origin_y_nm).str(x.label);
    p.u32(static_cast<std::uint32_t>(x.layers.size()));
    for (const WLayer &l : x.layers) p.i32(l.kicad_id).i32(l.type).str(l.name);
    p.u32(static_cast<std::uint32_t>(x.stackup.size()));
    for (const WStackup &s : x.stackup) p.align8().f64(s.thickness_mm).f64(s.er).str(s.name);
    p.u32(static_cast<std::uint32_t>(x.nets.size()));
    for (const auto &n : x.nets) p.i32(n.first).str(n.second);
    p.u32(static_cast<std::uint32_t>(x.footprints.size()));
    for (const WFootprint &f : x.footprints)
        p.align8().f64(f.rot_deg).i32(f.x).i32(f.y).u32(f.flipped).u32(0).str(f.ref).str(f.value).str(f.fpid);
    p.u32(static_cast<std::uint32_t>(x.order.size())).align8();
    for (const auto &rec : x.order) {
        const Bytes body = record_body(x, rec.first, rec.second);
        p.u32(rec.first).u32(static_cast<std::uint32_t>(body.size())).append(body);
    }
    return p;
}

Bytes encode(const WEnd &x) {
    Bytes p;
    p.u32(x.searches).u32(x.routes).u32(static_cast<std::uint32_t>(x.failed.size())).i32(x.rips);
    p.i64(x.expanded_total).i64(x.trace_entries).f64(x.wall_s);
    for (const auto &f : x.failed) p.str(f.first).str(f.second);
    return p;
}

WRun make_run(std::int32_t nx, std::int32_t ny, std::int32_t nd) {
    WRun r;
    r.nx = nx;
    r.ny = ny;
    r.nd = nd;
    for (std::int32_t k = 0; k < nd; ++k) {
        r.dirs.push_back(k % 3 - 1);
        r.dirs.push_back((k / 3) % 3 - 1);
        r.dirs.push_back(10 + k);
    }
    for (std::int32_t k = 0; k < nd; ++k) r.step.push_back(10 + k);
    for (std::int32_t k = 0; k < nd; ++k) r.step.push_back(20 + k);
    for (std::int32_t k = 0; k < (nd + 1) * nd; ++k) r.turn.push_back(k % 5 - 1);
    return r;
}

WGrds make_grids(std::uint32_t which, std::uint32_t nc, const std::string &label) {
    WGrds g;
    g.which = which;
    g.nc = nc;
    g.label = label;
    for (std::uint32_t k = 0; k < nc; ++k) {
        g.t0.push_back(static_cast<std::int32_t>(k % 4 == 0 ? BLOCK : k % 3));
        g.t1.push_back(static_cast<std::int32_t>(k % 5));
        g.w0.push_back(static_cast<std::int32_t>(k % 7 == 0 ? BLOCK : 0));
        g.w1.push_back(-static_cast<std::int32_t>(k)); // any i32 is stored as is
        g.novia.push_back(static_cast<std::uint8_t>(k % 3)); // 2 is a legal "forbidden"
    }
    return g;
}

// --- Writer ----------------------------------------------------------------------

Writer::Writer(double open_time, std::uint16_t major, std::uint16_t minor, std::uint32_t flags) {
    Bytes h;
    h.chars("GRTRACE\0", 8).u16(major).u16(minor).u32(flags).f64(open_time);
    out = h.b;
}

void Writer::chunk(const char tag[4], const Bytes &payload) {
    chunk_at(tag, payload, t + 0.125);
}

void Writer::chunk_at(const char tag[4], const Bytes &payload, double time) {
    t = time;
    chunk_offsets.push_back(out.size());
    Bytes h;
    h.chars(tag, 4).u32(static_cast<std::uint32_t>(payload.size())).f64(time);
    out.insert(out.end(), h.b.begin(), h.b.end());
    out.insert(out.end(), payload.b.begin(), payload.b.end());
    const std::size_t pad = (8 - payload.size() % 8) % 8;
    out.insert(out.end(), pad, 0);
}

void Writer::raw(const Bytes &bytes) {
    out.insert(out.end(), bytes.b.begin(), bytes.b.end());
}

// --- the sample ------------------------------------------------------------------------

namespace {

std::uint32_t cell(const WRun &r, std::uint32_t layer, std::uint32_t i, std::uint32_t j) {
    return layer * static_cast<std::uint32_t>(r.nx * r.ny) + j * static_cast<std::uint32_t>(r.nx) + i;
}

struct Builder {
    Writer w;
    Sample s;

    void add(const char *tag, const Bytes &payload) {
        w.chunk(tag, payload);
        s.tags.emplace_back(tag, 4);
        s.times.push_back(w.t);
    }
};

WBrd sample_board(const std::string &label) {
    WBrd b;
    b.thickness_mm = 1.6;
    b.origin_x_nm = 100500000;
    b.origin_y_nm = 50250000;
    b.label = label;
    b.layers = {{0, 0, "F.Cu"}, {31, 0, "B.Cu"}, {44, -1, "Edge.Cuts"}, {37, -1, "F.SilkS"}};
    b.stackup = {{0.035, std::numeric_limits<double>::quiet_NaN(), "F.Cu"},
                 {1.51, 4.5, "dielectric 1"},
                 {0.035, std::numeric_limits<double>::quiet_NaN(), "B.Cu"}};
    b.nets = {{1, "GND"}, {2, "/SIG"}};
    b.footprints = {{90.0, 101000000, 51000000, 0, "U1", "MCU", "Package_QFP:LQFP-48"},
                    {0.0, 102000000, 52000000, 1, "J1", "Conn", "Connector:Header_1x02"}};
    WPolyset filled;
    filled.layer_index = 0;
    filled.net = 1;
    filled.fp = 0;
    filled.role = 0;
    filled.sub = 0;
    filled.polys = {{{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}}, {{200, 200}, {300, 200}, {300, 300}}},
                    {{{2000, 2000}, {2500, 2000}, {2500, 2600}}}};
    WPolyset outline;
    outline.layer_index = 2;
    outline.role = 5;
    outline.sub = 100000;
    outline.polys = {{{{0, 0}, {5000, 0}, {5000, 4000}, {0, 4000}, {0, 0}}}};
    b.polysets = {filled, outline};
    b.holes = {{101000000, 51000000, 800000, 800000, 0.0, 1, 0, 0, 1},
               {102000000, 52000000, 600000, 1200000, 45.0, 0, 1, -1, -1}};
    b.padinfos = {{0, 1, 101000000, 51000000, 1, "1"}, {1, 2, 102000000, 52000000, 0, "A3"}};
    b.tracks = {{0, 2, 100000000, 50000000, 101000000, 50000000, 200000}};
    b.vias = {{101500000, 51500000, 600000, 300000, 1, 0, 1}};
    b.texts = {{3, 0, 101000000, 50500000, 90.0, "U1"}};
    b.order = {{1, 0}, {2, 0}, {99, 12}, {3, 0}, {4, 0}, {1, 1}, {5, 0}, {6, 0}, {2, 1}, {3, 1}};
    return b;
}

} // namespace

Sample build_sample(bool with_end) {
    Builder bd;
    Sample &s = bd.s;
    s.open_time = 1795392000.25;

    WNets nets;
    nets.entries = {{1, "GND"}, {2, "/SIG"}, {3, "VCC"}};
    s.nets.push_back(nets);
    bd.add("NETS", encode(nets));

    WMark m0;
    m0.level = 0;
    m0.text = "placement";
    s.marks.push_back(m0);
    bd.add("MARK", encode(m0));

    s.brds.push_back(sample_board("placed"));
    bd.add("BRD ", encode(s.brds.back()));

    // --- session 0: 6 x 5 grid, 8 directions ---
    WRun run0 = make_run(6, 5, 8);
    run0.flags = 1u | 4u;
    run0.edge_margin[0] = 0.3;
    run0.edge_margin[1] = 0.4;
    s.runs.push_back(run0);
    bd.add("RUN ", encode(run0));

    s.grds.push_back(make_grids(0, 30, "attach"));
    bd.add("GRDS", encode(s.grds.back()));

    WPad pad;
    pad.net = 1;
    pad.layers = 3;
    pad.power = 1.5;
    pad.cx = 0.5;
    pad.cy = 0.75;
    pad.x0 = 0.4;
    pad.y0 = 0.6;
    pad.x1 = 0.6;
    pad.y1 = 0.9;
    pad.d_t = 0.2 + 0.25 * 1.5;
    pad.d_w = 0.3 + 0.25 * 1.5;
    pad.d_novia = 0.1;
    s.pads.push_back(pad);
    bd.add("PAD ", encode(pad));

    WRect r0;
    r0.op = 0;
    r0.net = 2;
    r0.layers = 1;
    r0.levels = 3;
    r0.x0 = 0.25;
    r0.y0 = 0.25;
    r0.x1 = 0.5;
    r0.y1 = 0.5;
    r0.d_t = 0.1;
    r0.d_w = 0.15;
    WRect r1;
    r1.op = 1;
    r1.net = BLOCK;
    r1.layers = 3;
    r1.levels = 3;
    r1.x0 = 1.0;
    r1.y0 = 0.0;
    r1.x1 = 1.25;
    r1.y1 = 0.25;
    WRect r2;
    r2.op = 2;
    r2.net = BLOCK;
    r2.layers = 2;
    r2.levels = 1;
    r2.x0 = 4;
    r2.y0 = 3;
    r2.x1 = 5;
    r2.y1 = 4;
    for (const WRect &r : {r0, r1, r2}) {
        s.rects.push_back(r);
        bd.add("RECT", encode(r));
    }

    WDisc disc;
    disc.net = 3;
    disc.layer = 1;
    disc.i = 2;
    disc.j = 3;
    disc.r_t = 1;
    disc.r_w = 2;
    s.discs.push_back(disc);
    bd.add("DISC", encode(disc));

    WVia v0; // copper 0
    v0.copper_id = 0;
    v0.net = 3;
    v0.i = 4;
    v0.j = 1;
    v0.power = 1;
    v0.added = 1;
    WVia v1; // the net already has a via there: no copper
    v1.copper_id = NONE32;
    v1.net = 3;
    v1.i = 4;
    v1.j = 1;
    v1.added = 0;
    for (const WVia &v : {v0, v1}) {
        s.vias.push_back(v);
        bd.add("VIA ", encode(v));
    }

    // An unknown tag from a later minor version: skipped.
    Bytes opaque;
    opaque.u32(0xDEADBEEF).str("future").u8(7);
    bd.add("XTRA", opaque);
    ++s.unknown_chunks;

    // route_net 0 on net 2, not wide: attempt 1 fails, attempt 2 repeats it,
    // then a commit.  (Not physically meaningful; only formally valid.)
    WRnb rnb0;
    rnb0.route_id = 0;
    rnb0.net = 2;
    rnb0.wide = 0;
    rnb0.power = 0;
    rnb0.name = "/SIG";
    rnb0.pads = {{0.25, 0.25, 1, 2, "U1.4"}, {1.0, 1.0, 3, 1, "J1.1"}};
    s.rnbs.push_back(rnb0);
    bd.add("RNB ", encode(rnb0));

    WSrch s0;
    s0.search_id = 0;
    s0.route_id = 0;
    s0.ctx = 1;
    s0.net = 2;
    s0.flags = (1u << 1) | (1u << 7) | (1u << 9); // has_goal, has_costs, has_trace
    s0.gi = 4;
    s0.gj = 4;
    s0.expanded = 15;
    s0.duration_s = 0.0015;
    s0.src = {cell(run0, 0, 1, 1), cell(run0, 1, 1, 1)};
    s0.tgt = {cell(run0, 0, 4, 4), cell(run0, 0, 4, 4)}; // duplicates kept
    s0.trace_cell = {cell(run0, 0, 1, 1), cell(run0, 1, 1, 1), cell(run0, 0, 2, 1),
                     cell(run0, 0, 2, 2), cell(run0, 0, 3, 2), cell(run0, 1, 5, 4)};
    s0.trace_d = {0, 0, 10, 24, 34, 410};
    s0.ntrace_total = 6;
    s.srchs.push_back(s0);
    bd.add("SRCH", encode(s0));

    WSrch s1 = s0; // identical trace: stored as a repeat
    s1.search_id = 1;
    s1.trace_cell.clear();
    s1.trace_d.clear();
    s1.repeat_of = 0;
    s1.duration_s = 0.0012;
    s.srchs.push_back(s1);
    bd.add("SRCH", encode(s1));

    WCmit c1; // copper 1
    c1.copper_id = 1;
    c1.net = 2;
    c1.flags = 0;
    c1.ctx = 1;
    c1.route_id = 0;
    c1.width_mm = 0.2;
    c1.stubs = {{0, 0.25, 0.25, 0.3, 0.3, 0.2}};
    c1.vias = {{2, 1}};
    c1.path = {cell(run0, 0, 1, 1), cell(run0, 0, 2, 1), cell(run0, 1, 2, 1), cell(run0, 1, 3, 1)};
    s.cmits.push_back(c1);
    bd.add("CMIT", encode(c1));

    WRne rne0;
    rne0.route_id = 0;
    rne0.net = 2;
    rne0.rips_after = 0;
    rne0.status = 0;
    s.rnes.push_back(rne0);
    bd.add("RNE ", encode(rne0));

    bd.add("SNAP", encode_snap());
    s.grds.push_back(make_grids(1, 30, "snapshot"));
    bd.add("GRDS", encode(s.grds.back()));

    WCmit c2; // copper 2: a direct commit after the snapshot (ripped later)
    c2.copper_id = 2;
    c2.net = 3;
    c2.flags = 1u | 2u | 4u;
    c2.power = 1;
    c2.ctx = 0;
    c2.route_id = NONE32;
    c2.width_mm = 0.5;
    c2.vias = {{0, 0}};
    c2.path = {cell(run0, 0, 0, 0), cell(run0, 0, 0, 1), cell(run0, 1, 0, 1)};
    s.cmits.push_back(c2);
    bd.add("CMIT", encode(c2));

    // route_net 1 on net 1: a relaxed search finds a path through net 3,
    // which is rebuilt (ripped), then the commit; one pad fails.
    WRnb rnb1;
    rnb1.route_id = 1;
    rnb1.net = 1;
    rnb1.wide = 1;
    rnb1.power = 2;
    rnb1.name = "GND";
    rnb1.pads = {{0.5, 0.75, 3, 4, "U1.1"}, {1.25, 0.5, 1, 1, "J1.2"}, {0.0, 1.0, 2, 1, "J1.3"}};
    s.rnbs.push_back(rnb1);
    bd.add("RNB ", encode(rnb1));

    WSrch s2;
    s2.search_id = 2;
    s2.route_id = 1;
    s2.ctx = 1;
    s2.net = 1;
    s2.flags = 1u | (1u << 4) | (1u << 5) | (1u << 9); // wide, relax, found, has_trace; no costs
    s2.rips_at_call = 0;
    s2.max_cost = -1;
    s2.cost = 120;
    s2.expanded = 9;
    s2.duration_s = 0.0007;
    s2.src = {cell(run0, 0, 2, 3)};
    s2.tgt = {cell(run0, 0, 0, 0)};
    s2.path = {cell(run0, 0, 2, 3), cell(run0, 0, 1, 2), cell(run0, 0, 0, 1), cell(run0, 0, 0, 0)};
    s2.trace_cell = {cell(run0, 0, 2, 3), cell(run0, 0, 1, 2), cell(run0, 0, 0, 1), cell(run0, 0, 0, 0)};
    s2.ntrace_total = 4;
    s.srchs.push_back(s2);
    bd.add("SRCH", encode(s2));

    WRbld rb;
    rb.route_id = 1;
    rb.nets = {3, -1};
    rb.names = {"VCC", "nobody"};
    s.rblds.push_back(rb);
    bd.add("RBLD", encode(rb));

    WRip rip;
    rip.net = 3;
    rip.ctx = 1;
    rip.route_id = 1;
    rip.removed = {2}; // copper 0 predates the snapshot and survives
    s.rips.push_back(rip);
    bd.add("RIP ", encode(rip));

    WCmit c3; // copper 3
    c3.copper_id = 3;
    c3.net = 1;
    c3.flags = 1u;
    c3.power = 2;
    c3.ctx = 1;
    c3.route_id = 1;
    c3.width_mm = 0.5;
    c3.path = s2.path;
    s.cmits.push_back(c3);
    bd.add("CMIT", encode(c3));

    WRne rne1;
    rne1.route_id = 1;
    rne1.net = 1;
    rne1.rips_after = 1;
    rne1.status = 0;
    rne1.ripped = {"VCC"};
    rne1.failed = {"J1.2", "J1.3"};
    s.rnes.push_back(rne1);
    bd.add("RNE ", encode(rne1));

    // drop_to_plane: one search and a commit, then DROP.
    WSrch s3;
    s3.search_id = 3;
    s3.route_id = NONE32;
    s3.ctx = 2;
    s3.net = 1;
    s3.flags = (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9);
    s3.max_cost = 5000;
    s3.cost = 400;
    s3.expanded = 5;
    s3.duration_s = 0.0002;
    s3.src = {cell(run0, 0, 5, 0)};
    s3.path = {cell(run0, 0, 5, 0), cell(run0, 0, 5, 1)};
    s3.trace_cell = {cell(run0, 0, 5, 0), cell(run0, 0, 5, 1), cell(run0, 1, 5, 1)};
    s3.trace_d = {0, 10, 400};
    s3.ntrace_total = 3;
    s.srchs.push_back(s3);
    bd.add("SRCH", encode(s3));

    WCmit c4; // copper 4
    c4.copper_id = 4;
    c4.net = 1;
    c4.flags = 2u;
    c4.ctx = 2;
    c4.route_id = NONE32;
    c4.width_mm = 0.2;
    c4.vias = {{5, 1}};
    c4.path = s3.path;
    s.cmits.push_back(c4);
    bd.add("CMIT", encode(c4));

    WDrop drop;
    drop.net = 1;
    drop.ok = 1;
    drop.max_cost = 5000;
    drop.name = "GND";
    drop.pad_label = "U1.1";
    s.drops.push_back(drop);
    bd.add("DROP", encode(drop));

    // A direct search that raised: partial trace, cost and expanded -1.
    WSrch s4;
    s4.search_id = 4;
    s4.net = 2;
    s4.flags = (1u << 4) | (1u << 7) | (1u << 8) | (1u << 9);
    s4.cost = -1;
    s4.expanded = -1;
    s4.src = {cell(run0, 0, 3, 3)};
    s4.tgt = {cell(run0, 1, 3, 4)};
    s4.trace_cell = {cell(run0, 0, 3, 3), cell(run0, 0, 3, 4)};
    s4.trace_d = {0, 10};
    s4.ntrace_total = 2;
    s.srchs.push_back(s4);
    bd.add("SRCH", encode(s4));

    // A direct search with tracing off: no wavefront although expanded > 0.
    WSrch s5;
    s5.search_id = 5;
    s5.net = 3;
    s5.flags = (1u << 5);
    s5.cost = 10;
    s5.expanded = 12;
    s5.src = {cell(run0, 0, 0, 0)};
    s5.tgt = {cell(run0, 0, 1, 0)};
    s5.path = {cell(run0, 0, 0, 0), cell(run0, 0, 1, 0)};
    s.srchs.push_back(s5);
    bd.add("SRCH", encode(s5));

    WMark m1;
    m1.level = 1;
    m1.text = "direct write: novia fill \"CAN\"";
    s.marks.push_back(m1);
    bd.add("MARK", encode(m1));

    WRout rout;
    rout.segs = {{0, 2, 0.25, 0.25, 0.5, 0.25, 0.2}, {1, 1, 0.0, 0.25, 0.0, 0.0, 0.5}};
    rout.vias = {{0.5, 0.25, 2}};
    s.routs.push_back(rout);
    bd.add("ROUT", encode(rout));

    // --- session 1: a new router (4 x 3, 4 directions); ids continue ---
    WRun run1 = make_run(4, 3, 4);
    run1.flags = 2u;
    run1.cell = 0.5;
    run1.origin_x = 0;
    run1.origin_y = 0;
    run1.layer_name[0] = "F.Cu";
    run1.layer_name[1] = "In1.Cu";
    run1.producer = "synth session 2";
    s.runs.push_back(run1);
    bd.add("RUN ", encode(run1));

    s.grds.push_back(make_grids(0, 12, "attach"));
    bd.add("GRDS", encode(s.grds.back()));

    WCmit c5; // copper 5
    c5.copper_id = 5;
    c5.net = 2;
    c5.ctx = 0;
    c5.width_mm = 0.2;
    c5.path = {cell(run1, 0, 0, 0), cell(run1, 0, 1, 0), cell(run1, 0, 2, 0)};
    s.cmits.push_back(c5);
    bd.add("CMIT", encode(c5));

    WSrch s6;
    s6.search_id = 6;
    s6.net = 2;
    s6.flags = (1u << 5) | (1u << 7) | (1u << 9);
    s6.cost = 30;
    s6.expanded = 7;
    s6.duration_s = 0.0001;
    s6.src = {cell(run1, 0, 0, 0)};
    s6.tgt = {cell(run1, 1, 3, 2)};
    s6.path = {cell(run1, 0, 0, 0), cell(run1, 0, 1, 0), cell(run1, 1, 1, 0)};
    s6.trace_cell = {cell(run1, 0, 0, 0), cell(run1, 0, 1, 0), cell(run1, 0, 0, 1), cell(run1, 1, 1, 0)};
    s6.trace_d = {0, 10, 10, 410};
    s6.ntrace_total = 4;
    s.srchs.push_back(s6);
    bd.add("SRCH", encode(s6));

    WNets nets2; // a later NETS replaces a name
    nets2.entries = {{3, "+3V3"}, {4, "CAN_H"}};
    s.nets.push_back(nets2);
    bd.add("NETS", encode(nets2));

    s.brds.push_back(sample_board("final"));
    bd.add("BRD ", encode(s.brds.back()));

    if (with_end) {
        WEnd e;
        e.searches = static_cast<std::uint32_t>(s.srchs.size());
        e.routes = static_cast<std::uint32_t>(s.rnbs.size());
        e.rips = 0;
        for (const WSrch &x : s.srchs) {
            if (x.expanded > 0) e.expanded_total += x.expanded;
            e.trace_entries += static_cast<std::int64_t>(x.trace_cell.size());
        }
        e.wall_s = bd.w.t + 0.5;
        e.failed = {{"GND", "J1.2"}, {"GND", "J1.3"}};
        e.nfailed = static_cast<std::uint32_t>(e.failed.size());
        s.ends.push_back(e);
        bd.add("END ", encode(e));
    }

    s.bytes = bd.w.out;
    s.chunk_offsets = bd.w.chunk_offsets;
    return s;
}

bool write_file(const std::string &path, const std::vector<unsigned char> &bytes) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const std::size_t n = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    const bool ok = n == bytes.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace synth

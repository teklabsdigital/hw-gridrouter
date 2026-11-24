// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// test_trace.cpp: the trace reader against the synthetic writer.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "errors.hpp"
#include "trace.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using rvtest::open_bytes;

namespace {

const rv::Chunk *find_chunk(const rv::Trace &t, rv::Kind kind, std::size_t nth) {
    std::size_t seen = 0;
    for (const rv::Chunk &c : t.chunks())
        if (c.kind == kind && seen++ == nth) return &c;
    return nullptr;
}

bool same_double(double a, double b) {
    std::uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    return x == y; // bit-exact, NaN included
}

void print_issues(const rv::Trace &t) {
    for (const rv::Issue &is : t.issues())
        std::cerr << "    issue: " << (is.severity == rv::Issue::Error ? "error" : "warning") << " chunk "
                  << is.chunk << ": " << is.message << "\n";
}

// A file with a RUN and the given extra chunks.
std::vector<unsigned char> with_run(const std::vector<std::pair<std::string, synth::Bytes>> &chunks,
                                    std::uint16_t minor = 0) {
    synth::Writer w(1.0, 1, minor, 0);
    w.chunk("RUN ", synth::encode(synth::make_run(6, 5, 4)));
    for (const auto &c : chunks) w.chunk(c.first.c_str(), c.second);
    return w.out;
}

} // namespace

// --- the worked example of section 13 ---------------------------------------

TEST(worked_example_bytes_match_the_spec) {
    synth::Writer w(1795392000.25);
    synth::WMark m;
    m.level = 0;
    m.text = "CAN";
    w.chunk_at("MARK", synth::encode(m), 1.5);
    const unsigned char expected[56] = {
        0x47, 0x52, 0x54, 0x52, 0x41, 0x43, 0x45, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x10, 0x80, 0xe0, 0xc0, 0xda, 0x41, 0x4d, 0x41, 0x52, 0x4b,
        0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f, 0x00, 0x00,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x43, 0x41, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK_EQ(w.out.size(), sizeof expected);
    CHECK(w.out.size() == sizeof expected && std::memcmp(w.out.data(), expected, sizeof expected) == 0);
}

TEST(worked_example_reads) {
    const unsigned char bytes[56] = {
        0x47, 0x52, 0x54, 0x52, 0x41, 0x43, 0x45, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x10, 0x80, 0xe0, 0xc0, 0xda, 0x41, 0x4d, 0x41, 0x52, 0x4b,
        0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f, 0x00, 0x00,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x43, 0x41, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    auto t = open_bytes(std::vector<unsigned char>(bytes, bytes + 56));
    CHECK(t->header_complete());
    CHECK_EQ(t->major(), 1);
    CHECK_EQ(t->minor(), 0);
    CHECK(t->open_time() == 1795392000.25);
    CHECK_EQ(t->chunks().size(), 1u);
    CHECK_EQ(t->marks().size(), 1u);
    if (t->marks().size() == 1) {
        CHECK_EQ(t->marks()[0].level, 0u);
        CHECK_EQ(t->marks()[0].text, std::string("CAN"));
    }
    CHECK(t->chunks().size() == 1 && t->chunks()[0].t == 1.5);
    CHECK_EQ(t->indexed_end(), 56u);
    CHECK(!t->complete());
    CHECK_EQ(t->issues().size(), 0u);
}

// --- round trip of every chunk kind -------------------------------------------

TEST(sample_reads_without_issues) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->issues().size(), 0u);
    print_issues(*t);
    CHECK(t->complete());
    CHECK(!t->stopped());
    CHECK_EQ(t->chunks().size(), s.tags.size());
    CHECK_EQ(t->indexed_end(), s.bytes.size());
    CHECK_EQ(rvtest::invariant_failure(*t), std::string());
    for (std::size_t k = 0; k < t->chunks().size() && k < s.tags.size(); ++k) {
        CHECK_EQ(std::string(t->chunks()[k].tag, 4), s.tags[k]);
        CHECK_EQ(t->chunks()[k].offset, s.chunk_offsets[k]);
        CHECK(t->chunks()[k].t == s.times[k]);
        CHECK(!t->chunks()[k].corrupt);
    }
    // every kind appears
    for (int k = 0; k < rv::kKindCount; ++k) CHECK(t->kind_count(static_cast<rv::Kind>(k)) > 0);
    CHECK_EQ(t->kind_count(rv::Kind::Unknown), s.unknown_chunks);
    CHECK_EQ(rvtest::exercise(*t), 0);
}

TEST(round_trip_run_nets_pad_rect_disc_via) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->runs().size(), s.runs.size());
    for (std::size_t k = 0; k < t->runs().size() && k < s.runs.size(); ++k) {
        const rv::Run &r = t->runs()[k];
        const synth::WRun &w = s.runs[k];
        CHECK(r.valid);
        CHECK_EQ(r.nx, w.nx);
        CHECK_EQ(r.ny, w.ny);
        CHECK_EQ(r.nd, w.nd);
        CHECK_EQ(r.block, w.block);
        CHECK_EQ(r.via_cost, w.via_cost);
        CHECK_EQ(r.rip_cost, w.rip_cost);
        CHECK_EQ(r.max_rips, w.max_rips);
        CHECK_EQ(r.h_max, w.h_max);
        CHECK_EQ(r.h_min, w.h_min);
        CHECK_EQ(r.h_off, w.h_off);
        for (int i = 0; i < 2; ++i) {
            CHECK_EQ(r.r_track[i], w.r_track[i]);
            CHECK_EQ(r.r_wide[i], w.r_wide[i]);
            CHECK_EQ(r.r_via[i], w.r_via[i]);
            CHECK(same_double(r.pad_clear[i], w.pad_clear[i]));
            CHECK(same_double(r.edge_margin[i], w.edge_margin[i]));
            CHECK_EQ(r.layer_name[i], w.layer_name[i]);
        }
        CHECK_EQ(r.r_via_space, w.r_via_space);
        CHECK_EQ(r.flags, w.flags);
        CHECK(same_double(r.cell, w.cell));
        CHECK(same_double(r.origin_x, w.origin_x));
        CHECK(same_double(r.origin_y, w.origin_y));
        CHECK(same_double(r.track, w.track));
        CHECK(same_double(r.track_wide, w.track_wide));
        CHECK(same_double(r.pad_novia, w.pad_novia));
        CHECK(same_double(r.via_dia, w.via_dia));
        CHECK(same_double(r.via_drill, w.via_drill));
        CHECK(r.dirs == w.dirs);
        CHECK(r.step == w.step);
        CHECK(r.turn == w.turn);
        CHECK_EQ(r.producer, w.producer);
    }
    // sessions: the chunks after the second RUN belong to session 1
    const rv::Chunk *run1 = find_chunk(*t, rv::Kind::Run, 1);
    CHECK(run1 != nullptr);
    if (run1 != nullptr) {
        CHECK_EQ(run1->session, 1u);
        CHECK_EQ(t->chunks().back().session, 1u);
        CHECK_EQ(t->runs()[0].end_chunk, t->runs()[1].chunk);
        CHECK_EQ(t->runs()[1].end_chunk, t->chunks().size());
    }
    CHECK_EQ(t->chunks()[0].session, rv::kNone32); // NETS before the first RUN

    CHECK_EQ(t->nets_chunks().size(), s.nets.size());
    for (std::size_t k = 0; k < t->nets_chunks().size() && k < s.nets.size(); ++k)
        CHECK(t->nets_chunks()[k].entries == s.nets[k].entries);
    // later NETS entry replaces (7.2)
    CHECK_EQ(t->net_names().size(), 4u);
    CHECK_EQ(t->net_names().at(3), std::string("+3V3"));
    CHECK_EQ(t->net_names().at(2), std::string("/SIG"));

    CHECK_EQ(t->pads().size(), s.pads.size());
    if (!t->pads().empty()) {
        const rv::Pad &p = t->pads()[0];
        const synth::WPad &w = s.pads[0];
        CHECK_EQ(p.net, w.net);
        CHECK_EQ(p.layers, w.layers);
        CHECK(same_double(p.power, w.power) && same_double(p.cx, w.cx) && same_double(p.cy, w.cy));
        CHECK(same_double(p.x0, w.x0) && same_double(p.y0, w.y0) && same_double(p.x1, w.x1) &&
              same_double(p.y1, w.y1));
        CHECK(same_double(p.d_t, w.d_t) && same_double(p.d_w, w.d_w) && same_double(p.d_novia, w.d_novia));
    }
    CHECK_EQ(t->rects().size(), s.rects.size());
    for (std::size_t k = 0; k < t->rects().size() && k < s.rects.size(); ++k) {
        const rv::Rect &r = t->rects()[k];
        const synth::WRect &w = s.rects[k];
        CHECK_EQ(r.op, w.op);
        CHECK_EQ(r.net, w.net);
        CHECK_EQ(r.layers, w.layers);
        CHECK_EQ(r.levels, w.levels);
        CHECK(same_double(r.x0, w.x0) && same_double(r.y0, w.y0) && same_double(r.x1, w.x1) &&
              same_double(r.y1, w.y1) && same_double(r.d_t, w.d_t) && same_double(r.d_w, w.d_w));
    }
    CHECK_EQ(t->discs().size(), s.discs.size());
    if (!t->discs().empty()) {
        const rv::Disc &d = t->discs()[0];
        const synth::WDisc &w = s.discs[0];
        CHECK(d.net == w.net && d.layer == w.layer && d.i == w.i && d.j == w.j && d.r_t == w.r_t &&
              d.r_w == w.r_w);
    }
    CHECK_EQ(t->vias().size(), s.vias.size());
    for (std::size_t k = 0; k < t->vias().size() && k < s.vias.size(); ++k) {
        const rv::Via &v = t->vias()[k];
        const synth::WVia &w = s.vias[k];
        CHECK_EQ(v.copper_id_field, w.copper_id);
        CHECK_EQ(v.copper, w.copper_id);
        CHECK(v.net == w.net && v.i == w.i && v.j == w.j && v.power == w.power);
        CHECK_EQ(v.added, w.added != 0);
        CHECK_EQ(v.ctx, w.ctx);
        CHECK_EQ(v.route_id, w.route_id);
    }
}

TEST(round_trip_snap_grds) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->snaps().size(), 1u);
    CHECK_EQ(t->grids_infos().size(), s.grds.size());
    for (std::size_t k = 0; k < t->grids_infos().size() && k < s.grds.size(); ++k) {
        const rv::GridsInfo &g = t->grids_infos()[k];
        const synth::WGrds &w = s.grds[k];
        CHECK_EQ(g.which, w.which);
        CHECK_EQ(g.nc, w.nc);
        CHECK_EQ(g.label, w.label);
        auto grids = t->grids(static_cast<std::uint32_t>(k));
        CHECK(grids->t[0] == w.t0);
        CHECK(grids->t[1] == w.t1);
        CHECK(grids->w[0] == w.w0);
        CHECK(grids->w[1] == w.w1);
        CHECK(grids->novia == w.novia);
    }
}

TEST(round_trip_routes_rebuild_rip_drop_mark_rout_end) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->routes().size(), s.rnbs.size());
    for (std::size_t k = 0; k < t->routes().size() && k < s.rnbs.size(); ++k) {
        const rv::Route &r = t->routes()[k];
        const synth::WRnb &w = s.rnbs[k];
        CHECK(r.valid);
        CHECK_EQ(r.route_id_field, w.route_id);
        CHECK_EQ(r.net, w.net);
        CHECK_EQ(r.wide, w.wide);
        CHECK_EQ(r.power, w.power);
        CHECK_EQ(r.name, w.name);
        CHECK_EQ(r.pads.size(), w.pads.size());
        for (std::size_t p = 0; p < r.pads.size() && p < w.pads.size(); ++p) {
            CHECK(same_double(r.pads[p].cx, w.pads[p].cx) && same_double(r.pads[p].cy, w.pads[p].cy));
            CHECK_EQ(r.pads[p].layers, w.pads[p].layers);
            CHECK_EQ(r.pads[p].ncells, w.pads[p].ncells);
            CHECK_EQ(r.pads[p].label, w.pads[p].label);
        }
        CHECK_EQ(r.end, k); // each RNE pairs with its RNB
    }
    CHECK_EQ(t->route_ends().size(), s.rnes.size());
    for (std::size_t k = 0; k < t->route_ends().size() && k < s.rnes.size(); ++k) {
        const rv::RouteEnd &e = t->route_ends()[k];
        const synth::WRne &w = s.rnes[k];
        CHECK_EQ(e.route_id, w.route_id);
        CHECK_EQ(e.net, w.net);
        CHECK_EQ(e.rips_after, w.rips_after);
        CHECK_EQ(e.status, w.status);
        CHECK(e.ripped == w.ripped);
        CHECK(e.failed == w.failed);
    }
    CHECK_EQ(t->failed_pads(), 2u);

    CHECK_EQ(t->rebuilds().size(), s.rblds.size());
    if (!t->rebuilds().empty()) {
        CHECK_EQ(t->rebuilds()[0].route_id, s.rblds[0].route_id);
        CHECK(t->rebuilds()[0].nets == s.rblds[0].nets);
        CHECK(t->rebuilds()[0].names == s.rblds[0].names);
    }
    CHECK_EQ(t->rips().size(), s.rips.size());
    if (!t->rips().empty()) {
        const rv::Rip &r = t->rips()[0];
        CHECK_EQ(r.net, s.rips[0].net);
        CHECK_EQ(r.ctx, s.rips[0].ctx);
        CHECK_EQ(r.route_id, s.rips[0].route_id);
        CHECK(r.removed == s.rips[0].removed);
    }
    CHECK_EQ(t->drops().size(), s.drops.size());
    if (!t->drops().empty()) {
        const rv::Drop &d = t->drops()[0];
        CHECK(d.net == s.drops[0].net && d.ok == s.drops[0].ok && d.max_cost == s.drops[0].max_cost);
        CHECK_EQ(d.name, s.drops[0].name);
        CHECK_EQ(d.pad_label, s.drops[0].pad_label);
    }
    CHECK_EQ(t->marks().size(), s.marks.size());
    for (std::size_t k = 0; k < t->marks().size() && k < s.marks.size(); ++k) {
        CHECK_EQ(t->marks()[k].level, s.marks[k].level);
        CHECK_EQ(t->marks()[k].text, s.marks[k].text);
    }
    CHECK_EQ(t->routes_outputs().size(), s.routs.size());
    if (!t->routes_outputs().empty()) {
        const rv::Routes &o = t->routes_outputs()[0];
        const synth::WRout &w = s.routs[0];
        CHECK_EQ(o.segs.size(), w.segs.size());
        for (std::size_t k = 0; k < o.segs.size() && k < w.segs.size(); ++k) {
            CHECK(o.segs[k].layer == w.segs[k].layer && o.segs[k].net == w.segs[k].net);
            CHECK(same_double(o.segs[k].x0, w.segs[k].x0) && same_double(o.segs[k].y0, w.segs[k].y0) &&
                  same_double(o.segs[k].x1, w.segs[k].x1) && same_double(o.segs[k].y1, w.segs[k].y1) &&
                  same_double(o.segs[k].w, w.segs[k].w));
        }
        CHECK_EQ(o.vias.size(), w.vias.size());
        for (std::size_t k = 0; k < o.vias.size() && k < w.vias.size(); ++k)
            CHECK(same_double(o.vias[k].x, w.vias[k].x) && same_double(o.vias[k].y, w.vias[k].y) &&
                  o.vias[k].net == w.vias[k].net);
    }
    CHECK_EQ(t->ends().size(), 1u);
    if (!t->ends().empty()) {
        const rv::End &e = t->ends()[0];
        const synth::WEnd &w = s.ends[0];
        CHECK_EQ(e.searches, w.searches);
        CHECK_EQ(e.routes, w.routes);
        CHECK_EQ(e.nfailed, w.nfailed);
        CHECK_EQ(e.rips, w.rips);
        CHECK_EQ(e.expanded_total, w.expanded_total);
        CHECK_EQ(e.trace_entries, w.trace_entries);
        CHECK(same_double(e.wall_s, w.wall_s));
        CHECK(e.failed == w.failed);
        CHECK_EQ(e.expanded_total, t->expanded_total());
        CHECK_EQ(e.trace_entries, t->trace_entries());
    }
}

TEST(round_trip_srch_and_wavefronts) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->searches().size(), s.srchs.size());
    for (std::size_t k = 0; k < t->searches().size() && k < s.srchs.size(); ++k) {
        const rv::Search &x = t->searches()[k];
        const synth::WSrch &w = s.srchs[k];
        CHECK(x.valid);
        CHECK_EQ(x.id_field, w.search_id);
        CHECK_EQ(x.route_id, w.route_id);
        CHECK_EQ(x.ctx, w.ctx);
        CHECK_EQ(x.net, w.net);
        CHECK_EQ(x.flags, w.flags);
        CHECK(x.gi == w.gi && x.gj == w.gj && x.rips_at_call == w.rips_at_call);
        CHECK(x.max_cost == w.max_cost && x.cost == w.cost && x.expanded == w.expanded);
        CHECK(same_double(x.duration_s, w.duration_s));
        CHECK_EQ(x.nsrc, w.src.size());
        CHECK_EQ(x.ntgt, w.tgt.size());
        CHECK_EQ(x.npath, w.path.size());
        CHECK_EQ(x.ntrace, w.trace_cell.size());
        CHECK_EQ(x.ntrace_total, w.ntrace_total);
        CHECK_EQ(x.repeat_of, w.repeat_of);
        CHECK(std::vector<std::uint32_t>(x.src(), x.src() + x.nsrc) == w.src);
        CHECK(std::vector<std::uint32_t>(x.tgt(), x.tgt() + x.ntgt) == w.tgt);
        CHECK(std::vector<std::uint32_t>(x.path(), x.path() + x.npath) == w.path);

        auto wf = t->wavefront(static_cast<std::uint32_t>(k));
        const bool traced = (w.flags & (1u << 9)) != 0;
        CHECK_EQ(wf != nullptr, traced);
        if (wf && traced) {
            const synth::WSrch &src = w.repeat_of == synth::NONE32 ? w : s.srchs[w.repeat_of];
            CHECK(wf->cells == src.trace_cell);
            CHECK_EQ(wf->has_costs, (src.flags & (1u << 7)) != 0);
            if (wf->has_costs) CHECK(wf->costs == src.trace_d);
            else CHECK(wf->costs.empty());
            CHECK_EQ(wf->ntrace_total, src.ntrace_total);
        }
    }
    // the repeat shares its source's wavefront object
    auto a = t->wavefront(0);
    auto b = t->wavefront(1);
    CHECK(a != nullptr && a == b);
    CHECK_EQ(t->searches()[1].trace_source, 0u);
    // tracing off and error searches
    CHECK(t->wavefront(5) == nullptr);
    CHECK(t->wavefront(4) != nullptr && t->wavefront(4)->cells.size() == 2);
    bool threw = false;
    try {
        (void)t->wavefront(99);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);
}

TEST(round_trip_cmit) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->commits().size(), s.cmits.size());
    for (std::size_t k = 0; k < t->commits().size() && k < s.cmits.size(); ++k) {
        const rv::Commit &c = t->commits()[k];
        const synth::WCmit &w = s.cmits[k];
        CHECK_EQ(c.copper_id_field, w.copper_id);
        CHECK_EQ(c.net, w.net);
        CHECK_EQ(c.flags, w.flags);
        CHECK_EQ(c.power, w.power);
        CHECK_EQ(c.ctx, w.ctx);
        CHECK_EQ(c.route_id, w.route_id);
        CHECK(same_double(c.width_mm, w.width_mm));
        CHECK_EQ(c.stubs.size(), w.stubs.size());
        for (std::size_t j = 0; j < c.stubs.size() && j < w.stubs.size(); ++j)
            CHECK(c.stubs[j].layer == w.stubs[j].layer && same_double(c.stubs[j].x0, w.stubs[j].x0) &&
                  same_double(c.stubs[j].y0, w.stubs[j].y0) && same_double(c.stubs[j].x1, w.stubs[j].x1) &&
                  same_double(c.stubs[j].y1, w.stubs[j].y1) && same_double(c.stubs[j].w, w.stubs[j].w));
        CHECK_EQ(c.vias.size(), w.vias.size());
        for (std::size_t j = 0; j < c.vias.size() && j < w.vias.size(); ++j)
            CHECK(c.vias[j].i == w.vias[j].first && c.vias[j].j == w.vias[j].second);
        CHECK(c.path == w.path);
    }
}

TEST(round_trip_brd) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    CHECK_EQ(t->boards().size(), s.brds.size());
    for (std::size_t k = 0; k < t->boards().size() && k < s.brds.size(); ++k) {
        const rv::BoardInfo &b = t->boards()[k];
        const synth::WBrd &w = s.brds[k];
        CHECK(same_double(b.thickness_mm, w.thickness_mm));
        CHECK_EQ(b.origin_x_nm, w.origin_x_nm);
        CHECK_EQ(b.origin_y_nm, w.origin_y_nm);
        CHECK_EQ(b.label, w.label);
        CHECK_EQ(b.layers.size(), w.layers.size());
        for (std::size_t j = 0; j < b.layers.size() && j < w.layers.size(); ++j)
            CHECK(b.layers[j].kicad_id == w.layers[j].kicad_id && b.layers[j].type == w.layers[j].type &&
                  b.layers[j].name == w.layers[j].name);
        CHECK_EQ(b.stackup.size(), w.stackup.size());
        for (std::size_t j = 0; j < b.stackup.size() && j < w.stackup.size(); ++j)
            CHECK(same_double(b.stackup[j].thickness_mm, w.stackup[j].thickness_mm) &&
                  same_double(b.stackup[j].er, w.stackup[j].er) && b.stackup[j].name == w.stackup[j].name);
        CHECK(std::isnan(b.stackup[0].er));
        CHECK_EQ(b.nets.size(), w.nets.size());
        for (std::size_t j = 0; j < b.nets.size() && j < w.nets.size(); ++j)
            CHECK(b.nets[j].code == w.nets[j].first && b.nets[j].name == w.nets[j].second);
        CHECK_EQ(b.footprints.size(), w.footprints.size());
        for (std::size_t j = 0; j < b.footprints.size() && j < w.footprints.size(); ++j) {
            const rv::Footprint &f = b.footprints[j];
            const synth::WFootprint &wf = w.footprints[j];
            CHECK(same_double(f.rot_deg, wf.rot_deg) && f.x == wf.x && f.y == wf.y &&
                  f.flipped == (wf.flipped != 0) && f.ref == wf.ref && f.value == wf.value &&
                  f.fpid == wf.fpid);
        }
        CHECK_EQ(b.nrecords, w.order.size());
        CHECK_EQ(b.records_by_kind[0], 1u); // one unknown record kind, skipped
        CHECK_EQ(b.records_by_kind[1], w.polysets.size());
        CHECK_EQ(b.records_by_kind[6], w.texts.size());

        auto geo = t->board_geometry(static_cast<std::uint32_t>(k));
        CHECK_EQ(geo->polysets.size(), w.polysets.size());
        for (std::size_t j = 0; j < geo->polysets.size() && j < w.polysets.size(); ++j) {
            const rv::Polyset &p = geo->polysets[j];
            const synth::WPolyset &wp = w.polysets[j];
            CHECK(p.layer_index == wp.layer_index && p.net == wp.net && p.fp == wp.fp &&
                  p.role == wp.role && p.sub == wp.sub);
            // rebuild the nested form and compare
            std::vector<std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>>> polys;
            for (std::size_t q = 0; q + 1 < p.poly_start.size(); ++q) {
                polys.emplace_back();
                for (std::uint32_t c = p.poly_start[q]; c < p.poly_start[q + 1]; ++c) {
                    polys.back().emplace_back();
                    for (std::uint32_t pt = p.contour_start[c]; pt < p.contour_start[c + 1]; ++pt)
                        polys.back().back().emplace_back(p.xy[2 * pt], p.xy[2 * pt + 1]);
                }
            }
            CHECK(polys == wp.polys);
        }
        CHECK_EQ(geo->holes.size(), w.holes.size());
        for (std::size_t j = 0; j < geo->holes.size() && j < w.holes.size(); ++j) {
            const rv::Hole &h = geo->holes[j];
            const synth::WHole &wh = w.holes[j];
            CHECK(h.x == wh.x && h.y == wh.y && h.w == wh.w && h.h == wh.h &&
                  same_double(h.rot_deg, wh.rot_deg) && h.plated == wh.plated && h.shape == wh.shape &&
                  h.fp == wh.fp && h.net == wh.net);
        }
        CHECK_EQ(geo->padinfos.size(), w.padinfos.size());
        for (std::size_t j = 0; j < geo->padinfos.size() && j < w.padinfos.size(); ++j) {
            const rv::PadInfo &p = geo->padinfos[j];
            const synth::WPadInfo &wp = w.padinfos[j];
            CHECK(p.fp == wp.fp && p.net == wp.net && p.x == wp.x && p.y == wp.y &&
                  p.attrib == wp.attrib && p.number == wp.number);
        }
        CHECK_EQ(geo->tracks.size(), w.tracks.size());
        if (!geo->tracks.empty()) {
            const rv::BoardTrack &tr = geo->tracks[0];
            const synth::WTrack &wt = w.tracks[0];
            CHECK(tr.layer_index == wt.layer_index && tr.net == wt.net && tr.x0 == wt.x0 &&
                  tr.y0 == wt.y0 && tr.x1 == wt.x1 && tr.y1 == wt.y1 && tr.w == wt.w);
        }
        CHECK_EQ(geo->vias.size(), w.vias.size());
        if (!geo->vias.empty()) {
            const rv::BoardVia &v = geo->vias[0];
            const synth::WBVia &wv = w.vias[0];
            CHECK(v.x == wv.x && v.y == wv.y && v.dia == wv.dia && v.drill == wv.drill &&
                  v.net == wv.net && v.top == wv.top && v.bottom == wv.bottom);
        }
        CHECK_EQ(geo->texts.size(), w.texts.size());
        if (!geo->texts.empty()) {
            const rv::BoardText &x = geo->texts[0];
            const synth::WText &wx = w.texts[0];
            CHECK(x.layer_index == wx.layer_index && x.fp == wx.fp && x.x == wx.x && x.y == wx.y &&
                  same_double(x.angle_deg, wx.angle_deg) && x.text == wx.text);
        }
        // known records in file order
        std::vector<std::pair<std::uint32_t, std::uint32_t>> known;
        for (const auto &o : w.order)
            if (o.first >= 1 && o.first <= 6) known.push_back(o);
        CHECK(geo->order == known);
    }
}

// --- derived tables ---------------------------------------------------------------

TEST(copper_ids_follow_the_rule) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    // VIA(added) 0, CMIT 1, CMIT 2, CMIT 3, CMIT 4, then CMIT 5 in session 1:
    // the VIA with added = 0 takes no id and ids are not reset by RUN.
    const auto &cu = t->copper();
    CHECK_EQ(cu.size(), 6u);
    if (cu.size() == 6) {
        CHECK(cu[0].kind == rv::Kind::Via && cu[0].net == 3 && cu[0].session == 0);
        for (std::size_t k = 1; k < 6; ++k) CHECK(cu[k].kind == rv::Kind::Cmit);
        CHECK_EQ(cu[5].session, 1u);
        // the RIP removes copper 2 only (copper 0 predates the SNAP)
        const rv::Chunk *rip = find_chunk(*t, rv::Kind::Rip, 0);
        std::uint32_t rip_index = rv::kNone32;
        for (std::size_t k = 0; k < t->chunks().size(); ++k)
            if (&t->chunks()[k] == rip) rip_index = static_cast<std::uint32_t>(k);
        CHECK_EQ(cu[2].removed_by, rip_index);
        for (std::size_t k = 0; k < 6; ++k)
            if (k != 2) CHECK_EQ(cu[k].removed_by, rv::kNone32);
        CHECK_EQ(t->copper_removed(), 1u);
    }
    CHECK_EQ(t->vias()[1].copper, rv::kNone32);
}

TEST(copper_id_mismatch_is_reported) {
    synth::WCmit c;
    c.copper_id = 7; // should be 0
    c.net = 1;
    c.path = {0, 1};
    synth::WVia v;
    v.copper_id = 1;
    v.net = 1;
    v.added = 0; // should carry NONE32
    synth::WVia v2;
    v2.copper_id = 1; // correct: the second copper item
    v2.net = 1;
    v2.added = 1;
    auto bytes = with_run({{"CMIT", synth::encode(c)}, {"VIA ", synth::encode(v)}, {"VIA ", synth::encode(v2)}});
    auto t = open_bytes(bytes);
    // Three errors: the CMIT stores 7 (expected 0; the reader continues
    // from 7), the VIA with added = 0 stores 1, and the second VIA stores 1
    // where 8 is expected after the resynchronisation (17 open point 3).
    CHECK_EQ(t->error_count(), 3u);
    CHECK_EQ(t->copper().size(), 2u);
    CHECK_EQ(t->commits()[0].copper, 0u);
    CHECK_EQ(t->copper()[0].id, 7u);
    CHECK_EQ(t->vias()[1].copper, 1u);
    CHECK_EQ(t->copper()[1].id, 1u);
    CHECK_EQ(t->copper_index(7), 0u);
    CHECK_EQ(t->copper_index(1), 1u);
    CHECK_EQ(t->copper_index(0), rv::kNone32);
}

// --- 17 open points: the decisions this reader follows ---------------------------------

TEST(open_point_1_rip_before_the_first_snap) {
    // Router snapshotted before the recorder attached: a RIP before any
    // SNAP lists the net's copper of the session.  Valid, no issue; an
    // empty list is valid too (never snapshotted).
    synth::WCmit c0;
    c0.copper_id = 0;
    c0.net = 1;
    c0.path = {1, 2};
    synth::WCmit c1 = c0;
    c1.copper_id = 1;
    c1.net = 2;
    synth::WCmit c2 = c0;
    c2.copper_id = 2;
    synth::WRip listed;
    listed.net = 1;
    listed.removed = {0, 2};
    synth::WRip empty;
    empty.net = 2;
    auto t = open_bytes(with_run({{"CMIT", synth::encode(c0)},
                                  {"CMIT", synth::encode(c1)},
                                  {"CMIT", synth::encode(c2)},
                                  {"RIP ", synth::encode(listed)},
                                  {"RIP ", synth::encode(empty)}}));
    print_issues(*t);
    CHECK_EQ(t->issues().size(), 0u);
    CHECK_EQ(t->copper()[0].removed_by, 4u);
    CHECK_EQ(t->copper()[2].removed_by, 4u);
    CHECK_EQ(t->copper()[1].removed_by, rv::kNone32);
    CHECK(t->rips()[0].applied == std::vector<std::uint32_t>({0, 2}));
    // A partial list before any SNAP is still a warning.
    synth::WRip partial;
    partial.net = 1;
    partial.removed = {2};
    auto t2 = open_bytes(with_run({{"CMIT", synth::encode(c0)},
                                   {"CMIT", synth::encode(c1)},
                                   {"CMIT", synth::encode(c2)},
                                   {"RIP ", synth::encode(partial)}}));
    CHECK_EQ(t2->error_count(), 0u);
    CHECK_EQ(t2->warning_count(), 1u);
    CHECK_EQ(t2->copper()[2].removed_by, 4u);
    // After a SNAP an empty list is a warning when copper is expected.
    synth::WCmit c3 = c0;
    c3.copper_id = 1;
    synth::WRip none;
    none.net = 1;
    auto t3 = open_bytes(with_run({{"CMIT", synth::encode(c0)},
                                   {"SNAP", synth::encode_snap()},
                                   {"GRDS", synth::encode(synth::make_grids(1, 30, "snapshot"))},
                                   {"CMIT", synth::encode(c3)},
                                   {"RIP ", synth::encode(none)}}));
    CHECK_EQ(t3->error_count(), 0u);
    CHECK_EQ(t3->warning_count(), 1u);
}

TEST(open_point_3_resynchronise_after_an_id_mismatch) {
    // A corrupt VIA that created copper 0 takes no id; the next CMIT stores
    // 1.  One error for the VIA, one for the mismatch, and nothing after:
    // later chunks match again and RIP resolves the stored ids.
    synth::Bytes bad_via;
    bad_via.u32(0).i32(1); // far too short
    synth::WCmit c1;
    c1.copper_id = 1;
    c1.net = 1;
    c1.path = {1, 2};
    synth::WCmit c2 = c1;
    c2.copper_id = 2;
    synth::WRip rip;
    rip.net = 1;
    rip.removed = {1, 2};
    synth::WSrch s0;
    s0.search_id = 5; // expected 0
    s0.src = {1};
    synth::WSrch s1 = s0;
    s1.search_id = 6; // follows the stored 5
    auto t = open_bytes(with_run({{"VIA ", bad_via},
                                  {"CMIT", synth::encode(c1)},
                                  {"CMIT", synth::encode(c2)},
                                  {"RIP ", synth::encode(rip)},
                                  {"SRCH", synth::encode(s0)},
                                  {"SRCH", synth::encode(s1)}}));
    print_issues(*t);
    CHECK_EQ(t->error_count(), 3u); // corrupt VIA, CMIT copper_id 1 != 0, SRCH 5 != 0
    CHECK_EQ(t->warning_count(), 0u);
    CHECK_EQ(t->copper().size(), 2u);
    CHECK_EQ(t->copper()[0].id, 1u);
    CHECK_EQ(t->copper()[1].id, 2u);
    CHECK_EQ(t->copper()[0].removed_by, 4u);
    CHECK_EQ(t->copper()[1].removed_by, 4u);
    CHECK_EQ(t->search_index(6), 1u);
    CHECK_EQ(t->search_index(0), rv::kNone32);
    CHECK_EQ(rvtest::invariant_failure(*t), std::string());
}

TEST(open_point_4_error_search_with_cells_outside_the_grid) {
    synth::WSrch s;
    s.flags = 1u << 8; // error
    s.cost = -1;
    s.expanded = -1;
    s.src = {3, 0xFFFFFFFEu, 4}; // a negative cell stored as u32
    s.tgt = {60, 7};
    auto t = open_bytes(with_run({{"SRCH", synth::encode(s)}}));
    print_issues(*t);
    CHECK(!t->chunks()[1].corrupt);
    CHECK_EQ(t->error_count(), 0u);
    CHECK_EQ(t->warning_count(), 1u);
    const rv::Search &x = t->searches()[0];
    CHECK(x.valid);
    CHECK_EQ(x.nsrc, 3u);
    CHECK_EQ(x.ntgt, 2u);
    CHECK_EQ(x.nsrc_kept, 2u);
    CHECK_EQ(x.ntgt_kept, 1u);
    CHECK(std::vector<std::uint32_t>(x.src(), x.src() + x.nsrc_kept) == std::vector<std::uint32_t>({3, 4}));
    CHECK_EQ(x.tgt()[0], 7u);
    // Without `error` the same cells make the chunk corrupt, and a path
    // cell outside the grid always does.
    s.flags = 0;
    auto t2 = open_bytes(with_run({{"SRCH", synth::encode(s)}}));
    CHECK(t2->chunks()[1].corrupt);
    synth::WSrch p;
    p.flags = 1u << 8;
    p.src = {1};
    p.path = {1, 60};
    auto t3 = open_bytes(with_run({{"SRCH", synth::encode(p)}}));
    CHECK(t3->chunks()[1].corrupt);
}

TEST(open_point_5_vias_outside_the_grid) {
    synth::WVia v;
    v.copper_id = 0;
    v.net = 1;
    v.i = 6; // 6 x 5 grid
    v.j = 1;
    v.added = 1;
    synth::WCmit c;
    c.copper_id = 1;
    c.net = 1;
    c.path = {1, 2};
    c.vias = {{2, 0}, {-1, 3}};
    auto t = open_bytes(with_run({{"VIA ", synth::encode(v)}, {"CMIT", synth::encode(c)}}));
    print_issues(*t);
    CHECK_EQ(t->error_count(), 0u);
    CHECK_EQ(t->warning_count(), 2u);
    CHECK_EQ(t->copper().size(), 2u); // the via still takes copper id 0
    CHECK(t->vias()[0].off_grid);
    CHECK_EQ(t->copper()[1].id, 1u);
    CHECK(!t->commits()[0].vias[0].off_grid);
    CHECK(t->commits()[0].vias[1].off_grid);
}

TEST(open_point_6_rip_of_another_nets_copper) {
    synth::WCmit c0;
    c0.copper_id = 0;
    c0.net = 1;
    c0.path = {1, 2};
    synth::WCmit c1 = c0;
    c1.copper_id = 1;
    c1.net = 2;
    synth::WRip rip;
    rip.net = 1;
    rip.removed = {0, 1}; // copper 1 is net 2's
    synth::WRip again;
    again.net = 2;
    again.removed = {1}; // already removed: still an error
    auto t = open_bytes(with_run({{"SNAP", synth::encode_snap()},
                                  {"GRDS", synth::encode(synth::make_grids(1, 30, "snapshot"))},
                                  {"CMIT", synth::encode(c0)},
                                  {"CMIT", synth::encode(c1)},
                                  {"RIP ", synth::encode(rip)},
                                  {"RIP ", synth::encode(again)}}));
    print_issues(*t);
    CHECK_EQ(t->copper()[0].removed_by, 5u);
    CHECK_EQ(t->copper()[1].removed_by, 5u); // removed, as section 8 says
    CHECK_EQ(t->error_count(), 1u);          // the second RIP: already removed
    // warnings: another net's copper; the first list is not exactly {0};
    // the second list is not exactly {} (nothing of net 2 is left)
    CHECK_EQ(t->warning_count(), 3u);
}

TEST(corrupt_cmit_still_takes_a_copper_id) {
    synth::Bytes bad;
    bad.u32(0).i32(1).u32(0); // far too short for the 48-byte fixed part
    synth::WCmit c;
    c.copper_id = 1;
    c.net = 2;
    c.path = {3};
    auto t = open_bytes(with_run({{"CMIT", bad}, {"CMIT", synth::encode(c)}}));
    CHECK_EQ(t->copper().size(), 2u);
    CHECK(!t->copper()[0].valid);
    CHECK(t->copper()[1].valid);
    CHECK_EQ(t->error_count(), 1u); // only the corrupt chunk; the id 1 matches
    CHECK(t->chunks()[1].corrupt);
    CHECK_EQ(t->chunks()[1].record, rv::kNone32);
    CHECK_EQ(rvtest::invariant_failure(*t), std::string());
}

TEST(search_and_route_id_gaps_are_errors) {
    synth::WSrch s;
    s.search_id = 3; // expected 0
    s.src = {1};
    synth::WRnb r;
    r.route_id = 9; // expected 0
    r.name = "x";
    synth::WRne e;
    e.route_id = 9; // after the mismatch, route ids resolve by the stored id
    auto t = open_bytes(with_run({{"SRCH", synth::encode(s)}, {"RNB ", synth::encode(r)}, {"RNE ", synth::encode(e)}}));
    print_issues(*t);
    CHECK_EQ(t->error_count(), 2u);
    CHECK_EQ(t->warning_count(), 0u);
    CHECK(t->searches()[0].valid);
    CHECK(t->routes()[0].valid);
    CHECK_EQ(t->routes()[0].end, 0u);
    CHECK_EQ(t->route_index(9), 0u);
    // The counted id no longer names it.
    e.route_id = 0;
    auto t2 = open_bytes(with_run({{"SRCH", synth::encode(s)}, {"RNB ", synth::encode(r)}, {"RNE ", synth::encode(e)}}));
    CHECK_EQ(t2->error_count(), 3u);
}

TEST(net_chunks_and_totals) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    const auto &nc = t->net_chunks();
    CHECK(nc.count(rv::kNone32) == 0);
    CHECK(nc.count(synth::BLOCK) == 0); // BLOCK is not a net
    // net 3: DISC, VIA, VIA, CMIT 2, RIP, SRCH 5
    CHECK(nc.count(3) == 1 && nc.at(3).size() == 6);
    // net 1: PAD, RNB 1, SRCH 2, RBLD (under its route's net), CMIT 3, RNE 1, SRCH 3, CMIT 4, DROP
    CHECK(nc.count(1) == 1 && nc.at(1).size() == 9);
    for (const auto &n : nc)
        for (std::size_t k = 1; k < n.second.size(); ++k) CHECK(n.second[k] > n.second[k - 1]);
    CHECK_EQ(t->expanded_total(), 15 + 15 + 9 + 5 + 12 + 7);
    CHECK_EQ(t->trace_entries(), 6 + 4 + 3 + 2 + 4);
    CHECK_EQ(t->drops_failed(), 0u);
    CHECK_EQ(t->drops_raised(), 0u);
}

// --- robustness rules of 3.3, 10, 11 ------------------------------------------------

TEST(unknown_tag_is_skipped) {
    synth::Writer w;
    synth::Bytes junk;
    junk.zeros(13); // odd length, padded
    w.chunk("ZZZ9", junk);
    synth::WMark m;
    m.text = "after";
    w.chunk("MARK", synth::encode(m));
    auto t = open_bytes(w.out);
    CHECK_EQ(t->chunks().size(), 2u);
    CHECK(t->chunks()[0].kind == rv::Kind::Unknown);
    CHECK(!t->chunks()[0].corrupt);
    CHECK_EQ(t->chunks()[0].record, rv::kNone32);
    CHECK_EQ(t->marks().size(), 1u);
    CHECK_EQ(t->issues().size(), 0u);
}

TEST(unsupported_major_version_is_rejected) {
    synth::Writer w(1.0, 2, 0, 0);
    bool threw = false;
    std::string msg;
    try {
        (void)open_bytes(w.out);
    } catch (const rv::UnsupportedVersion &e) {
        threw = true;
        msg = e.what();
    }
    CHECK(threw);
    CHECK(msg.find("2.0") != std::string::npos);
    synth::Writer w0(1.0, 0, 9, 0);
    threw = false;
    try {
        (void)open_bytes(w0.out);
    } catch (const rv::UnsupportedVersion &) {
        threw = true;
    }
    CHECK(threw);
}

TEST(higher_minor_version_and_appended_fields_are_accepted) {
    // A 1.7 file whose every chunk has 8 extra bytes appended to its payload
    // (what a later minor version may do) reads like the 1.0 file.
    const synth::Sample s = synth::build_sample();
    auto base = open_bytes(s.bytes);
    synth::Writer w(s.open_time, 1, 7, 0xFFFF0000u);
    for (std::size_t k = 0; k < base->chunks().size(); ++k) {
        const rv::Chunk &c = base->chunks()[k];
        synth::Bytes p;
        p.b.assign(s.bytes.begin() + static_cast<std::ptrdiff_t>(c.payload_offset()),
                   s.bytes.begin() + static_cast<std::ptrdiff_t>(c.payload_offset() + c.len));
        if (c.kind != rv::Kind::Srch && c.kind != rv::Kind::Grds && c.kind != rv::Kind::Brd)
            p.u32(0x12345678).u32(0x9ABCDEF0);
        w.chunk_at(std::string(c.tag, 4).c_str(), p, c.t);
    }
    auto t = open_bytes(w.out);
    CHECK_EQ(t->minor(), 7);
    CHECK_EQ(t->header_flags(), 0xFFFF0000u);
    CHECK_EQ(t->issues().size(), 0u);
    print_issues(*t);
    CHECK_EQ(t->chunks().size(), base->chunks().size());
    for (std::size_t k = 0; k < t->chunks().size() && k < base->chunks().size(); ++k)
        CHECK_EQ(t->describe(static_cast<std::uint32_t>(k)), base->describe(static_cast<std::uint32_t>(k)));
    CHECK_EQ(rvtest::exercise(*t), 0);
}

TEST(trailing_bytes_in_srch_grds_brd_are_ignored) {
    synth::WSrch s;
    s.src = {1, 2};
    s.flags = (1u << 7) | (1u << 9);
    s.trace_cell = {1, 2, 3};
    s.trace_d = {0, 10, 20};
    s.ntrace_total = 3;
    synth::Bytes p = synth::encode(s);
    p.u32(0xFFFFFFFF).u32(0xFFFFFFFF); // later-version fields after the trace
    synth::Bytes g = synth::encode(synth::make_grids(0, 30, "x"));
    g.zeros(16);
    auto t = open_bytes(with_run({{"SRCH", p}, {"GRDS", g}}, 3));
    CHECK_EQ(t->issues().size(), 0u);
    auto wf = t->wavefront(0);
    CHECK(wf != nullptr && wf->cells == s.trace_cell && wf->costs == s.trace_d);
    CHECK(t->grids(0)->novia == synth::make_grids(0, 30, "x").novia);
}

TEST(unknown_enc_means_no_wavefront) {
    synth::WSrch s;
    s.src = {1};
    s.tgt = {2};
    s.path = {1, 2};
    s.flags = (1u << 5) | (1u << 9);
    s.enc = 3;
    s.ntrace_total = 40;
    s.enc_data = {1, 2, 3, 4, 5, 6, 7};
    auto t = open_bytes(with_run({{"SRCH", synth::encode(s)}}, 1));
    CHECK_EQ(t->issues().size(), 0u);
    CHECK(t->searches()[0].valid);
    CHECK(t->wavefront(0) == nullptr);
    CHECK_EQ(t->searches()[0].npath, 2u);
}

TEST(header_not_written_yet) {
    const synth::Sample s = synth::build_sample();
    for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 10u, 23u}) {
        auto t = open_bytes(std::vector<unsigned char>(s.bytes.begin(), s.bytes.begin() + static_cast<std::ptrdiff_t>(n)));
        CHECK(!t->header_complete());
        CHECK_EQ(t->chunks().size(), 0u);
        CHECK_EQ(t->issues().size(), 0u);
    }
    // a wrong byte inside the magic is not a trace file, even when short
    std::vector<unsigned char> bad(s.bytes.begin(), s.bytes.begin() + 5);
    bad[3] = 'X';
    bool threw = false;
    try {
        (void)open_bytes(bad);
    } catch (const rv::FormatError &) {
        threw = true;
    }
    CHECK(threw);
    // major 3 is rejected as soon as its two bytes exist
    std::vector<unsigned char> v3(s.bytes.begin(), s.bytes.begin() + 10);
    v3[8] = 3;
    threw = false;
    try {
        (void)open_bytes(v3);
    } catch (const rv::UnsupportedVersion &) {
        threw = true;
    }
    CHECK(threw);
}

TEST(large_counts_are_rejected_without_allocating) {
    std::vector<std::pair<std::string, synth::Bytes>> chunks;
    synth::Bytes nets;
    nets.u32(0xFFFFFFFFu).i32(1).str("a");
    chunks.push_back({"NETS", nets});
    synth::WSrch s;
    s.src = {1};
    synth::Bytes srch = synth::encode(s);
    srch.b[64] = 0xFF; // nsrc = 0xFFFFFFFF
    srch.b[65] = 0xFF;
    srch.b[66] = 0xFF;
    srch.b[67] = 0xFF;
    chunks.push_back({"SRCH", srch});
    synth::Bytes srch2 = synth::encode(s);
    srch2.b[76] = 0xFF; // ntrace = 0x7FFFFFFF
    srch2.b[77] = 0xFF;
    srch2.b[78] = 0xFF;
    srch2.b[79] = 0x7F;
    chunks.push_back({"SRCH", srch2});
    synth::Bytes rnb;
    rnb.u32(2).i32(1).u32(0).i32(0).u32(0x40000000u).u32(0);
    chunks.push_back({"RNB ", rnb});
    synth::Bytes rip;
    rip.i32(1).u32(0).u32(synth::NONE32).u32(0xFFFFFFF0u);
    chunks.push_back({"RIP ", rip});
    synth::WBrd brd;
    brd.layers = {{0, 0, "F.Cu"}};
    synth::WPolyset ps;
    ps.polys = {{{{0, 0}, {1, 1}, {2, 0}}}};
    brd.polysets = {ps};
    brd.order = {{1, 0}};
    synth::Bytes b = synth::encode(brd);
    // the polyset's npts sits after: brd fixed+label+tables+records header+24+4
    // find it by value 3 followed by points 0,0 -- simpler: overwrite every
    // u32 equal to 3 near the end with a huge count
    for (std::size_t k = b.size() - 40; k + 4 <= b.size(); k += 4)
        if (b.b[k] == 3 && b.b[k + 1] == 0 && b.b[k + 2] == 0 && b.b[k + 3] == 0) {
            b.b[k] = 0xFF;
            b.b[k + 1] = 0xFF;
            b.b[k + 2] = 0xFF;
            b.b[k + 3] = 0x0F;
        }
    chunks.push_back({"BRD ", b});
    synth::WRun big = synth::make_run(6, 5, 4);
    big.nd = 1000000;
    chunks.push_back({"RUN ", synth::encode(big)});
    synth::WRun huge = synth::make_run(6, 5, 4);
    huge.nx = 70000;
    huge.ny = 70000;
    chunks.push_back({"RUN ", synth::encode(huge)});
    synth::WMark m;
    m.text = "still read";
    chunks.push_back({"MARK", synth::encode(m)});
    auto t = open_bytes(with_run(chunks));
    CHECK_EQ(t->chunks().size(), chunks.size() + 1);
    for (std::size_t k = 1; k + 1 < t->chunks().size(); ++k) CHECK(t->chunks()[k].corrupt);
    CHECK_EQ(t->error_count(), chunks.size() - 1);
    CHECK_EQ(t->marks().size(), 1u);
    CHECK_EQ(t->searches().size(), 2u); // placeholders keep the ids
    CHECK_EQ(t->routes().size(), 1u);
    CHECK_EQ(t->runs().size(), 3u);
    CHECK_EQ(rvtest::invariant_failure(*t), std::string());
}

TEST(chunk_len_past_the_end_is_a_tail_not_an_error) {
    synth::Writer w;
    synth::WMark m;
    m.text = "ok";
    w.chunk("MARK", synth::encode(m));
    synth::Bytes h;
    h.chars("MARK", 4).u32(0xFFFFFFF0u).f64(9.0);
    w.raw(h);
    auto t = open_bytes(w.out);
    CHECK_EQ(t->chunks().size(), 1u);
    CHECK_EQ(t->issues().size(), 0u);
    CHECK(!t->complete());
    CHECK_EQ(t->indexed_end(), w.out.size() - 16); // just before the unfinished header
}

TEST(non_printable_tag_stops_reading) {
    synth::Writer w;
    synth::WMark m;
    m.text = "before";
    w.chunk("MARK", synth::encode(m));
    w.chunk("MA\x01K", synth::encode(m));
    w.chunk("MARK", synth::encode(m));
    auto t = open_bytes(w.out);
    CHECK_EQ(t->chunks().size(), 1u);
    CHECK(t->stopped());
    CHECK_EQ(t->error_count(), 1u);
    CHECK(!t->issues().empty() && t->issues()[0].offset == w.chunk_offsets[1]);
    CHECK_EQ(t->refresh(), 0u); // stays stopped
}

TEST(router_chunk_before_run_is_corrupt) {
    synth::Writer w;
    synth::WSrch s;
    s.src = {0};
    w.chunk("SRCH", synth::encode(s));
    synth::WMark m;
    w.chunk("MARK", synth::encode(m));
    auto t = open_bytes(w.out);
    CHECK_EQ(t->chunks().size(), 2u);
    CHECK(t->chunks()[0].corrupt);
    CHECK_EQ(t->searches().size(), 1u);
    CHECK(!t->searches()[0].valid);
    CHECK(!t->chunks()[1].corrupt);
}

TEST(cells_outside_the_grid_are_corrupt) {
    synth::WSrch s;
    s.src = {60}; // 6 x 5 grid: cells < 60
    synth::WCmit c;
    c.copper_id = 0;
    c.path = {59, 60};
    synth::WSrch ok;
    ok.search_id = 1;
    ok.src = {59};
    ok.flags = 1u << 9;
    ok.trace_cell = {59, 61}; // bad cell in the wavefront: found on load
    ok.ntrace_total = 2;
    auto t = open_bytes(with_run({{"SRCH", synth::encode(s)}, {"CMIT", synth::encode(c)}, {"SRCH", synth::encode(ok)}}));
    CHECK(t->chunks()[1].corrupt);
    CHECK(t->chunks()[2].corrupt);
    CHECK(!t->chunks()[3].corrupt);
    bool threw = false;
    try {
        (void)t->wavefront(1);
    } catch (const rv::FormatError &) {
        threw = true;
    }
    CHECK(threw);
}

TEST(grds_nc_must_match_the_run) {
    auto t = open_bytes(with_run({{"GRDS", synth::encode(synth::make_grids(0, 31, "bad"))}}));
    CHECK(t->chunks()[1].corrupt);
    CHECK_EQ(t->grids_infos().size(), 0u);
}

TEST(rip_checks) {
    synth::WCmit c0;
    c0.copper_id = 0;
    c0.net = 1;
    c0.path = {1};
    synth::WCmit c1 = c0;
    c1.copper_id = 1;
    synth::WRip rip;
    rip.net = 1;
    rip.removed = {1, 5, 0}; // after a SNAP only copper 1 is rippable; 5 does not exist; not ascending
    auto t = open_bytes(with_run({{"CMIT", synth::encode(c0)},
                                  {"SNAP", synth::encode_snap()},
                                  {"GRDS", synth::encode(synth::make_grids(1, 30, "snapshot"))},
                                  {"CMIT", synth::encode(c1)},
                                  {"RIP ", synth::encode(rip)}}));
    CHECK_EQ(t->error_count(), 1u);   // id 5 does not exist
    CHECK_EQ(t->warning_count(), 2u); // not ascending; not exactly {1}
    CHECK_EQ(t->copper()[1].removed_by, 5u);
    CHECK_EQ(t->copper()[0].removed_by, 5u); // still applied: the file says so
}

// --- on-demand caches ----------------------------------------------------------------

TEST(wavefront_cache_is_lru) {
    const synth::Sample s = synth::build_sample();
    auto t = open_bytes(s.bytes);
    t->set_wavefront_cache(2, 1u << 20);
    (void)t->wavefront(0); // miss
    (void)t->wavefront(2); // miss
    (void)t->wavefront(1); // hit: repeat of 0
    (void)t->wavefront(3); // miss, evicts 2 (least recent)
    std::vector<std::uint32_t> keys;
    t->wavefront_cache_keys([&](std::uint32_t k) { keys.push_back(k); });
    CHECK(keys == (std::vector<std::uint32_t>{3, 0}));
    CHECK_EQ(t->wavefront_cache_stats().hits, 1u);
    CHECK_EQ(t->wavefront_cache_stats().misses, 3u);
    CHECK_EQ(t->wavefront_cache_stats().evictions, 1u);
    // an evicted wavefront still held by a caller stays valid
    auto held = t->wavefront(2); // miss, evicts 0
    (void)t->wavefront(4);       // miss, evicts 3
    (void)t->wavefront(6);       // miss, evicts 2
    CHECK(held != nullptr && held->cells == s.srchs[2].trace_cell);
    // byte budget: 12 bytes keeps only the most recent entry
    t->set_wavefront_cache(16, 12);
    keys.clear();
    t->wavefront_cache_keys([&](std::uint32_t k) { keys.push_back(k); });
    CHECK(keys == (std::vector<std::uint32_t>{6}));
    CHECK(t->wavefront_cache_stats().bytes <= 32u);
}

TEST(repeat_of_must_point_back_to_a_traced_search) {
    synth::WSrch a;
    a.src = {1};
    a.flags = 1u << 9;
    a.trace_cell = {1, 2};
    a.ntrace_total = 2;
    synth::WSrch fwd; // points forward
    fwd.search_id = 1;
    fwd.src = {1};
    fwd.flags = 1u << 9;
    fwd.repeat_of = 5;
    fwd.ntrace_total = 2;
    synth::WSrch chain; // points at a repeat
    chain.search_id = 2;
    chain.src = {1};
    chain.flags = 1u << 9;
    chain.repeat_of = 1;
    chain.ntrace_total = 2;
    synth::WSrch good;
    good.search_id = 3;
    good.src = {1};
    good.flags = 1u << 9;
    good.repeat_of = 0;
    good.ntrace_total = 2;
    auto t = open_bytes(with_run({{"SRCH", synth::encode(a)},
                                  {"SRCH", synth::encode(fwd)},
                                  {"SRCH", synth::encode(chain)},
                                  {"SRCH", synth::encode(good)},
                                  {"RUN ", synth::encode(synth::make_run(6, 5, 4))},
                                  {"SRCH", [] {
                                       synth::WSrch other; // another session
                                       other.search_id = 4;
                                       other.src = {1};
                                       other.flags = 1u << 9;
                                       other.repeat_of = 0;
                                       other.ntrace_total = 2;
                                       return synth::encode(other);
                                   }()}}));
    CHECK_EQ(t->warning_count(), 3u);
    CHECK(t->wavefront(1) == nullptr);
    CHECK(t->wavefront(2) == nullptr);
    CHECK(t->wavefront(3) != nullptr && t->wavefront(3) == t->wavefront(0));
    CHECK(t->wavefront(4) == nullptr);
}

// --- following a file that grows ------------------------------------------------------

TEST(tailing_in_memory_equals_one_shot) {
    const synth::Sample s = synth::build_sample();
    auto full = open_bytes(s.bytes);
    const std::string want = rvtest::fingerprint(*full);
    // several piece sizes, including 1 byte at a time
    for (std::size_t step : {1u, 3u, 7u, 16u, 61u, 500u}) {
        auto data = std::make_shared<std::vector<unsigned char>>();
        auto t = rvtest::open_shared(data);
        std::uint32_t added = 0;
        for (std::size_t at = 0; at < s.bytes.size(); at += step) {
            const std::size_t n = std::min(step, s.bytes.size() - at);
            data->insert(data->end(), s.bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         s.bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
            added += t->refresh();
            CHECK_EQ(t->error_count(), 0u);
        }
        CHECK_EQ(added, s.tags.size());
        CHECK(t->complete());
        CHECK_EQ(rvtest::fingerprint(*t), want);
    }
}

TEST(tailing_a_file_on_disk) {
    rvtest::TempDir dir;
    CHECK(dir.ok());
    if (!dir.ok()) return;
    const synth::Sample s = synth::build_sample();
    const std::string path = dir.path("live.grtrace");
    CHECK(rvtest::append_file(path, s.bytes.data(), 11)); // header not complete yet
    auto t = rv::Trace::open_file(path);
    CHECK(!t->header_complete());
    std::size_t at = 11;
    std::uint32_t added = 0;
    // write chunk by chunk, each in two parts (half a chunk is not read)
    for (std::size_t k = 0; k <= s.chunk_offsets.size(); ++k) {
        const std::size_t end = k < s.chunk_offsets.size() ? s.chunk_offsets[k] : s.bytes.size();
        const std::size_t mid = at + (end - at) / 2;
        CHECK(rvtest::append_file(path, s.bytes.data() + at, mid - at));
        added += t->refresh();
        CHECK(rvtest::append_file(path, s.bytes.data() + mid, end - mid));
        added += t->refresh();
        at = end;
    }
    CHECK_EQ(added, s.tags.size());
    CHECK(t->complete());
    auto once = rv::Trace::open_file(path);
    CHECK_EQ(rvtest::fingerprint(*t), rvtest::fingerprint(*once));
    CHECK_EQ(rvtest::exercise(*t), 0);
}

TEST(refresh_detects_shrink_and_rewrite) {
    const synth::Sample s = synth::build_sample(false);
    {
        auto data = std::make_shared<std::vector<unsigned char>>(s.bytes);
        auto t = rvtest::open_shared(data);
        const std::size_t n = t->chunks().size();
        data->resize(data->size() - 10);
        bool threw = false;
        try {
            t->refresh();
        } catch (const rv::FormatError &) {
            threw = true;
        }
        CHECK(threw);
        CHECK(t->failed());
        CHECK_EQ(t->chunks().size(), n); // what was read stays
        // keeps failing even if the file is restored
        data->assign(s.bytes.begin(), s.bytes.end());
        threw = false;
        try {
            t->refresh();
        } catch (const rv::FormatError &) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        auto data = std::make_shared<std::vector<unsigned char>>(s.bytes);
        auto t = rvtest::open_shared(data);
        (*data)[20] ^= 0x01; // open_time changed: rewritten
        bool threw = false;
        try {
            t->refresh();
        } catch (const rv::FormatError &) {
            threw = true;
        }
        CHECK(threw);
        CHECK(t->failed());
    }
    {
        // the last chunk header changed with the same header: rewritten
        auto data = std::make_shared<std::vector<unsigned char>>(s.bytes);
        auto t = rvtest::open_shared(data);
        (*data)[s.chunk_offsets.back() + 4] ^= 0x08;
        bool threw = false;
        try {
            t->refresh();
        } catch (const rv::FormatError &) {
            threw = true;
        }
        CHECK(threw);
    }
}

TEST(bytes_after_end_are_reported) {
    const synth::Sample s = synth::build_sample();
    auto data = std::make_shared<std::vector<unsigned char>>(s.bytes);
    auto t = rvtest::open_shared(data);
    CHECK_EQ(t->warning_count(), 0u);
    data->insert(data->end(), 24, 0);
    CHECK_EQ(t->refresh(), 0u);
    CHECK_EQ(t->warning_count(), 1u);
    CHECK_EQ(t->refresh(), 0u);
    CHECK_EQ(t->warning_count(), 1u);
}

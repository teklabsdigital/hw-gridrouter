// synth_timeline.cpp: controlled routing runs for the M3 tests (synth.hpp).
//
// Written from docs/TRACE_FORMAT.md alone, like the rest of synth; shares no
// code with the reader.

#include "synth.hpp"

#include <algorithm>
#include <cmath>

namespace synth {

namespace {

std::int32_t nm(double origin_mm, double router_mm) {
    return static_cast<std::int32_t>(std::llround(origin_mm * 1e6) + std::llround(router_mm * 1e6));
}

} // namespace

void TimelineWriter::nets(const std::vector<std::pair<std::int32_t, std::string>> &entries) {
    WNets n;
    n.entries = entries;
    w.chunk("NETS", encode(n));
}

void TimelineWriter::start(std::int32_t nx, std::int32_t ny) {
    run = make_run(nx, ny, 8);
    run.cell = 0.25;
    run.origin_x = 100.5;
    run.origin_y = 50.25;
    w.chunk("RUN ", encode(run));
    since_snap.clear();
    alive.clear();
    pads.clear();
}

std::uint32_t TimelineWriter::cell(const Cell &c) const {
    return c.layer * static_cast<std::uint32_t>(run.nx * run.ny) + static_cast<std::uint32_t>(c.j * run.nx + c.i);
}

std::uint32_t TimelineWriter::commit(std::int32_t net, const std::vector<Cell> &path, double width,
                                     const std::vector<std::pair<std::int32_t, std::int32_t>> &vias,
                                     const std::vector<WStub> &stubs, std::uint32_t ctx, std::uint32_t route) {
    WCmit c;
    c.copper_id = next_copper;
    c.net = net;
    c.width_mm = width;
    c.ctx = ctx;
    c.route_id = route;
    c.vias = vias;
    c.stubs = stubs;
    for (const Cell &p : path) c.path.push_back(cell(p));
    w.chunk("CMIT", encode(c));
    since_snap[net].push_back(next_copper);
    Item it;
    it.net = net;
    it.path = path;
    it.vias = vias;
    it.stubs = stubs;
    it.width = width;
    alive[next_copper] = it;
    return next_copper++;
}

std::uint32_t TimelineWriter::via(std::int32_t net, std::int32_t i, std::int32_t j) {
    WVia v;
    v.copper_id = next_copper;
    v.net = net;
    v.i = i;
    v.j = j;
    v.added = 1;
    w.chunk("VIA ", encode(v));
    since_snap[net].push_back(next_copper);
    Item it;
    it.net = net;
    it.vias = {{i, j}};
    alive[next_copper] = it;
    return next_copper++;
}

void TimelineWriter::rip(std::int32_t net, std::uint32_t ctx, std::uint32_t route,
                         const std::vector<std::uint32_t> &extra) {
    WRip r;
    r.net = net;
    r.ctx = ctx;
    r.route_id = route;
    r.removed = since_snap[net];
    since_snap[net].clear();
    for (std::uint32_t id : extra) {
        r.removed.push_back(id);
        for (auto &kv : since_snap) kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), id), kv.second.end());
    }
    std::sort(r.removed.begin(), r.removed.end());
    for (std::uint32_t id : r.removed) alive.erase(id);
    w.chunk("RIP ", encode(r));
}

void TimelineWriter::snap() {
    w.chunk("SNAP", encode_snap());
    w.chunk("GRDS", encode(make_grids(1, static_cast<std::uint32_t>(run.nx * run.ny), "snapshot")));
    since_snap.clear();
}

std::uint32_t TimelineWriter::rnb(std::int32_t net, const std::string &name, const std::vector<WRnbPad> &pads_) {
    WRnb r;
    r.route_id = next_route;
    r.net = net;
    r.name = name;
    r.pads = pads_;
    w.chunk("RNB ", encode(r));
    return next_route++;
}

void TimelineWriter::rne(std::uint32_t route, std::int32_t net, const std::vector<std::string> &failed) {
    WRne e;
    e.route_id = route;
    e.net = net;
    e.failed = failed;
    w.chunk("RNE ", encode(e));
}

std::uint32_t TimelineWriter::srch(std::int32_t net, const std::vector<Cell> &src, const std::vector<Cell> &tgt,
                                   const std::vector<Cell> &path, std::int64_t expanded, std::uint32_t ctx,
                                   std::uint32_t route) {
    WSrch s;
    s.search_id = next_search;
    s.route_id = route;
    s.ctx = ctx;
    s.net = net;
    s.expanded = expanded;
    s.duration_s = 1e-4 * static_cast<double>(expanded);
    expanded_total += expanded;
    if (!path.empty()) {
        s.flags |= 1u << 5; // found
        s.cost = static_cast<std::int64_t>(path.size()) * 10;
    }
    for (const Cell &c : src) s.src.push_back(cell(c));
    for (const Cell &c : tgt) s.tgt.push_back(cell(c));
    for (const Cell &c : path) s.path.push_back(cell(c));
    w.chunk("SRCH", encode(s));
    return next_search++;
}

void TimelineWriter::rbld(std::uint32_t route, const std::vector<std::string> &names,
                          const std::vector<std::int32_t> &nets_) {
    WRbld b;
    b.route_id = route;
    b.names = names;
    b.nets = nets_;
    w.chunk("RBLD", encode(b));
}

void TimelineWriter::drop(std::int32_t net, std::uint32_t ok, const std::string &name, const std::string &label) {
    WDrop d;
    d.net = net;
    d.ok = ok;
    d.name = name;
    d.pad_label = label;
    w.chunk("DROP", encode(d));
}

void TimelineWriter::mark(std::uint32_t level, const std::string &text) {
    WMark m;
    m.level = level;
    m.text = text;
    w.chunk("MARK", encode(m));
}

void TimelineWriter::pad(std::int32_t net, double cx, double cy, double half) {
    WPad p;
    p.net = net;
    p.layers = 1;
    p.cx = cx;
    p.cy = cy;
    p.x0 = cx - half;
    p.y0 = cy - half;
    p.x1 = cx + half;
    p.y1 = cy + half;
    p.d_t = run.pad_clear[0];
    p.d_w = run.pad_clear[1];
    p.d_novia = run.pad_novia;
    pads.push_back(p);
    w.chunk("PAD ", encode(p));
}

void TimelineWriter::brd(const WBrd &b) {
    w.chunk("BRD ", encode(b));
}

void TimelineWriter::rout() {
    WRout r;
    const double c = run.cell;
    for (const auto &kv : alive) {
        const Item &it = kv.second;
        for (std::size_t k = 1; k < it.path.size(); ++k) {
            const Cell &a = it.path[k - 1], &b = it.path[k];
            if (a.layer != b.layer) continue;
            WRoutSeg s;
            s.layer = a.layer;
            s.net = it.net;
            s.x0 = a.i * c;
            s.y0 = a.j * c;
            s.x1 = b.i * c;
            s.y1 = b.j * c;
            s.w = it.width;
            r.segs.push_back(s);
        }
        for (const WStub &st : it.stubs) {
            WRoutSeg s;
            s.layer = st.layer;
            s.net = it.net;
            s.x0 = st.x0;
            s.y0 = st.y0;
            s.x1 = st.x1;
            s.y1 = st.y1;
            s.w = st.w;
            r.segs.push_back(s);
        }
        for (const auto &v : it.vias) {
            WRoutVia rv;
            rv.x = v.first * c;
            rv.y = v.second * c;
            rv.net = it.net;
            r.vias.push_back(rv);
        }
    }
    w.chunk("ROUT", encode(r));
}

void TimelineWriter::end(const std::vector<std::pair<std::string, std::string>> &failed) {
    WEnd e;
    e.searches = next_search;
    e.routes = next_route;
    e.expanded_total = expanded_total;
    e.failed = failed;
    e.nfailed = static_cast<std::uint32_t>(failed.size());
    w.chunk("END ", encode(e));
}

WBrd TimelineWriter::board(const std::string &label, bool routed) const {
    WBrd b;
    b.label = label;
    b.origin_x_nm = static_cast<std::int32_t>(std::llround(run.origin_x * 1e6));
    b.origin_y_nm = static_cast<std::int32_t>(std::llround(run.origin_y * 1e6));
    b.layers = {{0, 0, "F.Cu"}, {2, 0, "B.Cu"}, {25, -1, "Edge.Cuts"}};
    b.nets = {{0, ""}, {1, "GND"}, {2, "/SIG"}, {3, "VCC"}, {4, "/CLK"}};
    const double c = run.cell;
    auto X = [&](double mm) { return nm(run.origin_x, mm); };
    auto Y = [&](double mm) { return nm(run.origin_y, mm); };
    // Outline: the grid extent.
    WPolyset edge;
    edge.layer_index = 2;
    edge.role = 5;
    const double x0 = -c / 2, y0 = -c / 2, x1 = (run.nx - 0.5) * c, y1 = (run.ny - 0.5) * c;
    edge.polys = {{{{X(x0), Y(y0)}, {X(x1), Y(y0)}, {X(x1), Y(y1)}, {X(x0), Y(y1)}}}};
    b.polysets.push_back(edge);
    for (const WPad &p : pads) {
        WPolyset ps;
        ps.layer_index = 0;
        ps.net = p.net;
        ps.role = 0;
        ps.polys = {{{{X(p.x0), Y(p.y0)}, {X(p.x1), Y(p.y0)}, {X(p.x1), Y(p.y1)}, {X(p.x0), Y(p.y1)}}}};
        b.polysets.push_back(ps);
    }
    if (routed) {
        for (const auto &kv : alive) {
            const Item &it = kv.second;
            const std::int32_t w_nm = static_cast<std::int32_t>(std::llround(it.width * 1e6));
            for (std::size_t k = 1; k < it.path.size(); ++k) {
                const Cell &a = it.path[k - 1], &q = it.path[k];
                if (a.layer != q.layer) continue;
                WTrack t;
                t.layer_index = static_cast<std::int32_t>(a.layer);
                t.net = it.net;
                t.x0 = X(a.i * c);
                t.y0 = Y(a.j * c);
                t.x1 = X(q.i * c);
                t.y1 = Y(q.j * c);
                t.w = w_nm;
                b.tracks.push_back(t);
            }
            for (const WStub &st : it.stubs) {
                WTrack t;
                t.layer_index = static_cast<std::int32_t>(st.layer);
                t.net = it.net;
                t.x0 = X(st.x0);
                t.y0 = Y(st.y0);
                t.x1 = X(st.x1);
                t.y1 = Y(st.y1);
                t.w = static_cast<std::int32_t>(std::llround(st.w * 1e6));
                b.tracks.push_back(t);
            }
            for (const auto &v : it.vias) {
                WBVia bv;
                bv.x = X(v.first * c);
                bv.y = Y(v.second * c);
                bv.dia = static_cast<std::int32_t>(std::llround(run.via_dia * 1e6));
                bv.drill = static_cast<std::int32_t>(std::llround(run.via_drill * 1e6));
                bv.net = it.net;
                bv.top = 0;
                bv.bottom = 1;
                b.vias.push_back(bv);
            }
        }
    }
    for (std::uint32_t k = 0; k < b.polysets.size(); ++k) b.order.push_back({1, k});
    for (std::uint32_t k = 0; k < b.tracks.size(); ++k) b.order.push_back({4, k});
    for (std::uint32_t k = 0; k < b.vias.size(); ++k) b.order.push_back({5, k});
    return b;
}

std::vector<unsigned char> build_timeline_sample() {
    TimelineWriter tw;
    auto C = [](std::uint32_t l, std::int32_t i, std::int32_t j) {
        Cell c;
        c.layer = l;
        c.i = i;
        c.j = j;
        return c;
    };
    // A straight-and-diagonal run from (i0, j) along layer l to x = i1.
    auto line = [&](std::uint32_t l, std::int32_t i0, std::int32_t i1, std::int32_t j) {
        std::vector<Cell> p;
        for (std::int32_t i = i0; i0 <= i1 ? i <= i1 : i >= i1; i += i0 <= i1 ? 1 : -1) p.push_back(C(l, i, j));
        return p;
    };
    auto cat = [](std::vector<Cell> a, const std::vector<Cell> &b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    const double c = 0.25;

    tw.nets({{1, "GND"}, {2, "/SIG"}, {3, "VCC"}, {4, "/CLK"}});
    tw.mark(0, "setup");
    tw.run = make_run(40, 24, 8);
    tw.run.cell = c;
    tw.run.origin_x = 100.5;
    tw.run.origin_y = 50.25;
    tw.brd(tw.board("placed", false)); // no pads yet: an outline only
    // --- session 0 ---
    tw.start(40, 24);
    tw.pad(2, 2 * c, 3 * c, 0.4);   // /SIG U1.1
    tw.pad(2, 34 * c, 6 * c, 0.4);  // /SIG J1.1
    tw.pad(3, 4 * c, 20 * c, 0.4);  // VCC U2.1
    tw.pad(3, 30 * c, 20 * c, 0.4); // VCC C1.1
    tw.pad(4, 6 * c, 10 * c, 0.4);  // /CLK U1.2
    tw.pad(4, 36 * c, 14 * c, 0.4); // /CLK X1.1
    tw.pad(1, 20 * c, 17 * c, 0.4); // GND U3.1
    tw.pad(1, 12 * c, 22 * c, 0.4); // GND J2.1
    tw.mark(1, "a note: not an event");
    tw.snap();
    tw.mark(0, "route");

    // /SIG: F.Cu from U1.1 east, a diagonal, a via, B.Cu to J1.1.
    std::uint32_t r = tw.rnb(2, "/SIG", {{2 * c, 3 * c, 1, 4, "U1.1"}, {34 * c, 6 * c, 1, 4, "J1.1"}});
    std::vector<Cell> sig = cat(cat(line(0, 2, 20, 3), {C(0, 21, 4), C(0, 22, 5), C(0, 23, 6)}),
                                cat({C(1, 23, 6)}, line(1, 24, 34, 6)));
    tw.srch(2, {C(0, 2, 3)}, {C(0, 34, 6), C(1, 34, 6)}, sig, 1800, 1, r);
    tw.commit(2, sig, 0.2, {{23, 6}}, {{0, 1.75 * c, 3 * c, 2 * c, 3 * c, 0.2}}, 1, r);
    tw.rne(r, 2, {});

    tw.via(3, 10, 15); // a standalone via of VCC

    r = tw.rnb(3, "VCC", {{4 * c, 20 * c, 1, 4, "U2.1"}, {30 * c, 20 * c, 1, 4, "C1.1"}});
    std::vector<Cell> vcc = line(0, 4, 30, 20);
    tw.srch(3, {C(0, 4, 20)}, {C(0, 30, 20)}, vcc, 900, 1, r);
    tw.commit(3, vcc, 0.5, {}, {}, 1, r);
    tw.rne(r, 3, {});

    // /CLK: blocked by /SIG; rebuild rips /SIG, then /CLK goes through.
    r = tw.rnb(4, "/CLK", {{6 * c, 10 * c, 1, 4, "U1.2"}, {36 * c, 14 * c, 1, 4, "X1.1"}});
    tw.srch(4, {C(0, 6, 10)}, {C(0, 36, 14)}, {}, 25000, 1, r);
    tw.rbld(r, {"/SIG"}, {2});
    tw.rip(2, 1, r);
    std::vector<Cell> clk = cat(line(0, 6, 32, 10), {C(0, 33, 11), C(0, 34, 12), C(0, 35, 13), C(0, 36, 14)});
    tw.srch(4, {C(0, 6, 10)}, {C(0, 36, 14)}, clk, 4000, 1, r);
    tw.commit(4, clk, 0.2, {}, {}, 1, r);
    tw.rne(r, 4, {});

    // /SIG again, around /CLK on B.Cu.
    r = tw.rnb(2, "/SIG", {{2 * c, 3 * c, 1, 4, "U1.1"}, {34 * c, 6 * c, 1, 4, "J1.1"}});
    std::vector<Cell> sig2 = cat(cat(line(0, 2, 8, 3), {C(1, 8, 3)}), line(1, 9, 34, 3));
    sig2.push_back(C(1, 34, 4));
    sig2.push_back(C(1, 34, 5));
    sig2.push_back(C(1, 34, 6));
    tw.srch(2, {C(0, 2, 3)}, {C(0, 34, 6), C(1, 34, 6)}, sig2, 2600, 1, r);
    tw.commit(2, sig2, 0.2, {{8, 3}}, {}, 1, r);
    tw.rne(r, 2, {});

    // GND: one pad fails.
    r = tw.rnb(1, "GND", {{20 * c, 17 * c, 1, 4, "U3.1"}, {12 * c, 22 * c, 1, 4, "J2.1"}, {38 * c, 2 * c, 1, 4, "TP1"}});
    std::vector<Cell> gnd = line(0, 20, 37, 17);
    tw.srch(1, {C(0, 20, 17)}, {C(0, 38, 2)}, {}, 300, 1, r);
    tw.srch(1, {C(0, 20, 17)}, {C(0, 12, 22)}, {}, 6000, 1, r);
    tw.rne(r, 1, {"TP1", "J2.1"});

    tw.mark(0, "planes");
    // drop_to_plane of a GND pad: its search fails.
    tw.srch(1, {C(0, 11, 22), C(0, 12, 22), C(0, 13, 22)}, {C(1, 12, 22)}, {}, 500, 2);
    tw.drop(1, 0, "GND", "J2.1");
    // A direct rip of VCC: the standalone via and the VCC track go.
    tw.rip(3);

    // GND again: clears both failures (the RNE ones and the DROP one).
    r = tw.rnb(1, "GND", {{20 * c, 17 * c, 1, 4, "U3.1"}, {12 * c, 22 * c, 1, 4, "J2.1"}});
    std::vector<Cell> gnd2 = cat(line(0, 20, 14, 17), {C(0, 13, 18), C(0, 12, 19), C(0, 12, 20), C(0, 12, 21), C(0, 12, 22)});
    tw.srch(1, {C(0, 20, 17)}, {C(0, 12, 22)}, gnd2, 1200, 1, r);
    tw.commit(1, gnd2, 0.5, {}, {}, 1, r);
    tw.rne(r, 1, {});
    (void)gnd;

    tw.rout();
    tw.brd(tw.board("final", true));

    // --- session 1: a second, short router session ---
    tw.mark(0, "second session");
    tw.start(40, 24);
    const std::uint32_t a = tw.commit(2, line(1, 5, 15, 8), 0.2);
    const std::uint32_t b = tw.commit(4, line(1, 5, 15, 12), 0.2);
    (void)a;
    // A rip of /SIG that also lists /CLK's copper (valid, with a warning).
    tw.rip(2, 0, NONE32, {b});
    tw.commit(4, line(0, 5, 15, 12), 0.2);
    tw.end({});
    return tw.w.out;
}

} // namespace synth

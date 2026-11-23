// test_inspect.cpp: M5 core: the nets table, the selection and its
// highlight, hit testing, the attempt history, and their C interface.

#include "check.hpp"
#include "inspect.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "rv.h"
#include "synth.hpp"
#include "timeline.hpp"
#include "trace_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using synth::Cell;

struct S {
    rv_session *s = nullptr;
    S() { rv_create(&s); }
    ~S() { rv_destroy(s); }
};

Cell C(std::uint32_t l, std::int32_t i, std::int32_t j) {
    Cell c;
    c.layer = l;
    c.i = i;
    c.j = j;
    return c;
}

std::vector<Cell> line(std::uint32_t l, std::int32_t i0, std::int32_t i1, std::int32_t j) {
    std::vector<Cell> p;
    for (std::int32_t i = i0; i <= i1; ++i) p.push_back(C(l, i, j));
    return p;
}

const double kCell = 0.25, kOx = 100.5, kOy = 50.25;
double bx(double i) { return kOx + i * kCell; }
double by(double j) { return kOy + j * kCell; }

// A small run for picking and highlighting: a board with footprints, pad
// numbers, a zone, a track and a via, written before the routing (so the
// router copper stays drawn), then /SIG and GND routed (GND with a failed
// pad), wavefronts stored.
//   /SIG (2): pads U1.1 at cell (2, 3) and J1.1 at (30, 3); router copper
//             F.Cu j = 3, i = 2..30; board track F.Cu j = 6, i = 5..15;
//             board via at (20, 9)
//   GND (1):  pad U3.1 at (10, 15); router copper B.Cu j = 15, i = 10..30;
//             board zone B.Cu over j = 18..23; failed pad J2.1 at (36, 20)
std::vector<unsigned char> pick_trace() {
    synth::TimelineWriter tw;
    tw.wavefronts = true;
    tw.nets({{1, "GND"}, {2, "/SIG"}, {3, "VCC"}});
    tw.start(40, 24);
    tw.pad(2, 2 * kCell, 3 * kCell, 0.4);
    tw.pad(2, 30 * kCell, 3 * kCell, 0.4);
    tw.pad(1, 10 * kCell, 15 * kCell, 0.4);
    synth::WBrd b = tw.board("placed", false);
    auto X = [](double i) { return static_cast<std::int32_t>(std::llround(bx(i) * 1e6)); };
    auto Y = [](double j) { return static_cast<std::int32_t>(std::llround(by(j) * 1e6)); };
    b.footprints = {{0, 0, 0, 0, "U1", "IC", "lib:U"}, {0, 0, 0, 0, "J1", "CONN", "lib:J"},
                    {0, 0, 0, 0, "U3", "IC", "lib:U"}};
    // board() writes the outline first, then one polyset per pad in order.
    b.polysets[1].fp = 0;
    b.polysets[2].fp = 1;
    b.polysets[3].fp = 2;
    b.padinfos = {{0, 2, X(2), Y(3), 0, "1"}, {0, 2, X(2) + 2000000, Y(3), 0, "2"}, {1, 2, X(30), Y(3), 0, "1"},
                  {2, 1, X(10), Y(15), 0, "1"}};
    synth::WPolyset zone;
    zone.layer_index = 1;
    zone.net = 1;
    zone.role = 3;
    zone.polys = {{{{X(0), Y(18)}, {X(39), Y(18)}, {X(39), Y(23)}, {X(0), Y(23)}}}};
    b.polysets.push_back(zone);
    synth::WTrack tr;
    tr.layer_index = 0;
    tr.net = 2;
    tr.x0 = X(5);
    tr.y0 = Y(6);
    tr.x1 = X(15);
    tr.y1 = Y(6);
    tr.w = 200000;
    b.tracks.push_back(tr);
    synth::WBVia v;
    v.x = X(20);
    v.y = Y(9);
    v.dia = 600000;
    v.drill = 300000;
    v.net = 2;
    v.top = 0;
    v.bottom = 1;
    b.vias.push_back(v);
    b.order.clear();
    for (std::uint32_t k = 0; k < b.polysets.size(); ++k) b.order.push_back({1, k});
    b.order.push_back({4, 0});
    b.order.push_back({5, 0});
    for (std::uint32_t k = 0; k < b.padinfos.size(); ++k) b.order.push_back({3, k});
    tw.brd(b);
    tw.mark(0, "route");
    std::uint32_t r = tw.rnb(2, "/SIG", {{2 * kCell, 3 * kCell, 1, 4, "U1.1"}, {30 * kCell, 3 * kCell, 1, 4, "J1.1"}});
    tw.srch(2, {C(0, 2, 3)}, {C(0, 30, 3)}, line(0, 2, 30, 3), 900, 1, r);
    tw.commit(2, line(0, 2, 30, 3), 0.2, {}, {}, 1, r);
    tw.rne(r, 2, {});
    r = tw.rnb(1, "GND", {{10 * kCell, 15 * kCell, 1, 4, "U3.1"}, {36 * kCell, 20 * kCell, 1, 4, "J2.1"}});
    tw.srch(1, {C(1, 10, 15)}, {C(1, 30, 15)}, line(1, 10, 30, 15), 1200, 1, r);
    tw.commit(1, line(1, 10, 30, 15), 0.3, {}, {}, 1, r);
    tw.srch(1, {C(1, 30, 15)}, {C(1, 36, 20)}, {}, 700, 1, r);
    tw.rne(r, 1, {"J2.1"});
    tw.end({});
    return tw.w.out;
}

bool open_bytes(S &s, rvtest::TempDir &dir, const std::string &name, const std::vector<unsigned char> &bytes) {
    const std::string path = dir.path(name);
    if (!synth::write_file(path, bytes)) return false;
    if (rv_open(s.s, path.c_str()) != RV_OK) return false;
    rv_view_set_size(s.s, 800, 600, 1);
    rv_view_fit(s.s);
    return true;
}

// Hits at a board point, as (kind, text) topmost first.
std::vector<rv_hit> hits_at(S &s, double x, double y) {
    double vx = 0, vy = 0;
    rv_board_to_view(s.s, x, y, &vx, &vy);
    uint32_t n = 0;
    std::vector<rv_hit> out;
    if (rv_hit_test(s.s, vx, vy, &n) != RV_OK) return out;
    for (uint32_t k = 0; k < n; ++k) {
        rv_hit h;
        if (rv_hit_get(s.s, k, &h) == RV_OK) out.push_back(h);
    }
    return out;
}

bool has_kind(const std::vector<rv_hit> &h, uint32_t kind) {
    for (const rv_hit &x : h)
        if (x.kind == kind) return true;
    return false;
}

double view_scale(S &s) {
    rv_view v;
    rv_view_get(s.s, &v);
    return v.scale;
}

// --- naive recounts -------------------------------------------------------------------

struct Naive {
    std::uint32_t routes = 0, searches = 0, found = 0, not_found = 0, commits = 0, vias = 0, failed = 0;
    std::int64_t expanded = 0;
    std::set<std::uint32_t> rips;
};

std::map<std::pair<std::uint32_t, std::int32_t>, Naive> naive_totals(const rv::Trace &t, const rv::Timeline &tl) {
    std::map<std::pair<std::uint32_t, std::int32_t>, Naive> m;
    auto sess = [&](std::uint32_t chunk) { return t.chunks()[chunk].session; };
    for (const rv::Route &r : t.routes())
        if (r.valid) ++m[{sess(r.chunk), r.net}].routes;
    for (const rv::Search &s : t.searches()) {
        if (!s.valid) continue;
        Naive &n = m[{sess(s.chunk), s.net}];
        ++n.searches;
        if (s.has(rv::srch_flags::kFound)) ++n.found;
        else ++n.not_found;
        if (s.expanded > 0) n.expanded += s.expanded;
    }
    for (const rv::Commit &c : t.commits()) {
        Naive &n = m[{sess(c.chunk), c.net}];
        ++n.commits;
        n.vias += static_cast<std::uint32_t>(c.vias.size());
    }
    for (const rv::Via &v : t.vias())
        if (v.added) ++m[{sess(v.chunk), v.net}].vias;
    for (const rv::RouteEnd &re : t.route_ends()) m[{sess(re.chunk), re.net}].failed += static_cast<std::uint32_t>(re.failed.size());
    for (const rv::Drop &d : t.drops())
        if (d.ok == 0) ++m[{sess(d.chunk), d.net}].failed;
    for (const rv::Copper &c : t.copper())
        if (c.removed_by != rv::kNone32) m[{c.session, c.net}].rips.insert(tl.event_of_chunk(c.removed_by));
    return m;
}

// The state of (session, net) at event e from the definitions in rv.h.
rv::NetNow naive_now(const rv::Trace &t, const rv::Timeline &tl, std::uint32_t session, std::int32_t net,
                     std::uint32_t e, bool &ambiguous) {
    rv::NetNow n;
    ambiguous = false;
    if (session == rv::kNone32) return n;
    std::uint32_t last = tl.size() - 1;
    if (session + 1 < t.runs().size()) last = tl.event_of_chunk(t.runs()[session + 1].chunk) - 1;
    e = std::min(e, last);
    auto ev = [&](std::uint32_t chunk) { return tl.event_of_chunk(chunk); };
    bool routing = false;
    std::uint32_t last_rne = rv::kNone32, last_rnb = rv::kNone32;
    bool rne_ok = false;
    for (const rv::Route &r : t.routes()) {
        if (!r.valid || r.net != net || t.chunks()[r.chunk].session != session || ev(r.chunk) > e) continue;
        last_rnb = last_rnb == rv::kNone32 ? ev(r.chunk) : std::max(last_rnb, ev(r.chunk));
        if (r.end == rv::kNone32 || ev(t.route_ends()[r.end].chunk) > e) routing = true;
    }
    for (const rv::RouteEnd &re : t.route_ends()) {
        if (re.net != net || t.chunks()[re.chunk].session != session || ev(re.chunk) > e) continue;
        if (last_rne == rv::kNone32 || ev(re.chunk) >= last_rne) {
            last_rne = ev(re.chunk);
            rne_ok = re.status == 0 && re.failed.empty();
        }
    }
    std::uint32_t last_birth = rv::kNone32, last_death = rv::kNone32;
    bool death_after_rne = false;
    for (std::size_t k = 0; k < t.copper().size(); ++k) {
        const rv::Copper &c = t.copper()[k];
        const rv::CopperLife &l = tl.copper_life()[k];
        if (c.net != net || c.session != session || l.birth == rv::kNone32 || l.birth > e) continue;
        last_birth = last_birth == rv::kNone32 ? l.birth : std::max(last_birth, l.birth);
        if (l.death != rv::kNone32 && l.death <= e) {
            last_death = last_death == rv::kNone32 ? l.death : std::max(last_death, l.death);
            if (last_rne != rv::kNone32 && l.death > last_rne) death_after_rne = true;
        } else {
            ++n.copper;
        }
    }
    for (const rv::Failure &f : tl.failures())
        if (f.net == net && f.session == session && f.birth <= e && (f.death == rv::kNone32 || f.death > e)) ++n.failures;
    const bool removed_last = last_death != rv::kNone32 && (last_birth == rv::kNone32 || last_death > last_birth);
    if (last_death != rv::kNone32 && last_death == last_birth) ambiguous = true;
    const bool complete = last_rne != rv::kNone32 && rne_ok && !death_after_rne &&
                          (last_rnb == rv::kNone32 || last_rnb <= last_rne);
    if (routing) n.state = rv::NetState::Routing;
    else if (n.failures > 0) n.state = rv::NetState::Failed;
    else if (removed_last) n.state = rv::NetState::Ripped;
    else if (complete) n.state = rv::NetState::Complete;
    else if (n.copper > 0) n.state = rv::NetState::Routed;
    else n.state = rv::NetState::NotStarted;
    return n;
}

int check_table(const std::vector<unsigned char> &bytes) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(bytes);
    rv::Timeline tl(*t, rv::FitLengthPacing());
    rv::NetTable table;
    table.build(*t, tl);
    const auto naive = naive_totals(*t, tl);
    int checked = 0;
    std::set<std::pair<std::uint32_t, std::int32_t>> seen;
    for (std::uint32_t r = 0; r < table.rows().size(); ++r) {
        const rv::NetRow &row = table.rows()[r];
        CHECK(seen.insert({row.session, row.net}).second);
        auto it = naive.find({row.session, row.net});
        const Naive z = it != naive.end() ? it->second : Naive();
        CHECK_EQ(row.routes, z.routes);
        CHECK_EQ(row.searches, z.searches);
        CHECK_EQ(row.found, z.found);
        CHECK_EQ(row.not_found, z.not_found);
        CHECK_EQ(row.commits, z.commits);
        CHECK_EQ(row.vias, z.vias);
        CHECK_EQ(row.failed_pads, z.failed);
        CHECK_EQ(row.expanded, z.expanded);
        CHECK_EQ(row.rips, static_cast<std::uint32_t>(z.rips.size()));
        CHECK(row.seconds >= 0);
        if (row.session != rv::kNone32 && !row.events.empty()) {
            CHECK_EQ(row.first_event, row.events.front());
            CHECK_EQ(row.last_event, row.events.back());
            CHECK(std::is_sorted(row.events.begin(), row.events.end()));
        }
        for (std::uint32_t e = 0; e < tl.size(); ++e) {
            bool amb = false;
            const rv::NetNow want = naive_now(*t, tl, row.session, row.net, e, amb);
            const rv::NetNow got = table.at(r, e);
            CHECK_EQ(got.copper, want.copper);
            CHECK_EQ(got.failures, want.failures);
            if (!amb) CHECK_EQ(static_cast<int>(got.state), static_cast<int>(want.state));
            ++checked;
        }
    }
    // Every (session, net) with a count has a row.
    for (const auto &kv : naive) CHECK(seen.count(kv.first) == 1);
    return checked;
}

} // namespace

// --- the table ----------------------------------------------------------------------------

TEST(nets_table_totals_and_states_against_a_naive_recount) {
    int checked = check_table(synth::build_timeline_sample());
    for (std::uint32_t seed = 1; seed <= 60; ++seed) checked += check_table(synth::random_timeline(seed));
    checked += check_table(pick_trace());
    std::printf("    %d (row, position) states compared\n", checked);
}

TEST(nets_table_of_the_sample_run) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline tl(*t, rv::FitLengthPacing());
    rv::NetTable table;
    table.build(*t, tl);
    auto find = [&](std::uint32_t session, std::int32_t net) -> const rv::NetRow * {
        for (const rv::NetRow &r : table.rows())
            if (r.session == session && r.net == net) return &r;
        return nullptr;
    };
    const rv::NetRow *sig = find(0, 2), *gnd = find(0, 1), *vcc = find(0, 3), *clk = find(0, 4);
    CHECK(sig != nullptr && gnd != nullptr && vcc != nullptr && clk != nullptr);
    if (sig == nullptr || gnd == nullptr || vcc == nullptr || clk == nullptr) return;
    CHECK_EQ(sig->name, std::string("/SIG"));
    CHECK_EQ(sig->routes, 2u);
    CHECK_EQ(sig->rips, 1u);      // ripped by /CLK's rebuild
    CHECK_EQ(gnd->failed_pads, 3u); // TP1, J2.1 from the RNE, J2.1 from the DROP
    CHECK_EQ(vcc->vias, 1u);
    CHECK_EQ(vcc->rips, 1u);      // the direct rip
    CHECK_EQ(clk->not_found, 1u);
    CHECK_EQ(clk->found, 1u);
    // At the end of session 0 (its rows show that session's end).
    const std::uint32_t end = tl.size() - 1;
    CHECK(table.at(static_cast<std::uint32_t>(sig - table.rows().data()), end).state == rv::NetState::Complete);
    CHECK(table.at(static_cast<std::uint32_t>(vcc - table.rows().data()), end).state == rv::NetState::Ripped);
    CHECK(table.at(static_cast<std::uint32_t>(gnd - table.rows().data()), end).state == rv::NetState::Complete);
    // Mid-route: /CLK is routing at its RNB.
    std::uint32_t rnb = rv::kNone32;
    for (std::uint32_t e = 0; e < tl.size(); ++e)
        if (tl.events()[e].kind == rv::EvKind::RouteBegin && tl.events()[e].net == 4) rnb = e;
    CHECK(table.at(static_cast<std::uint32_t>(clk - table.rows().data()), rnb).state == rv::NetState::Routing);
    // Before anything: not started.
    CHECK(table.at(static_cast<std::uint32_t>(clk - table.rows().data()), 0).state == rv::NetState::NotStarted);
    // Session 1 rows exist for /SIG and /CLK.
    CHECK(find(1, 2) != nullptr);
    CHECK(find(1, 4) != nullptr);
    CHECK_EQ(find(1, 4)->rips, 1u); // the rip of /SIG that listed /CLK's copper
}

TEST(nets_table_sort_and_filter) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::random_timeline(7));
    rv::Timeline tl(*t, rv::FitLengthPacing());
    rv::NetTable table;
    table.build(*t, tl);
    const std::uint32_t e = tl.size() / 2;
    for (std::uint32_t c = 0; c < rv::netcol::Count; ++c) {
        for (int d = 0; d < 2; ++d) {
            table.set_view(c, d != 0, "");
            table.update(e);
            const std::vector<std::uint32_t> &v = table.view();
            CHECK_EQ(v.size(), table.rows().size());
            for (std::size_t k = 1; k < v.size(); ++k) {
                const rv::NetRow &a = table.rows()[v[k - 1]], &b = table.rows()[v[k]];
                // Compare by the cell's sort value: re-derive it from the row.
                auto key = [&](std::uint32_t r) -> double {
                    const rv::NetRow &x = table.rows()[r];
                    const rv::NetNow n = table.now()[r];
                    switch (c) {
                    case rv::netcol::Net: return x.net;
                    case rv::netcol::Session: return x.session == rv::kNone32 ? 1e18 : x.session;
                    case rv::netcol::State: return static_cast<double>(n.state);
                    case rv::netcol::Routes: return x.routes;
                    case rv::netcol::Searches: return x.searches;
                    case rv::netcol::Found: return x.found;
                    case rv::netcol::NotFound: return x.not_found;
                    case rv::netcol::Commits: return x.commits;
                    case rv::netcol::Vias: return x.vias;
                    case rv::netcol::Rips: return x.rips;
                    case rv::netcol::FailedPads: return x.failed_pads;
                    case rv::netcol::Expanded: return static_cast<double>(x.expanded);
                    case rv::netcol::Seconds: return x.seconds;
                    case rv::netcol::First: return x.first_event == rv::kNone32 ? 1e18 : x.first_event;
                    case rv::netcol::Last: return x.last_event == rv::kNone32 ? 1e18 : x.last_event;
                    case rv::netcol::Copper: return n.copper;
                    case rv::netcol::Failures: return n.failures;
                    default: return 0;
                    }
                };
                if (c == rv::netcol::Name) {
                    std::string x = a.name, y = b.name;
                    for (char &ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    for (char &ch : y) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    CHECK(d == 0 ? x <= y : x >= y);
                    if (x == y) CHECK(a.session < b.session || (a.session == b.session && a.net < b.net));
                } else {
                    const double x = key(v[k - 1]), y = key(v[k]);
                    CHECK(d == 0 ? x <= y : x >= y);
                    if (x == y) CHECK(a.session < b.session || (a.session == b.session && a.net < b.net));
                }
            }
        }
    }
    table.set_view(rv::netcol::Name, false, "n2");
    table.update(e);
    CHECK(!table.view().empty());
    for (std::uint32_t r : table.view()) CHECK_EQ(table.rows()[r].name, std::string("N2"));
    table.set_view(rv::netcol::Name, false, "zz");
    table.update(e);
    CHECK(table.view().empty());
    // The generation changes with the view and with live values, not otherwise.
    table.set_view(rv::netcol::Copper, true, "");
    table.update(0);
    const std::uint32_t g = table.generation();
    CHECK(!table.update(0));
    CHECK_EQ(table.generation(), g);
}

TEST(nets_table_through_the_c_interface) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "t.grtrace", synth::build_timeline_sample()));
    rv_nets_info info;
    CHECK_EQ(rv_nets_get(s.s, &info), RV_OK);
    CHECK(info.row_count >= 6);
    CHECK_EQ(info.view_count, info.row_count);
    CHECK_EQ(info.sort_column, static_cast<uint32_t>(RV_NETCOL_NAME));
    CHECK_STREQ(rv_net_column_title(RV_NETCOL_STATE), "State");
    CHECK_EQ(rv_net_column_align(RV_NETCOL_SEARCHES), 1u);
    CHECK_STREQ(rv_net_state_name(RV_NET_COMPLETE), "complete");
    CHECK_EQ(rv_nets_set_view(s.s, RV_NETCOL_NAME, 0, "sig"), RV_OK);
    CHECK_EQ(rv_nets_get(s.s, &info), RV_OK);
    CHECK_EQ(info.view_count, 2u); // /SIG in sessions 1 and 2
    rv_net_row row;
    CHECK_EQ(rv_nets_row(s.s, 0, &row), RV_OK);
    CHECK_STREQ(row.name, "/SIG");
    CHECK_EQ(row.session, 0u);
    const char *text = nullptr;
    CHECK_EQ(rv_nets_cell(s.s, 0, RV_NETCOL_SESSION, &text), RV_OK);
    CHECK_STREQ(text, "1");
    CHECK_EQ(rv_nets_cell(s.s, 0, RV_NETCOL_STATE, &text), RV_OK);
    CHECK_STREQ(text, "complete");
    CHECK_EQ(rv_nets_cell(s.s, 5, 0, &text), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_nets_set_view(s.s, RV_NETCOL_COUNT, 0, nullptr), RV_E_INVALID_ARGUMENT);
    // The selected net's row in the position's session (session 2 at the end).
    uint32_t changed = 0;
    CHECK_EQ(rv_select_net(s.s, 2, &changed), RV_OK);
    CHECK_EQ(rv_nets_get(s.s, &info), RV_OK);
    CHECK_EQ(info.selected_position, 1u);
    // Live values follow the position; the generation changes with them.
    const uint32_t g = info.generation;
    CHECK_EQ(rv_seek_event(s.s, 0, &changed), RV_OK);
    CHECK_EQ(rv_nets_get(s.s, &info), RV_OK);
    CHECK(info.generation != g);
    CHECK_EQ(rv_nets_cell(s.s, 0, RV_NETCOL_STATE, &text), RV_OK);
    CHECK_STREQ(text, "not started");
}

// --- the selection and the highlight --------------------------------------------------------

namespace {

struct OverlayView {
    rv_display_list d;
    const rv_slot *slots = nullptr;
    uint32_t n = 0;
    const rv_slot *slot(uint32_t kind) const {
        for (uint32_t k = 0; k < n; ++k)
            if (slots[k].kind == kind) return &slots[k];
        return nullptr;
    }
};

OverlayView overlay_of(S &s) {
    OverlayView o;
    std::memset(&o.d, 0, sizeof o.d);
    rv_get_overlay(s.s, &o.d, &o.slots, &o.n);
    return o;
}

std::uint32_t mixw(std::uint32_t c, double f) {
    auto ch = [&](int sh) {
        const double v = (c >> sh) & 0xFF;
        return static_cast<std::uint32_t>(std::lround(v + (255.0 - v) * f)) << sh;
    };
    return ch(24) | ch(16) | ch(8) | (c & 0xFF);
}

} // namespace

TEST(highlight_slots_and_display_lists) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "p.grtrace", pick_trace()));
    OverlayView o = overlay_of(s);
    const rv_slot *dim = o.slot(RV_SLOT_DIM), *hl = o.slot(RV_SLOT_HIGHLIGHT), *mk = o.slot(RV_SLOT_MARKERS);
    CHECK(dim != nullptr && hl != nullptr && mk != nullptr);
    if (dim == nullptr || hl == nullptr || mk == nullptr) return;
    CHECK_EQ(dim->batch_count, 0u);
    CHECK_EQ(hl->batch_count, 0u);
    CHECK(dim->order_end <= hl->order_begin && hl->order_end <= mk->order_begin);
    const uint32_t gen0 = o.d.generation, hlgen0 = hl->generation;
    const uint32_t mkgen0 = mk->generation;

    uint32_t changed = 0;
    CHECK_EQ(rv_select_net(s.s, 2, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_SELECTION) != 0);
    CHECK((changed & RV_CHANGED_OVERLAY) != 0);
    o = overlay_of(s);
    dim = o.slot(RV_SLOT_DIM);
    hl = o.slot(RV_SLOT_HIGHLIGHT);
    mk = o.slot(RV_SLOT_MARKERS);
    CHECK(o.d.generation != gen0);
    CHECK(hl->generation != hlgen0);
    CHECK_EQ(mk->generation, mkgen0);
    CHECK_EQ(o.n, 8u); // the slot set does not change with the selection
    // The dim slot: one filled rectangle of the background at alpha 0.65,
    // covering the board.
    CHECK_EQ(dim->batch_count, 1u);
    const rv_batch &db = o.d.batches[dim->first_batch];
    CHECK_EQ(db.prim, static_cast<uint32_t>(RV_FILL));
    CHECK_EQ(db.rgba, (o.d.background & 0xFFFFFF00u) | 166u);
    CHECK(db.bbox[0] <= bx(-0.5) && db.bbox[2] >= bx(39.5) && db.bbox[1] <= by(-0.5) && db.bbox[3] >= by(23.5));
    // The highlight: /SIG only, brightened, in the highlight ranks, sorted.
    rv_layer_info fcu;
    std::memset(&fcu, 0, sizeof fcu);
    uint32_t nl = 0;
    rv_layer_count(s.s, &nl);
    for (uint32_t k = 0; k < nl; ++k) {
        rv_layer_info li;
        rv_layer(s.s, k, &li);
        if (std::strcmp(li.name, "F.Cu") == 0) fcu = li;
    }
    uint32_t fills = 0, fill_items = 0, segs = 0, strokes = 0, discs = 0, rings = 0;
    for (uint32_t k = hl->first_batch; k < hl->first_batch + hl->batch_count; ++k) {
        const rv_batch &b = o.d.batches[k];
        CHECK(b.order >= hl->order_begin && b.order < hl->order_end);
        if (k > hl->first_batch) CHECK(o.d.batches[k - 1].order <= b.order);
        CHECK(b.key >= 0x80000000u);
        if (b.prim == RV_FILL) {
            ++fills;
            fill_items += b.count;
            CHECK_EQ(b.rgba, mixw(fcu.rgba, 0.3)); // the pads, on F.Cu
        }
        if (b.prim == RV_SEGS) segs += b.count;
        if (b.prim == RV_STROKE) {
            strokes += b.count;
            CHECK_EQ(b.rgba, mixw(fcu.rgba, 0.3)); // the router copper, on F.Cu
        }
        if (b.prim == RV_DISC) discs += b.count;
        if (b.prim == RV_RING) rings += b.count;
    }
    CHECK_EQ(fill_items, 2u); // U1.1 and J1.1, not GND's U3.1, not the GND zone
    CHECK_EQ(segs, 1u);       // the board track
    CHECK_EQ(strokes, 1u);    // the router copper
    CHECK_EQ(discs, 3u);      // the via on F.Cu and B.Cu, its drill
    CHECK_EQ(rings, 1u);      // the via's hole wall
    (void)fills;
    rv_selection sel;
    CHECK_EQ(rv_selection_get(s.s, &sel), RV_OK);
    CHECK_EQ(sel.active, 1u);
    CHECK_EQ(sel.has_net, 1u);
    CHECK_STREQ(sel.name, "/SIG");
    CHECK_EQ(sel.board_nets, 1u);
    CHECK_EQ(sel.board_items, 4u); // 2 pads, 1 track, 1 via

    // Selecting the same net again changes nothing.
    CHECK_EQ(rv_select_net(s.s, 2, &changed), RV_OK);
    CHECK_EQ(changed, 0u);

    // Hiding F.Cu hides the highlight there too.
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 0), RV_OK);
    o = overlay_of(s);
    hl = o.slot(RV_SLOT_HIGHLIGHT);
    for (uint32_t k = hl->first_batch; k < hl->first_batch + hl->batch_count; ++k)
        CHECK(o.d.batches[k].layer != fcu.index);
    CHECK(hl->batch_count > 0); // the via on B.Cu and its holes remain
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 1), RV_OK);

    // GND: its zone is highlighted (a fill on B.Cu), and its router copper.
    CHECK_EQ(rv_select_name(s.s, "GND", &changed), RV_OK);
    CHECK_EQ(rv_selection_get(s.s, &sel), RV_OK);
    CHECK_EQ(sel.has_net, 1u);
    CHECK_EQ(sel.net, 1);
    CHECK_EQ(sel.board_items, 2u); // pad U3.1, the zone
    // A name of no net: the board is dimmed, nothing highlighted.
    CHECK_EQ(rv_select_name(s.s, "NOPE", &changed), RV_OK);
    CHECK_EQ(rv_selection_get(s.s, &sel), RV_OK);
    CHECK_EQ(sel.active, 1u);
    CHECK_EQ(sel.has_net, 0u);
    CHECK_EQ(sel.board_nets, 0u);
    o = overlay_of(s);
    CHECK_EQ(o.slot(RV_SLOT_DIM)->batch_count, 1u);
    CHECK_EQ(o.slot(RV_SLOT_HIGHLIGHT)->batch_count, 0u);
    // A router net without a name: no board match.
    CHECK_EQ(rv_select_net(s.s, 77, &changed), RV_OK);
    CHECK_EQ(rv_selection_get(s.s, &sel), RV_OK);
    CHECK_STREQ(sel.name, "");
    CHECK_EQ(sel.board_nets, 0u);
    // Clearing empties both slots again.
    CHECK_EQ(rv_select_clear(s.s, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_SELECTION) != 0);
    o = overlay_of(s);
    CHECK_EQ(o.slot(RV_SLOT_DIM)->batch_count, 0u);
    CHECK_EQ(o.slot(RV_SLOT_HIGHLIGHT)->batch_count, 0u);
    CHECK_EQ(rv_select_name(s.s, nullptr, &changed), RV_OK);
    CHECK_EQ(changed, 0u);
}

TEST(highlight_of_a_ghost_mid_run) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "t.grtrace", synth::build_timeline_sample()));
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_event(s.s, 16, &changed), RV_OK); // the rebuild rip-up: /SIG's ghost
    CHECK_EQ(rv_select_name(s.s, "/SIG", &changed), RV_OK);
    OverlayView o = overlay_of(s);
    const rv_slot *hl = o.slot(RV_SLOT_HIGHLIGHT);
    CHECK(hl != nullptr);
    if (hl == nullptr) return;
    bool ghost = false;
    for (uint32_t k = hl->first_batch; k < hl->first_batch + hl->batch_count; ++k) {
        const rv_batch &b = o.d.batches[k];
        if ((b.rgba & 0xFF) == 191u) ghost = true; // alpha 0.75: a fresh ghost
    }
    CHECK(ghost);
    CHECK(hl->batch_count > 0);
    // The order mapping keeps the stacking: B.Cu < F.Cu, sub-slots, holes on top.
    CHECK(rv::highlight_order(rv::order_key(100, 0x40)) < rv::highlight_order(rv::order_key(170, 0x10)));
    CHECK(rv::highlight_order(rv::order_key(170, 0x30)) < rv::highlight_order(rv::order_key(170, 0x38)));
    CHECK(rv::highlight_order(rv::order_key(170, 0x38)) < rv::highlight_order(rv::order_key(170, 0x40)));
    CHECK(rv::highlight_order(rv::order_key(170, 0x60)) < rv::highlight_order(rv::order_key(rv::kRankHoles, 0x10)));
    CHECK(rv::highlight_order(rv::order_key(rv::kRankHoles, 0x30)) < rv::order_key(rv::kRankHighlightEnd, 0));
    CHECK(rv::highlight_order(rv::order_key(100, 0x08)) >= rv::order_key(rv::kRankHighlight, 0));
}

// A board written before the first RUN (as the dongle's generator does):
// the overlay already has the first session's (empty) router slots there, so
// the session's first event does not change the slot structure (which made a
// client draw every plane again: M4's slowest playback frame).
TEST(slots_before_the_first_run) {
    synth::TimelineWriter tw;
    tw.nets({{1, "GND"}, {2, "/SIG"}});
    tw.run = synth::make_run(40, 24, 8);
    tw.run.cell = 0.25;
    tw.run.origin_x = 100.5;
    tw.run.origin_y = 50.25;
    tw.brd(tw.board("placed", false));
    tw.start(40, 24);
    tw.commit(2, line(0, 2, 30, 3), 0.2, {}, {}, 0, 0xFFFFFFFFu);
    tw.end({});
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "b.grtrace", tw.w.out));
    rv_timeline tl;
    rv_get_timeline(s.s, &tl);
    CHECK(tl.event_count >= 3);
    CHECK_EQ(tl.events[0].kind, static_cast<uint32_t>(RV_EV_BOARD));
    uint32_t changed = 0;
    std::vector<std::pair<uint32_t, uint32_t>> shape0, shape;
    for (uint32_t e = 0; e < tl.event_count; ++e) {
        CHECK_EQ(rv_seek_event(s.s, e, &changed), RV_OK);
        OverlayView o = overlay_of(s);
        shape.clear();
        for (uint32_t k = 0; k < o.n; ++k) shape.emplace_back(o.slots[k].kind, o.slots[k].order_begin);
        if (e == 0) shape0 = shape;
        CHECK(shape == shape0);
        if (e == 0) CHECK_EQ(o.d.batch_count, 0u);
    }
    CHECK_EQ(shape0.size(), std::size_t(8));
}

TEST(selection_drives_one_net_pacing) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "t.grtrace", synth::build_timeline_sample()));
    rv_pacing p;
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    p.mode = RV_PACING_ONE_NET;
    p.net = 1;
    uint32_t changed = 0;
    CHECK_EQ(rv_set_pacing(s.s, &p, &changed), RV_OK);
    CHECK_EQ(rv_select_net(s.s, 4, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_TIMELINE) != 0);
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    CHECK_EQ(p.net, 4);
    // Other modes are left alone.
    p.mode = RV_PACING_FIT;
    CHECK_EQ(rv_set_pacing(s.s, &p, &changed), RV_OK);
    CHECK_EQ(rv_select_net(s.s, 2, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_TIMELINE) == 0);
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    CHECK_EQ(p.mode, static_cast<uint32_t>(RV_PACING_FIT));
}

// --- hit testing ---------------------------------------------------------------------------

TEST(hit_test_geometry_order_tolerance_and_visibility) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "p.grtrace", pick_trace()));
    // Pad U1.1 over the router copper of /SIG: the pad on top.
    std::vector<rv_hit> h = hits_at(s, bx(2), by(3));
    CHECK(h.size() >= 2);
    if (h.size() >= 2) {
        CHECK_EQ(h[0].kind, RV_HIT_PAD);
        CHECK_STREQ(h[0].footprint, "U1");
        CHECK_STREQ(h[0].pad, "1");
        CHECK_STREQ(h[0].net_name, "/SIG");
        CHECK_EQ(h[1].kind, RV_HIT_ROUTED);
        CHECK_EQ(h[1].net, 2);
        CHECK_EQ(h[1].has_net, 1u);
        CHECK(h[1].event != RV_NONE);
    }
    // Router copper alone.
    h = hits_at(s, bx(20), by(3));
    CHECK_EQ(h.size(), std::size_t(1));
    if (!h.empty()) {
        CHECK_EQ(h[0].kind, RV_HIT_ROUTED);
        CHECK_EQ(h[0].router_layer, 0u);
    }
    // The zone: inside only.
    h = hits_at(s, bx(20), by(20));
    CHECK(has_kind(h, RV_HIT_ZONE));
    if (!h.empty()) {
        CHECK_EQ(h.back().kind, RV_HIT_ZONE);
        CHECK_STREQ(h.back().net_name, "GND");
    }
    CHECK(!has_kind(hits_at(s, bx(20), by(17.9)), RV_HIT_ZONE));
    // The via, over the zone? (9 < 18: no) alone with nothing else.
    h = hits_at(s, bx(20), by(9));
    CHECK(!h.empty());
    if (!h.empty()) CHECK_EQ(h[0].kind, RV_HIT_VIA);
    // The board track: within 3 view points of its edge, not 4.
    const double sc = view_scale(s);
    const double edge = by(6) + 0.1;
    CHECK(has_kind(hits_at(s, bx(10), edge + 2.0 / sc), RV_HIT_TRACK));
    CHECK(!has_kind(hits_at(s, bx(10), edge + 4.0 / sc), RV_HIT_TRACK));
    // Visibility: the layer, the class.
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 0), RV_OK);
    CHECK(!has_kind(hits_at(s, bx(10), by(6)), RV_HIT_TRACK));
    CHECK(!has_kind(hits_at(s, bx(20), by(3)), RV_HIT_ROUTED));
    CHECK(has_kind(hits_at(s, bx(20), by(9)), RV_HIT_VIA)); // still on B.Cu
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 1), RV_OK);
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_TRACKS, 0), RV_OK);
    CHECK(!has_kind(hits_at(s, bx(10), by(6)), RV_HIT_TRACK));
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_TRACKS, 1), RV_OK);
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_ROUTED, 0), RV_OK);
    CHECK(hits_at(s, bx(20), by(3)).empty());
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_ROUTED, 1), RV_OK);
    // The failed pad J2.1 of GND: the marker first.
    h = hits_at(s, bx(36), by(20));
    CHECK(!h.empty());
    if (!h.empty()) {
        CHECK_EQ(h[0].kind, RV_HIT_FAILURE);
        CHECK_EQ(h[0].net, 1);
        CHECK(has_kind(h, RV_HIT_ZONE)); // the zone under it, last
    }
    // After pan and zoom: the same hits at the same board points.
    CHECK_EQ(rv_view_zoom(s.s, 3.0, 100, 100), RV_OK);
    CHECK_EQ(rv_view_pan(s.s, -250, 120), RV_OK);
    h = hits_at(s, bx(2), by(3));
    CHECK(!h.empty());
    if (!h.empty()) CHECK_EQ(h[0].kind, RV_HIT_PAD);
    const double sc2 = view_scale(s);
    CHECK(has_kind(hits_at(s, bx(10), edge + 2.0 / sc2), RV_HIT_TRACK));
    CHECK(!has_kind(hits_at(s, bx(10), edge + 4.0 / sc2), RV_HIT_TRACK));
    // Nothing far away.
    CHECK(hits_at(s, 0, 0).empty());
    // The hover text: the position and the topmost hit.
    double vx = 0, vy = 0;
    rv_board_to_view(s.s, bx(2), by(3), &vx, &vy);
    const char *text = nullptr;
    CHECK_EQ(rv_hover_status(s.s, vx, vy, &text), RV_OK);
    CHECK(text != nullptr && std::strstr(text, "X ") == text && std::strstr(text, "pad U1.1") != nullptr);
}

TEST(hit_test_wavefront_cells) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "p.grtrace", pick_trace()));
    // GND's first search, part revealed.
    rv_timeline tl;
    rv_get_timeline(s.s, &tl);
    uint32_t ev = RV_NONE;
    for (uint32_t e = 0; e < tl.event_count; ++e)
        if (tl.events[e].kind == RV_EV_SEARCH && tl.events[e].net == 1 && ev == RV_NONE) ev = e;
    CHECK(ev != RV_NONE);
    if (ev == RV_NONE) return;
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_time(s.s, tl.events[ev].start + 0.5 * tl.events[ev].duration, &changed), RV_OK);
    rv_position pos;
    rv_position_get(s.s, &pos);
    CHECK(pos.cells_revealed > 1);
    rv_search_info si;
    rv_search(s.s, tl.events[ev].record, &si);
    const uint32_t *cells = nullptr;
    const int32_t *costs = nullptr;
    uint32_t n = 0;
    CHECK_EQ(rv_wavefront(s.s, si.wavefront_search, &cells, &costs, &n), RV_OK);
    CHECK(n > pos.cells_revealed);
    // The source cell (first in the order) and a cell not yet revealed.
    const uint32_t nc = 40 * 24;
    auto at = [&](uint32_t cell) { return hits_at(s, bx((cell % nc) % 40), by((cell % nc) / 40)); };
    std::vector<rv_hit> h = at(cells[0]);
    bool found = false;
    // Both router layers' cells at the point can be hits (the upper first);
    // the source is the one on its layer.
    auto wave_hit = [](const std::vector<rv_hit> &hs, uint32_t cell) {
        for (const rv_hit &x : hs)
            if (x.kind == RV_HIT_WAVEFRONT && x.cell == cell) return true;
        return false;
    };
    if (!h.empty() && h.size() >= 2 && h[0].kind == RV_HIT_WAVEFRONT && h[1].kind == RV_HIT_WAVEFRONT)
        CHECK(h[0].router_layer < h[1].router_layer);
    for (const rv_hit &x : h)
        if (x.kind == RV_HIT_WAVEFRONT && x.cell == cells[0]) {
            found = true;
            CHECK_EQ(x.order_index, 0u);
            CHECK_EQ(x.router_layer, cells[0] / nc);
            CHECK_EQ(x.net, 1);
            CHECK_EQ(x.record, tl.events[ev].record);
            if (costs != nullptr) CHECK_EQ(x.cost, static_cast<int64_t>(costs[0]));
        }
    CHECK(found);
    // A cell revealed later than the position: no wavefront hit there.
    uint32_t late = RV_NONE;
    for (uint32_t k = n; k-- > pos.cells_revealed;) {
        bool earlier = false;
        for (uint32_t q = 0; q < pos.cells_revealed; ++q)
            if (cells[q] == cells[k]) earlier = true;
        if (!earlier) {
            late = cells[k];
            break;
        }
    }
    CHECK(late != RV_NONE);
    if (late != RV_NONE) CHECK(!wave_hit(at(late), late));
    CHECK(wave_hit(at(cells[pos.cells_revealed - 1]), cells[pos.cells_revealed - 1]));
    // Hidden with the class.
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_WAVEFRONT, 0), RV_OK);
    CHECK(!has_kind(at(cells[0]), RV_HIT_WAVEFRONT));
}

TEST(click_selects_the_net_under_the_point) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "p.grtrace", pick_trace()));
    double vx = 0, vy = 0;
    uint32_t changed = 0;
    rv_selection sel;
    // A board track: its net by name, which is router net 2 too.
    rv_board_to_view(s.s, bx(10), by(6), &vx, &vy);
    CHECK_EQ(rv_click(s.s, vx, vy, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_SELECTION) != 0);
    rv_selection_get(s.s, &sel);
    CHECK_EQ(sel.active, 1u);
    CHECK_EQ(sel.has_net, 1u);
    CHECK_EQ(sel.net, 2);
    // Router copper of GND.
    rv_board_to_view(s.s, bx(20), by(15), &vx, &vy);
    CHECK_EQ(rv_click(s.s, vx, vy, &changed), RV_OK);
    rv_selection_get(s.s, &sel);
    CHECK_EQ(sel.net, 1);
    CHECK_STREQ(sel.name, "GND");
    // Empty space clears.
    rv_board_to_view(s.s, bx(30), by(11), &vx, &vy);
    CHECK_EQ(rv_click(s.s, vx, vy, &changed), RV_OK);
    rv_selection_get(s.s, &sel);
    CHECK_EQ(sel.active, 0u);
    CHECK_EQ(rv_click(s.s, vx, vy, &changed), RV_OK);
    CHECK_EQ(changed, 0u);
}

// --- history ----------------------------------------------------------------------------

namespace {

void check_history(const rv::Trace &t, const rv::Timeline &tl, std::int32_t net) {
    const std::vector<rv::HistoryEntry> h = rv::net_history(t, tl, net);
    std::uint32_t prev = 0;
    std::uint32_t searches = 0, commits = 0, routes = 0, ripped = 0, returns = 0;
    std::set<std::uint32_t> chunks;
    for (const rv::HistoryEntry &e : h) {
        if (e.kind == rv::hist::Session) {
            CHECK_EQ(e.depth, 0u);
            continue;
        }
        CHECK(e.chunk >= prev);
        prev = e.chunk;
        CHECK(e.depth == 1 || e.depth == 2);
        CHECK_EQ(e.event, tl.event_of_chunk(e.chunk));
        CHECK(!e.text.empty());
        chunks.insert(e.chunk);
        if (e.kind == rv::hist::Search) ++searches;
        if (e.kind == rv::hist::Commit) ++commits;
        if (e.kind == rv::hist::Route) ++routes;
        if (e.kind == rv::hist::RipSuffered) ++ripped;
        if (e.kind == rv::hist::RouteEnd) ++returns;
    }
    // Against the chunk sequence: every event chunk of the net, once.
    std::uint32_t want_s = 0, want_c = 0, want_r = 0, want_e = 0;
    std::set<std::uint32_t> rips;
    auto it = t.net_chunks().find(net);
    if (it != t.net_chunks().end())
        for (std::uint32_t k : it->second) {
            const rv::Chunk &c = t.chunks()[k];
            if (c.corrupt || c.record == rv::kNone32) continue;
            bool want = false;
            if (c.kind == rv::Kind::Srch && t.searches()[c.record].valid) ++want_s, want = true;
            if (c.kind == rv::Kind::Cmit) ++want_c, want = true;
            if (c.kind == rv::Kind::Rnb && t.routes()[c.record].valid) ++want_r, want = true;
            if (c.kind == rv::Kind::Rne) ++want_e, want = true;
            if (want) CHECK(chunks.count(k) == 1);
        }
    for (const rv::Copper &c : t.copper())
        if (c.net == net && c.removed_by != rv::kNone32) rips.insert(c.removed_by);
    CHECK_EQ(searches, want_s);
    CHECK_EQ(commits, want_c);
    CHECK_EQ(routes, want_r);
    CHECK_EQ(returns, want_e);
    CHECK_EQ(ripped, static_cast<std::uint32_t>(rips.size()));
    // The events list: those of the entries.
    const std::vector<std::uint32_t> ev = rv::net_events(t, tl, net);
    CHECK(std::is_sorted(ev.begin(), ev.end()));
    for (const rv::HistoryEntry &e : h)
        if (e.kind != rv::hist::Session) CHECK(std::binary_search(ev.begin(), ev.end(), e.event));
}

} // namespace

TEST(history_against_the_chunk_sequence) {
    {
        std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
        rv::Timeline tl(*t, rv::FitLengthPacing());
        for (std::int32_t net = 1; net <= 4; ++net) check_history(*t, tl, net);
        // /SIG: two route_net calls in session 1, ripped by /CLK's rebuild;
        // in session 2 a commit removed by a direct rip.
        const std::vector<rv::HistoryEntry> h = rv::net_history(*t, tl, 2);
        std::string all;
        for (const rv::HistoryEntry &e : h) all += std::string(e.depth * 2, ' ') + e.text + "\n";
        CHECK(all.find("router session 1\n") == 0);
        CHECK(all.find("ripped up by the route_net of /CLK: 1 copper items removed") != std::string::npos);
        CHECK(all.find("router session 2") != std::string::npos);
        CHECK(all.find("ripped up by a direct rip") != std::string::npos);
        CHECK(all.find("    search 0 (route_net): found, cost") != std::string::npos);
        CHECK(all.find(": ok\n") != std::string::npos);
        // GND: the failed pads and the failed drop.
        std::string g;
        for (const rv::HistoryEntry &e : rv::net_history(*t, tl, 1)) g += e.text + "\n";
        CHECK(g.find("2 pads failed") != std::string::npos);
        CHECK(g.find("failed pad TP1") != std::string::npos);
        CHECK(g.find("drop_to_plane pad J2.1: failed") != std::string::npos);
        // /CLK: the rebuild it caused, and the session-2 rip of /SIG that took its copper.
        std::string c;
        for (const rv::HistoryEntry &e : rv::net_history(*t, tl, 4)) c += e.text + "\n";
        CHECK(c.find("rebuild: rips up /SIG") != std::string::npos);
        CHECK(c.find("(a rip of /SIG)") != std::string::npos);
    }
    for (std::uint32_t seed = 1; seed <= 30; ++seed) {
        std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::random_timeline(seed));
        rv::Timeline tl(*t, rv::FitLengthPacing());
        for (std::int32_t net = 1; net <= 4; ++net) check_history(*t, tl, net);
    }
}

TEST(history_through_the_c_interface) {
    rvtest::TempDir dir;
    S s;
    CHECK(open_bytes(s, dir, "t.grtrace", synth::build_timeline_sample()));
    uint32_t n = 0;
    CHECK_EQ(rv_net_history(s.s, 4, &n), RV_OK);
    CHECK(n > 4);
    rv_history_entry e;
    CHECK_EQ(rv_history_get(s.s, 0, &e), RV_OK);
    CHECK_EQ(e.kind, RV_HIST_SESSION);
    CHECK_STREQ(e.text, "router session 1");
    CHECK_EQ(rv_history_get(s.s, n, &e), RV_E_INVALID_ARGUMENT);
    const uint32_t *ev = nullptr;
    uint32_t ne = 0;
    CHECK_EQ(rv_net_events(s.s, 4, &ev, &ne), RV_OK);
    CHECK(ne > 0 && ev != nullptr);
    for (uint32_t k = 1; k < ne; ++k) CHECK(ev[k - 1] < ev[k]);
    // A net with nothing: empty, not an error.
    CHECK_EQ(rv_net_history(s.s, 99, &n), RV_OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(rv_net_events(s.s, 99, &ev, &ne), RV_OK);
    CHECK_EQ(ne, 0u);
}

TEST(inspect_calls_without_a_trace) {
    S s;
    rv_nets_info info;
    CHECK_EQ(rv_nets_get(s.s, &info), RV_OK);
    CHECK_EQ(info.row_count, 0u);
    uint32_t n = 7;
    CHECK_EQ(rv_hit_test(s.s, 10, 10, &n), RV_OK);
    CHECK_EQ(n, 0u);
    const char *text = nullptr;
    CHECK_EQ(rv_hover_status(s.s, 10, 10, &text), RV_OK);
    CHECK(text != nullptr);
    uint32_t changed = 0;
    CHECK_EQ(rv_click(s.s, 10, 10, &changed), RV_OK);
    CHECK_EQ(rv_net_history(s.s, 1, &n), RV_OK);
    CHECK_EQ(n, 0u);
    rv_selection sel;
    CHECK_EQ(rv_select_name(s.s, "X", &changed), RV_OK);
    CHECK_EQ(rv_selection_get(s.s, &sel), RV_OK);
    CHECK_EQ(sel.active, 1u);
    CHECK_EQ(rv_hit_test(s.s, 0.0 / 0.0 * 0 + 1e308 * 10, 1, &n), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_nets_row(s.s, 0, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_hit_get(s.s, 0, nullptr), RV_E_INVALID_ARGUMENT);
}

// Optional: the real traces in RV_REAL_TRACES (':'-separated): the table
// against the naive recount at a sample of positions, the pick time.
TEST(inspect_real_traces) {
    const char *env = std::getenv("RV_REAL_TRACES");
    if (env == nullptr || *env == 0) {
        std::printf("    skipped: RV_REAL_TRACES not set\n");
        return;
    }
    std::string list = env;
    std::size_t a = 0;
    while (a <= list.size()) {
        std::size_t b = list.find(':', a);
        if (b == std::string::npos) b = list.size();
        const std::string path = list.substr(a, b - a);
        a = b + 1;
        if (path.empty()) continue;
        std::unique_ptr<rv::Trace> t;
        try {
            t = rv::Trace::open_file(path);
        } catch (...) {
            CHECK(false);
            continue;
        }
        rv::Timeline tl(*t, rv::FitLengthPacing());
        rv::NetTable table;
        table.build(*t, tl);
        const auto naive = naive_totals(*t, tl);
        int states = 0;
        for (std::uint32_t r = 0; r < table.rows().size(); ++r) {
            const rv::NetRow &row = table.rows()[r];
            auto it = naive.find({row.session, row.net});
            const Naive z = it != naive.end() ? it->second : Naive();
            CHECK_EQ(row.searches, z.searches);
            CHECK_EQ(row.commits, z.commits);
            CHECK_EQ(row.rips, static_cast<std::uint32_t>(z.rips.size()));
            CHECK_EQ(row.failed_pads, z.failed);
            for (std::uint32_t e = 0; e < tl.size(); e += 97) {
                bool amb = false;
                const rv::NetNow want = naive_now(*t, tl, row.session, row.net, e, amb);
                const rv::NetNow got = table.at(r, e);
                CHECK_EQ(got.copper, want.copper);
                CHECK_EQ(got.failures, want.failures);
                if (!amb) CHECK_EQ(static_cast<int>(got.state), static_cast<int>(want.state));
                ++states;
            }
        }
        std::printf("    %s: %zu rows, %d states compared\n", path.c_str(), table.rows().size(), states);
    }
}

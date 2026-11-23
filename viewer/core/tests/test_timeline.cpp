// test_timeline.cpp: M3 core: the timeline, the state at every position,
// the playback clock, the overlay and the board under the position.

#include "check.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "routed.hpp"
#include "rv.h"
#include "synth.hpp"
#include "timeline.hpp"
#include "trace_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using synth::Cell;

struct SessionGuard {
    rv_session *s = nullptr;
    SessionGuard() { rv_create(&s); }
    ~SessionGuard() { rv_destroy(s); }
};

Cell C(std::uint32_t l, std::int32_t i, std::int32_t j) {
    Cell c;
    c.layer = l;
    c.i = i;
    c.j = j;
    return c;
}

struct Fixture {
    std::unique_ptr<rv::Trace> trace;
    std::unique_ptr<rv::Timeline> tl;
    explicit Fixture(const std::vector<unsigned char> &bytes) {
        trace = rvtest::open_bytes(bytes);
        tl.reset(new rv::Timeline(*trace, rv::FitLengthPacing()));
    }
};

// --- naive replay, chunk by chunk, from the reader's tables only -------------------

std::vector<std::uint32_t> naive_alive(const rv::Trace &t, std::uint32_t P) {
    std::set<std::uint32_t> alive;
    for (std::uint32_t k = 0; k <= P && k < t.chunks().size(); ++k) {
        const rv::Chunk &c = t.chunks()[k];
        if (c.kind == rv::Kind::Run) {
            alive.clear();
            continue;
        }
        if (c.corrupt || c.record == rv::kNone32) continue;
        if (c.kind == rv::Kind::Cmit) alive.insert(t.commits()[c.record].copper);
        if (c.kind == rv::Kind::Via && t.vias()[c.record].added) alive.insert(t.vias()[c.record].copper);
        if (c.kind == rv::Kind::Rip)
            for (std::uint32_t id : t.rips()[c.record].removed) alive.erase(t.copper_index(id));
    }
    return std::vector<std::uint32_t>(alive.begin(), alive.end());
}

std::multiset<std::pair<std::string, std::string>> naive_failures(const rv::Trace &t, std::uint32_t P) {
    std::multiset<std::pair<std::string, std::string>> f;
    for (std::uint32_t k = 0; k <= P && k < t.chunks().size(); ++k) {
        const rv::Chunk &c = t.chunks()[k];
        if (c.kind == rv::Kind::Run) {
            f.clear();
            continue;
        }
        if (c.corrupt || c.record == rv::kNone32) continue;
        if (c.kind == rv::Kind::Rnb && t.routes()[c.record].valid) {
            const std::string &name = t.routes()[c.record].name;
            for (auto it = f.begin(); it != f.end();) it = it->first == name ? f.erase(it) : std::next(it);
        }
        if (c.kind == rv::Kind::Rne) {
            const rv::RouteEnd &re = t.route_ends()[c.record];
            const std::uint32_t ri = t.route_index(re.route_id);
            const std::string name = ri != rv::kNone32 ? t.routes()[ri].name : std::string();
            for (const std::string &l : re.failed) f.insert({name, l});
        }
        if (c.kind == rv::Kind::Drop && t.drops()[c.record].ok == 0)
            f.insert({t.drops()[c.record].name, t.drops()[c.record].pad_label});
    }
    return f;
}

std::uint32_t naive_board(const rv::Trace &t, std::uint32_t P) {
    std::uint32_t b = rv::kNone32;
    for (std::uint32_t k = 0; k < t.boards().size(); ++k)
        if (t.boards()[k].chunk <= P) b = k;
    return b;
}

// Compares the timeline's state at every event with the naive replay.
int check_against_naive(const rv::Trace &t, const rv::Timeline &tl) {
    int bad = 0;
    for (std::uint32_t e = 0; e < tl.size(); ++e) {
        const std::uint32_t P = tl.events()[e].last;
        std::vector<std::uint32_t> got;
        tl.alive_copper(e, got);
        if (got != naive_alive(t, P)) ++bad;
        std::vector<std::uint32_t> fl;
        tl.alive_failures(e, fl);
        std::multiset<std::pair<std::string, std::string>> fs;
        for (std::uint32_t f : fl) fs.insert({tl.failures()[f].name, tl.failures()[f].label});
        if (fs != naive_failures(t, P)) ++bad;
        if (tl.events()[e].board != naive_board(t, P)) ++bad;
    }
    return bad;
}

// --- random routing runs ---------------------------------------------------------

std::vector<unsigned char> random_run(std::uint32_t seed) {
    std::mt19937 rng(seed);
    auto R = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    synth::TimelineWriter tw;
    tw.nets({{1, "N1"}, {2, "N2"}, {3, "N3"}, {4, "N4"}});
    tw.run = synth::make_run(12, 8, 8);
    auto path = [&]() {
        std::vector<Cell> p;
        Cell c = C(R(2), static_cast<std::int32_t>(R(12)), static_cast<std::int32_t>(R(8)));
        p.push_back(c);
        const std::uint32_t n = 1 + R(7);
        for (std::uint32_t k = 0; k < n; ++k) {
            if (R(5) == 0) {
                c.layer ^= 1u;
            } else {
                c.i = std::max(0, std::min(11, c.i + static_cast<std::int32_t>(R(3)) - 1));
                c.j = std::max(0, std::min(7, c.j + static_cast<std::int32_t>(R(3)) - 1));
            }
            p.push_back(c);
        }
        return p;
    };
    auto name = [](std::int32_t net) { return "N" + std::to_string(net); };
    const std::uint32_t sessions = 1 + R(3);
    for (std::uint32_t s = 0; s < sessions; ++s) {
        if (R(2) == 0) tw.brd(tw.board("b", R(2) == 0));
        tw.start(12, 8);
        const std::uint32_t ops = 15 + R(40);
        for (std::uint32_t k = 0; k < ops; ++k) {
            const std::int32_t net = 1 + static_cast<std::int32_t>(R(4));
            switch (R(11)) {
            case 0:
            case 1:
            case 2: tw.commit(net, path(), R(2) ? 0.2 : 0.5, R(3) == 0 ? std::vector<std::pair<std::int32_t, std::int32_t>>{{static_cast<std::int32_t>(R(12)), static_cast<std::int32_t>(R(8))}} : std::vector<std::pair<std::int32_t, std::int32_t>>{}); break;
            case 3: tw.via(net, static_cast<std::int32_t>(R(12)), static_cast<std::int32_t>(R(8))); break;
            case 4: {
                std::vector<std::uint32_t> extra;
                if (R(4) == 0 && !tw.alive.empty()) {
                    auto it = tw.alive.begin();
                    std::advance(it, R(static_cast<std::uint32_t>(tw.alive.size())));
                    if (it->second.net != net) extra.push_back(it->first);
                }
                tw.rip(net, 0, synth::NONE32, extra);
                break;
            }
            case 5:
                if (R(3) == 0) tw.snap();
                break;
            case 6: {
                const std::uint32_t r = tw.rnb(net, name(net), {{1.0, 1.0, 1, 2, "P1"}, {2.0, 1.5, 1, 2, "P2"}});
                tw.srch(net, {C(0, 4, 4)}, {C(0, 8, 4)}, {}, 1 + R(5000), 1, r);
                if (R(2) == 0) {
                    const std::int32_t other = 1 + static_cast<std::int32_t>(R(4));
                    tw.rbld(r, {name(other)}, {other});
                    tw.rip(other, 1, r);
                }
                const std::vector<Cell> p = path();
                tw.srch(net, {p.front()}, {p.back()}, p, 1 + R(5000), 1, r);
                tw.commit(net, p, 0.2, {}, {}, 1, r);
                std::vector<std::string> failed;
                if (R(3) == 0) failed.push_back("P2");
                if (R(4) == 0) failed.push_back("P1");
                tw.rne(r, net, failed);
                break;
            }
            case 7:
                tw.srch(net, {C(0, 2, 2), C(0, 3, 2)}, {C(1, 2, 2)}, {}, 1 + R(300), 2);
                tw.drop(net, R(3), name(net), "P" + std::to_string(1 + R(2)));
                break;
            case 8: tw.mark(R(2), "phase " + std::to_string(k)); break;
            case 9: tw.brd(tw.board("mid", R(2) == 0)); break;
            default: tw.pad(net, 0.25 * R(12), 0.25 * R(8), 0.3); break;
            }
        }
        if (R(2) == 0) tw.rout();
    }
    if (R(4) != 0) tw.end({});
    return tw.w.out;
}

// --- the overlay as text, for comparing states ---------------------------------------

std::string list_text(const rv_display_list &d, bool keys) {
    std::ostringstream o;
    o.precision(9);
    for (uint32_t k = 0; k < d.batch_count; ++k) {
        const rv_batch &b = d.batches[k];
        o << "batch " << b.prim << " " << b.cls << " " << b.layer << " " << b.order << " " << b.rgba << " "
          << b.width << " " << b.count;
        if (keys) o << " key " << b.key;
        o << ":";
        for (uint32_t i = b.first; i < b.first + b.count; ++i) {
            switch (b.prim) {
            case RV_STROKE:
                for (uint32_t p = 0; p < d.contours[2 * i + 1]; ++p)
                    o << " " << d.xy[2 * (d.contours[2 * i] + p)] << "," << d.xy[2 * (d.contours[2 * i] + p) + 1];
                o << " |";
                break;
            case RV_SEGS:
                for (int q = 0; q < 4; ++q) o << " " << d.segs[4 * i + q];
                break;
            case RV_DISC:
            case RV_RING:
                for (int q = 0; q < 3; ++q) o << " " << d.circles[3 * i + q];
                break;
            default: break;
            }
        }
        o << "\n";
    }
    return o.str();
}

// Geometry of one batch (for checking that a key always names the same geometry).
std::string batch_geometry(const rv_display_list &d, const rv_batch &b) {
    rv_display_list one = d;
    rv_batch copy = b;
    copy.rgba = 0;
    one.batches = &copy;
    one.batch_count = 1;
    return list_text(one, false);
}

struct State {
    std::string overlay;
    uint32_t board = RV_NONE, static_batches = 0;
    std::string caption;
};

State state_of(rv_session *s) {
    State st;
    rv_display_list ov;
    const rv_slot *slots = nullptr;
    uint32_t n = 0;
    rv_get_overlay(s, &ov, &slots, &n);
    st.overlay = list_text(ov, false);
    for (uint32_t k = 0; k < n; ++k) {
        std::ostringstream o;
        o << "slot " << slots[k].kind << " " << slots[k].order_begin << " " << slots[k].order_end << " "
          << slots[k].layer << " " << slots[k].first_batch << " " << slots[k].batch_count << "\n";
        st.overlay += o.str();
    }
    rv_display_list d;
    rv_get_display_list(s, &d);
    st.board = d.board;
    st.static_batches = d.batch_count;
    const char *c = nullptr;
    rv_text(s, RV_TEXT_CAPTION, &c);
    st.caption = c != nullptr ? c : "";
    return st;
}

bool same(const State &a, const State &b) {
    return a.overlay == b.overlay && a.board == b.board && a.static_batches == b.static_batches && a.caption == b.caption;
}

// --- routed copper as segments, for coverage checks ---------------------------------

struct Seg {
    std::uint32_t layer;
    std::int32_t net;
    double x0, y0, x1, y1;
};

std::vector<Seg> copper_segments(const rv::Trace &t, const std::vector<rv::CopperGeom> &geo,
                                 const std::vector<std::uint32_t> &alive) {
    std::vector<Seg> out;
    for (std::uint32_t c : alive) {
        const rv::CopperGeom &g = geo[c];
        const std::int32_t net = t.copper()[c].net;
        for (const rv::CopperGeom::Line &l : g.lines) {
            for (std::uint32_t k = 0; k + 1 < l.count || (l.count == 1 && k == 0); ++k) {
                const std::uint32_t a = l.first + k, b = l.count == 1 ? a : a + 1;
                out.push_back({l.layer, net, g.pts[2 * a], g.pts[2 * a + 1], g.pts[2 * b], g.pts[2 * b + 1]});
            }
        }
        for (const rv::CopperGeom::Seg &s : g.stubs) out.push_back({s.layer, net, s.x0, s.y0, s.x1, s.y1});
    }
    return out;
}

double dist_to_seg(double px, double py, const Seg &s) {
    const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const double len2 = dx * dx + dy * dy;
    double u = len2 > 0 ? ((px - s.x0) * dx + (py - s.y0) * dy) / len2 : 0;
    u = std::max(0.0, std::min(1.0, u));
    const double qx = s.x0 + u * dx - px, qy = s.y0 + u * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

// Fraction of the length of segs `a` (sampled every `step` mm) lying on some
// segment of `b` with the same layer (and net when check_net).
double covered(const std::vector<Seg> &a, const std::vector<Seg> &b, double tol, bool check_net, double *total = nullptr) {
    double len = 0, cov = 0;
    for (const Seg &s : a) {
        const double l = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
        const int n = std::max(2, static_cast<int>(l / 0.02) + 1);
        for (int k = 0; k < n; ++k) {
            const double u = n == 1 ? 0 : static_cast<double>(k) / (n - 1);
            const double px = s.x0 + u * (s.x1 - s.x0), py = s.y0 + u * (s.y1 - s.y0);
            bool hit = false;
            for (const Seg &q : b) {
                if (q.layer != s.layer || (check_net && q.net != s.net)) continue;
                if (dist_to_seg(px, py, q) <= tol) {
                    hit = true;
                    break;
                }
            }
            len += l / n;
            if (hit) cov += l / n;
        }
    }
    if (total != nullptr) *total = len;
    return len > 0 ? cov / len : 1.0;
}

struct RoutCheck {
    bool has = false;
    double seg_cover = 1, total_len = 0;
    std::size_t vias = 0, vias_matched = 0;
};

// The latest ROUT against the router copper alive at its event (14 item 8:
// ROUT drops spurs and merges edges, so it lies within the union).
RoutCheck check_rout(const rv::Trace &t, const rv::Timeline &tl, const std::vector<rv::CopperGeom> &geo) {
    RoutCheck r;
    if (t.routes_outputs().empty()) return r;
    const rv::Routes &ro = t.routes_outputs().back();
    const rv::Chunk &ch = t.chunks()[ro.chunk];
    if (ch.session >= t.runs().size() || !t.runs()[ch.session].valid) return r;
    r.has = true;
    const rv::Frame fr(t.runs()[ch.session]);
    std::vector<std::uint32_t> alive;
    tl.alive_copper(tl.event_of_chunk(ro.chunk), alive);
    const std::vector<Seg> cu = copper_segments(t, geo, alive);
    std::vector<Seg> rs;
    for (const rv::RouteSeg &s : ro.segs) rs.push_back({s.layer, s.net, fr.x(s.x0), fr.y(s.y0), fr.x(s.x1), fr.y(s.y1)});
    r.seg_cover = covered(rs, cu, 1e-6, true, &r.total_len);
    for (const rv::RouteVia &v : ro.vias) {
        ++r.vias;
        const double x = fr.x(v.x), y = fr.y(v.y);
        bool hit = false;
        for (std::uint32_t c : alive)
            for (const auto &p : geo[c].vias)
                if (t.copper()[c].net == v.net && std::hypot(p[0] - x, p[1] - y) < 1e-6) hit = true;
        if (hit) ++r.vias_matched;
    }
    return r;
}

struct BoardCheck {
    bool has = false;
    double board_in_routed = 1, routed_in_board = 1, board_len = 0, routed_len = 0;
    std::size_t board_vias = 0, vias_matched = 0, routed_vias = 0;
};

// A board written after routing against the router copper at its event.
BoardCheck check_board(rv::Trace &t, const rv::Timeline &tl, const std::vector<rv::CopperGeom> &geo, std::uint32_t b) {
    BoardCheck r;
    const rv::BoardInfo &bi = t.boards()[b];
    const std::uint32_t e = tl.event_of_chunk(bi.chunk);
    const std::uint32_t session = tl.events()[e].session;
    if (session >= t.runs().size() || !t.runs()[session].valid) return r;
    r.has = true;
    const rv::Run &run = t.runs()[session];
    auto geo_b = t.board_geometry(b);
    std::vector<Seg> bs;
    for (const rv::BoardTrack &tr : geo_b->tracks) {
        const std::string &ln = bi.layers[static_cast<std::size_t>(tr.layer_index)].name;
        std::uint32_t layer = ln == run.layer_name[0] ? 0 : ln == run.layer_name[1] ? 1 : 99;
        bs.push_back({layer, 0, tr.x0 * 1e-6, tr.y0 * 1e-6, tr.x1 * 1e-6, tr.y1 * 1e-6});
    }
    std::vector<std::uint32_t> alive;
    tl.alive_copper(e, alive);
    const std::vector<Seg> cu = copper_segments(t, geo, alive);
    r.board_in_routed = covered(bs, cu, 1e-5, false, &r.board_len);
    r.routed_in_board = covered(cu, bs, 1e-5, false, &r.routed_len);
    for (const rv::BoardVia &v : geo_b->vias) {
        ++r.board_vias;
        bool hit = false;
        for (std::uint32_t c : alive)
            for (const auto &p : geo[c].vias)
                if (std::hypot(p[0] - v.x * 1e-6, p[1] - v.y * 1e-6) < 1e-5) hit = true;
        if (hit) ++r.vias_matched;
    }
    for (std::uint32_t c : alive) r.routed_vias += geo[c].vias.size();
    return r;
}

std::uint32_t find_event(const rv::Timeline &tl, rv::EvKind kind, std::uint32_t nth = 0) {
    for (std::uint32_t e = 0; e < tl.size(); ++e)
        if (tl.events()[e].kind == kind && nth-- == 0) return e;
    return rv::kNone32;
}

bool write_temp(rvtest::TempDir &dir, const std::string &name, const std::vector<unsigned char> &bytes,
                std::string &path) {
    path = dir.path(name);
    return synth::write_file(path, bytes);
}

} // namespace

// --- events -------------------------------------------------------------------------

TEST(timeline_events_of_the_sample_run) {
    Fixture f(synth::build_timeline_sample());
    CHECK_EQ(f.trace->error_count(), 0u);
    const rv::Timeline &tl = *f.tl;
    std::string kinds;
    for (const rv::Event &e : tl.events()) kinds += std::string(rv::event_kind_name(static_cast<std::uint32_t>(e.kind))) + ",";
    // NETS, the MARK note, PADs, GRDS and ROUT are absorbed; the RBLD and its
    // RIP are one rip-up.
    const std::string want =
        "phase,board,session,snapshot,phase,"
        "route_net begins,search,commit,route_net ends,via,"
        "route_net begins,search,commit,route_net ends,"
        "route_net begins,search,rip-up,search,commit,route_net ends,"
        "route_net begins,search,commit,route_net ends,"
        "route_net begins,search,search,route_net ends,"
        "phase,search,drop_to_plane,rip-up,"
        "route_net begins,search,commit,route_net ends,board,"
        "phase,session,commit,commit,rip-up,commit,end,";
    CHECK_EQ(kinds, want);
    // Chunk ranges cover the file without gaps.
    std::uint32_t next = 0;
    for (const rv::Event &e : tl.events()) {
        CHECK_EQ(e.first, next);
        CHECK(e.first <= e.chunk && e.chunk <= e.last);
        next = e.last + 1;
    }
    CHECK_EQ(next, static_cast<std::uint32_t>(f.trace->chunks().size()));
    // The rebuild's rip-up holds the RBLD and its RIP.
    const std::uint32_t rb = find_event(tl, rv::EvKind::RipUp);
    CHECK((tl.events()[rb].flags & rv::evf::kRebuild) != 0);
    CHECK_EQ(tl.events()[rb].last, tl.events()[rb].chunk + 1);
    CHECK(tl.events()[rb].route != rv::kNone32);
    // Phases
    CHECK_EQ(tl.phases().size(), std::size_t(4));
    CHECK_EQ(tl.phases()[1].label, std::string("route"));
    CHECK_EQ(tl.events()[find_event(tl, rv::EvKind::Drop)].phase, 2u);
    // Counts after the last event
    const rv::Counts &c = tl.counts(tl.size() - 1);
    CHECK_EQ(c.commits, 8u);
    CHECK_EQ(c.vias, 3u); // the standalone via and two CMIT vias
    CHECK_EQ(c.rips, 3u);
    CHECK_EQ(c.removed, 5u);
    CHECK_EQ(c.searches, 9u);
    CHECK_EQ(c.routes, 6u);
    CHECK_EQ(tl.rip_ticks().size(), std::size_t(3));
    CHECK_EQ(tl.failure_ticks().size(), std::size_t(2));
}

TEST(timeline_pacing_and_positions) {
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    CHECK_EQ(tl.total(), rv::FitLengthPacing::kTotal);
    double sum = 0;
    for (std::uint32_t e = 0; e < tl.size(); ++e) {
        const rv::Event &ev = tl.events()[e];
        CHECK(ev.duration > 0);
        if (e > 0) CHECK(ev.start > tl.events()[e - 1].start);
        if (e > 0) CHECK(std::fabs(ev.start - (tl.events()[e - 1].start + tl.events()[e - 1].duration)) < 1e-9);
        CHECK_EQ(tl.event_at(ev.start), e);
        CHECK_EQ(tl.event_at(ev.start + ev.duration * 0.5), e);
        sum += ev.duration;
    }
    CHECK(std::fabs(sum - tl.total()) < 1e-9);
    CHECK_EQ(tl.events().back().start + tl.events().back().duration, tl.total());
    CHECK_EQ(tl.event_at(-5), 0u);
    CHECK_EQ(tl.event_at(tl.total()), tl.size() - 1);
    CHECK_EQ(tl.event_at(1e9), tl.size() - 1);
    // A bigger search gets more time than a smaller one; a commit 2 units.
    rv::FitLengthPacing p;
    rv::Event a, b;
    a.kind = b.kind = rv::EvKind::Search;
    a.size = 10;
    b.size = 100000;
    CHECK(p.weight(b) > p.weight(a));
    a.kind = rv::EvKind::Commit;
    CHECK_EQ(p.weight(a), 2.0);
    // No events at all.
    synth::Writer w;
    w.chunk("NETS", synth::encode(synth::WNets()));
    Fixture empty(w.out);
    CHECK_EQ(empty.tl->size(), 0u);
    CHECK_EQ(empty.tl->event_at(3), rv::kNone32);
}

// --- state at every position against a naive replay ----------------------------------

TEST(state_at_every_event_equals_naive_replay) {
    {
        Fixture f(synth::build_timeline_sample());
        CHECK_EQ(check_against_naive(*f.trace, *f.tl), 0);
    }
    {
        Fixture f(synth::build_sample().bytes);
        CHECK_EQ(check_against_naive(*f.trace, *f.tl), 0);
    }
    int traces = 0, events = 0;
    for (std::uint32_t seed = 1; seed <= 150; ++seed) {
        Fixture f(random_run(seed));
        if (f.trace->error_count() != 0) {
            std::printf("    seed %u: %u errors\n", seed, f.trace->error_count());
            for (const rv::Issue &is : f.trace->issues())
                if (is.severity == rv::Issue::Error) std::printf("      %s\n", is.message.c_str());
        }
        CHECK_EQ(f.trace->error_count(), 0u);
        const int bad = check_against_naive(*f.trace, *f.tl);
        if (bad != 0) std::printf("    seed %u: %d mismatches\n", seed, bad);
        CHECK_EQ(bad, 0);
        ++traces;
        events += static_cast<int>(f.tl->size());
    }
    std::printf("    %d random runs, %d events compared\n", traces, events);
}

TEST(state_naive_replay_on_corrupt_inputs) {
    // Mutated runs: whatever the reader makes of them, the timeline agrees
    // with the naive replay and nothing throws.
    std::mt19937 rng(7);
    int compared = 0;
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        std::vector<unsigned char> b = random_run(seed);
        for (int k = 0; k < 6; ++k) b[24 + rng() % (b.size() - 24)] = static_cast<unsigned char>(rng());
        std::unique_ptr<rv::Trace> t;
        try {
            t = rvtest::open_bytes(b);
        } catch (const std::exception &) {
            continue;
        }
        rv::Timeline tl(*t, rv::FitLengthPacing());
        CHECK_EQ(check_against_naive(*t, tl), 0);
        // The overlay builds at every event.
        const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
        rv::Overlay ov;
        rv::Theme theme;
        for (std::uint32_t e = 0; e < tl.size(); ++e) {
            rv::OverlayInput in;
            in.trace = t.get();
            in.timeline = &tl;
            in.geometry = &geo;
            in.theme = &theme;
            in.event = e;
            in.time = tl.events()[e].start;
            ov.build(in);
        }
        ++compared;
    }
    CHECK(compared > 10);
}

// --- geometry -----------------------------------------------------------------------

TEST(commit_geometry_in_board_mm) {
    synth::TimelineWriter tw;
    tw.start(20, 10); // cell 0.25 mm, origin (100.5, 50.25)
    // F.Cu (2,3) (3,3) (4,3) then diagonal (5,4) (6,5), a layer change, B.Cu
    // (6,5) (6,6) (6,7); a listed via at (6,5) and one outside the grid.
    tw.commit(2, {C(0, 2, 3), C(0, 3, 3), C(0, 4, 3), C(0, 5, 4), C(0, 6, 5), C(1, 6, 5), C(1, 6, 6), C(1, 6, 7)}, 0.3,
              {{6, 5}, {40, 1}}, {{1, 0.375, 0.75, 0.5, 0.75, 0.2}});
    tw.via(3, 1, 1);
    // A run of one cell between two layer changes draws nothing; a repeated
    // cell is a dot.
    tw.commit(4, {C(0, 9, 9), C(1, 9, 9), C(0, 9, 9), C(0, 9, 9)}, 0.2);
    auto t = rvtest::open_bytes(tw.w.out);
    const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
    CHECK_EQ(geo.size(), std::size_t(3));
    auto X = [](std::int32_t i) { return (100500000LL + std::llround(i * 0.25 * 1e6)) * 1e-6; };
    auto Y = [](std::int32_t j) { return (50250000LL + std::llround(j * 0.25 * 1e6)) * 1e-6; };
    const rv::CopperGeom &g = geo[0];
    CHECK(g.drawable);
    CHECK_EQ(g.width, 0.3);
    CHECK_EQ(g.lines.size(), std::size_t(2));
    if (g.lines.size() == 2) {
        // F.Cu: (2,3) -> (4,3) -> (6,5): the collinear interior points go.
        CHECK_EQ(g.lines[0].layer, 0u);
        CHECK_EQ(g.lines[0].count, 3u);
        const std::int32_t want0[] = {2, 3, 4, 3, 6, 5};
        for (int k = 0; k < 3; ++k) {
            CHECK(std::fabs(g.pts[2 * (g.lines[0].first + k)] - X(want0[2 * k])) < 1e-9);
            CHECK(std::fabs(g.pts[2 * (g.lines[0].first + k) + 1] - Y(want0[2 * k + 1])) < 1e-9);
        }
        CHECK_EQ(g.lines[1].layer, 1u);
        CHECK_EQ(g.lines[1].count, 2u);
        CHECK(std::fabs(g.pts[2 * g.lines[1].first] - X(6)) < 1e-9);
        CHECK(std::fabs(g.pts[2 * g.lines[1].first + 1] - Y(5)) < 1e-9);
        CHECK(std::fabs(g.pts[2 * (g.lines[1].first + 1) + 1] - Y(7)) < 1e-9);
    }
    // The via inside the grid only; the stub from router mm.
    CHECK_EQ(g.vias.size(), std::size_t(1));
    if (!g.vias.empty()) {
        CHECK(std::fabs(g.vias[0][0] - 102.0) < 1e-9);
        CHECK(std::fabs(g.vias[0][1] - 51.5) < 1e-9);
    }
    CHECK_EQ(g.stubs.size(), std::size_t(1));
    if (!g.stubs.empty()) {
        CHECK_EQ(g.stubs[0].layer, 1u);
        CHECK(std::fabs(g.stubs[0].x0 - 100.875) < 1e-9);
        CHECK(std::fabs(g.stubs[0].y0 - 51.0) < 1e-9);
        CHECK(std::fabs(g.stubs[0].x1 - 101.0) < 1e-9);
        CHECK_EQ(g.stubs[0].w, 0.2);
    }
    CHECK_EQ(g.via_dia, 0.6);
    CHECK_EQ(g.via_drill, 0.3);
    // The standalone via.
    CHECK_EQ(geo[1].vias.size(), std::size_t(1));
    CHECK(std::fabs(geo[1].vias[0][0] - 100.75) < 1e-9 && std::fabs(geo[1].vias[0][1] - 50.5) < 1e-9);
    // The one-cell run draws nothing; the repeated cell is a dot.
    CHECK_EQ(geo[2].lines.size(), std::size_t(1));
    if (!geo[2].lines.empty()) {
        CHECK_EQ(geo[2].lines[0].count, 1u);
        CHECK_EQ(geo[2].lines[0].layer, 0u);
    }
    // The overlay draws them: two polylines, a stub and two via discs per
    // layer, drills for both vias.
    rv::Timeline tl(*t, rv::FitLengthPacing());
    rv::Overlay ov;
    rv::Theme theme;
    rv::OverlayInput in;
    in.trace = t.get();
    in.timeline = &tl;
    in.geometry = &geo;
    in.theme = &theme;
    in.event = tl.size() - 1;
    in.time = tl.total();
    ov.build(in);
    std::uint32_t strokes = 0, segs = 0, discs = 0, drills = 0;
    for (const rv::Batch &b : ov.list().batches) {
        if (b.order == rv::order_key(rv::kRankHoles, rv::sub::kRoutedDrill)) drills += b.count;
        else if (b.prim == rv::Prim::Stroke && b.order < rv::order_key(rv::kRankOverlay, 0)) strokes += b.count;
        else if (b.prim == rv::Prim::Segs) segs += b.count;
        else if (b.prim == rv::Prim::Disc && b.order < rv::order_key(rv::kRankOverlay, 0)) discs += b.count;
        CHECK(b.key >= rv::kKeyBase);
    }
    CHECK_EQ(strokes, 3u);
    CHECK_EQ(segs, 1u);
    CHECK_EQ(discs, 4u);
    CHECK_EQ(drills, 2u);
    CHECK_EQ(ov.slots().size(), std::size_t(4));
}

// --- ghosts, failures, the board ------------------------------------------------------

TEST(ghost_alpha_over_time) {
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    const std::uint32_t e = find_event(tl, rv::EvKind::RipUp); // rips /SIG's first commit
    const double t0 = tl.events()[e].start;
    std::vector<std::pair<std::uint32_t, double>> g;
    tl.ghosts(t0, g);
    CHECK_EQ(g.size(), std::size_t(1));
    if (!g.empty()) CHECK_EQ(g[0].second, 1.0);
    g.clear();
    tl.ghosts(t0 + 0.75, g);
    CHECK_EQ(g.size(), std::size_t(1));
    if (!g.empty()) CHECK(std::fabs(g[0].second - 0.5) < 1e-9);
    g.clear();
    tl.ghosts(t0 + rv::Timeline::kGhostSeconds, g);
    CHECK(g.empty());
    g.clear();
    tl.ghosts(std::nextafter(t0, 0.0), g);
    CHECK(g.empty()); // not before the rip-up
    CHECK_EQ(rv::ghost_level(1.0), rv::kGhostLevels);
    CHECK_EQ(rv::ghost_level(0.5), rv::kGhostLevels / 2);
    CHECK_EQ(rv::ghost_level(1e-9), 1);

    // Through the C interface: the ghost batch fades with the clock.
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "run.grtrace", synth::build_timeline_sample(), path));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_time(s.s, t0, &changed), RV_OK);
    auto ghost_alpha = [&]() -> int {
        rv_display_list d;
        const rv_slot *slots = nullptr;
        uint32_t n = 0;
        rv_get_overlay(s.s, &d, &slots, &n);
        for (uint32_t k = 0; k < d.batch_count; ++k)
            if ((d.batches[k].order & 0xFF) == RV_SUB_GHOST) return static_cast<int>(d.batches[k].rgba & 0xFF);
        return -1;
    };
    CHECK_EQ(ghost_alpha(), static_cast<int>(std::lround(0.75 * 255)));
    CHECK_EQ(rv_seek_time(s.s, t0 + 0.75, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_OVERLAY) != 0);
    CHECK_EQ(ghost_alpha(), static_cast<int>(std::lround(0.75 * 8 / 16.0 * 255)));
    rv_position p;
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.ghosts, 1u);
    CHECK_EQ(rv_seek_time(s.s, t0 + 1.6, &changed), RV_OK);
    CHECK_EQ(ghost_alpha(), -1);
    // Hiding routed copper hides ghosts too.
    CHECK_EQ(rv_seek_time(s.s, t0 + 0.1, &changed), RV_OK);
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_ROUTED, 0), RV_OK);
    CHECK_EQ(ghost_alpha(), -1);
}

TEST(failure_marker_lifetime_and_place) {
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    auto alive_at = [&](std::uint32_t e) {
        std::vector<std::uint32_t> v;
        tl.alive_failures(e, v);
        return v;
    };
    // The GND route_net that fails two pads is the 5th RNE.
    const std::uint32_t rne = find_event(tl, rv::EvKind::RouteEnd, 4);
    CHECK((tl.events()[rne].flags & rv::evf::kFailed) != 0);
    CHECK(alive_at(rne - 1).empty());
    CHECK_EQ(alive_at(rne).size(), std::size_t(2));
    const std::uint32_t drop = find_event(tl, rv::EvKind::Drop);
    CHECK_EQ(alive_at(drop).size(), std::size_t(3));
    // Places: TP1 at its RNB pad; J2.1 at its pad; the DROP at the centre
    // of its search's sources (11..13, 22).
    for (const rv::Failure &fa : tl.failures()) {
        CHECK(fa.placed);
        if (fa.label == "TP1") CHECK(std::fabs(fa.x - 110.0) < 1e-9 && std::fabs(fa.y - 50.75) < 1e-9);
        else CHECK(std::fabs(fa.x - 103.5) < 1e-9 && std::fabs(fa.y - 55.75) < 1e-9);
        CHECK_EQ(fa.name, std::string("GND"));
    }
    CHECK(tl.failures()[2].from_drop);
    // The next route_net of GND clears all three, at its RNB.
    const std::uint32_t rnb = find_event(tl, rv::EvKind::RouteBegin, 5);
    CHECK_EQ(alive_at(rnb - 1).size(), std::size_t(3));
    CHECK(alive_at(rnb).empty());
    for (const rv::Failure &fa : tl.failures()) CHECK_EQ(fa.death, rnb);
    // END: its failed list is what is alive (here none).
    CHECK(alive_at(tl.size() - 1).empty());
    // A new session clears failures too (random runs check it against the
    // naive replay); the step kind finds both failure events.
    CHECK_EQ(rv::step_target(tl, 0, rv::StepKind::Failure, 1), rne);
    CHECK_EQ(rv::step_target(tl, tl.events()[rne].start, rv::StepKind::Failure, 1), drop);
    CHECK_EQ(rv::step_target(tl, tl.events()[drop].start, rv::StepKind::Failure, 1), rv::kNone32);
}

TEST(board_underneath_and_no_double_drawing) {
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    const rv::Trace &t = *f.trace;
    const std::uint32_t final_e = find_event(tl, rv::EvKind::Board, 1);
    CHECK_EQ(t.boards()[1].label, std::string("final"));
    // Before the final board: the placed board, routed copper drawn.
    CHECK_EQ(tl.events()[final_e - 1].board, 0u);
    CHECK(!tl.board_holds_routing(final_e - 1, 0));
    // At it: the final board already holds the routing.
    CHECK_EQ(tl.events()[final_e].board, 1u);
    CHECK(tl.board_holds_routing(final_e, 1));
    // An explicitly chosen final board at a position before it: drawn both.
    CHECK(!tl.board_holds_routing(final_e - 1, 1));
    // The second session changes copper after the final board: drawn.
    CHECK(!tl.board_holds_routing(tl.size() - 1, 1));
    // The placed board has no tracks: never holds routing.
    CHECK(!tl.board_holds_routing(final_e, 0));
    // The final board's tracks and vias are exactly the copper alive at it.
    const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(t);
    rv::Trace &tm = *f.trace;
    const BoardCheck bc = check_board(tm, tl, geo, 1);
    CHECK(bc.has);
    CHECK_EQ(bc.board_in_routed, 1.0);
    CHECK_EQ(bc.routed_in_board, 1.0);
    CHECK_EQ(bc.vias_matched, bc.board_vias);
    CHECK_EQ(bc.routed_vias, bc.board_vias);
    // And ROUT lies within the copper.
    const RoutCheck rc = check_rout(t, tl, geo);
    CHECK(rc.has);
    CHECK_EQ(rc.seg_cover, 1.0);
    CHECK_EQ(rc.vias_matched, rc.vias);

    // Through the C interface.
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "run.grtrace", synth::build_timeline_sample(), path));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    rv_position p;
    uint32_t sel = 9, changed = 0;
    CHECK_EQ(rv_seek_event(s.s, final_e - 1, &changed), RV_OK);
    CHECK_EQ(rv_board_selected(s.s, &sel), RV_OK);
    CHECK_EQ(sel, 0u);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.routed_hidden, 0u);
    CHECK(p.copper_alive > 0);
    CHECK_EQ(rv_seek_event(s.s, final_e, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_BOARD) != 0);
    CHECK_EQ(rv_board_selected(s.s, &sel), RV_OK);
    CHECK_EQ(sel, 1u);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.routed_hidden, 1u);
    rv_display_list d;
    const rv_slot *slots = nullptr;
    uint32_t n = 0;
    CHECK_EQ(rv_get_overlay(s.s, &d, &slots, &n), RV_OK);
    for (uint32_t k = 0; k < d.batch_count; ++k) CHECK(d.batches[k].order >= rv::order_key(rv::kRankOverlay, 0));
    // Explicitly the final board, back before it: routing drawn on it.
    CHECK_EQ(rv_board_select(s.s, 1), RV_OK);
    CHECK_EQ(rv_seek_event(s.s, final_e - 1, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.routed_hidden, 0u);
    CHECK_EQ(rv_board_selected(s.s, &sel), RV_OK);
    CHECK_EQ(sel, 1u);
}

TEST(router_outline_without_a_board) {
    synth::TimelineWriter tw;
    tw.start(20, 10);
    tw.pad(1, 1.0, 1.0, 0.3);
    synth::WRect r;
    r.op = 2;
    r.net = synth::BLOCK;
    r.layers = 2;
    r.levels = 1;
    r.x0 = 4;
    r.y0 = 3;
    r.x1 = 5;
    r.y1 = 4;
    tw.w.chunk("RECT", synth::encode(r));
    synth::WDisc dc;
    dc.net = 1;
    dc.layer = 1;
    dc.i = 8;
    dc.j = 2;
    dc.r_t = 2;
    tw.w.chunk("DISC", synth::encode(dc));
    tw.commit(1, {C(0, 1, 1), C(0, 9, 1)});
    tw.end({});
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "outline.grtrace", tw.w.out, path));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    uint32_t n = 0;
    CHECK_EQ(rv_layer_count(s.s, &n), RV_OK);
    CHECK_EQ(n, 3u);
    std::set<std::string> names;
    for (uint32_t k = 0; k < n; ++k) {
        rv_layer_info li;
        CHECK_EQ(rv_layer(s.s, k, &li), RV_OK);
        names.insert(li.name);
    }
    CHECK(names == std::set<std::string>({"F.Cu", "B.Cu", "Edge.Cuts"}));
    rv_display_list d;
    CHECK_EQ(rv_get_display_list(s.s, &d), RV_OK);
    CHECK_EQ(d.board, RV_NONE);
    CHECK_EQ(d.has_extent, 1u);
    CHECK(std::fabs(d.extent[0] - (100.5 - 0.125)) < 1e-4);
    CHECK(std::fabs(d.extent[2] - (100.5 + 19.5 * 0.25)) < 1e-4);
    CHECK(std::fabs(d.extent[3] - (50.25 + 9.5 * 0.25)) < 1e-4);
    uint32_t strokes = 0, rings = 0;
    for (uint32_t k = 0; k < d.batch_count; ++k) {
        if (d.batches[k].prim == RV_STROKE) strokes += d.batches[k].count;
        if (d.batches[k].prim == RV_RING) rings += d.batches[k].count;
    }
    CHECK_EQ(strokes, 3u); // grid extent, pad, block_cells
    CHECK_EQ(rings, 1u);
    // The routed copper is in the overlay, on the outline's layers.
    const rv_slot *slots = nullptr;
    CHECK_EQ(rv_get_overlay(s.s, &d, &slots, &n), RV_OK);
    CHECK(d.batch_count > 0);
    CHECK_EQ(n, 4u);
    CHECK_EQ(slots[0].kind, RV_SLOT_ROUTED);
}

// --- the clock, stepping, seeking -------------------------------------------------------

TEST(playback_clock_speed_and_pause) {
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "run.grtrace", synth::build_timeline_sample(), path));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    rv_position p;
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.at_end, 1u); // opens at the end, paused
    CHECK_EQ(p.playing, 0u);
    CHECK_EQ(p.time, p.total);
    uint32_t changed = 9;
    CHECK_EQ(rv_advance(s.s, 1.0, &changed), RV_OK);
    CHECK_EQ(changed, 0u); // paused: nothing moves
    // Playing at the end starts over.
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_POSITION) != 0);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, 0.0);
    CHECK_EQ(p.playing, 1u);
    CHECK_EQ(rv_advance(s.s, 1.5, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, 1.5);
    // Speeds snap to the list and clamp.
    CHECK_EQ(rv_set_speed(s.s, 3.0), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.speed, 4.0);
    CHECK_EQ(rv_advance(s.s, 0.5, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, 3.5);
    CHECK_EQ(rv_speed_step(s.s, 20), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.speed, 64.0);
    CHECK_EQ(rv_speed_step(s.s, -20), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.speed, 0.25);
    const char *txt = nullptr;
    CHECK_EQ(rv_text(s.s, RV_TEXT_SPEED, &txt), RV_OK);
    CHECK_STREQ(txt, "0.25\xc3\x97");
    CHECK_EQ(rv_set_speed(s.s, 0), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_set_speed(s.s, 64), RV_OK);
    // Playing past the end stops there.
    CHECK_EQ(rv_advance(s.s, 1000.0, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.at_end, 1u);
    CHECK_EQ(p.playing, 0u);
    // Backward: from the end down to the start, then it stops.
    CHECK_EQ(rv_set_direction(s.s, -1), RV_OK);
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK_EQ(rv_advance(s.s, 1.0, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, p.total - 64.0);
    CHECK_EQ(rv_advance(s.s, 10.0, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, 0.0);
    CHECK_EQ(p.playing, 0u);
    // Playing backward at the start starts over from the end.
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, p.total);
    CHECK_EQ(rv_play(s.s, 0, &changed), RV_OK);
    CHECK_EQ(rv_advance(s.s, -1.0, &changed), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_advance(s.s, std::nan(""), &changed), RV_E_INVALID_ARGUMENT);
    // Seeks clamp.
    CHECK_EQ(rv_seek_fraction(s.s, 2.0, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, p.total);
    CHECK_EQ(rv_seek_time(s.s, -3, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.time, 0.0);
    CHECK_EQ(p.event, 0u);
    CHECK_EQ(rv_seek_event(s.s, 99999, &changed), RV_E_INVALID_ARGUMENT);
}

TEST(stepping_by_kind_at_both_ends) {
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "run.grtrace", synth::build_timeline_sample(), path));
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    rv_position p;
    uint32_t changed = 0;
    for (uint32_t kind = 0; kind < RV_STEP_KIND_COUNT; ++kind) {
        std::vector<std::uint32_t> want;
        for (std::uint32_t e = 0; e < tl.size(); ++e)
            if (rv::step_matches(tl.events()[e], static_cast<rv::StepKind>(kind))) want.push_back(e);
        // Forward from the start.
        CHECK_EQ(rv_jump(s.s, 0, &changed), RV_OK);
        std::vector<std::uint32_t> got;
        if (!want.empty() && want[0] == 0) got.push_back(0);
        for (;;) {
            CHECK_EQ(rv_step(s.s, kind, 1, &changed), RV_OK);
            if (changed == 0) break;
            CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
            CHECK_EQ(p.fraction, 0.0);
            got.push_back(p.event);
        }
        CHECK(got == want);
        // Backward from the end.
        CHECK_EQ(rv_jump(s.s, 1, &changed), RV_OK);
        std::vector<std::uint32_t> back;
        for (;;) {
            CHECK_EQ(rv_step(s.s, kind, -1, &changed), RV_OK);
            if (changed == 0) break;
            CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
            back.push_back(p.event);
        }
        std::reverse(back.begin(), back.end());
        CHECK(back == want);
    }
    // Stepping pauses.
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, -1, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.playing, 0u);
    CHECK_EQ(rv_step(s.s, RV_STEP_KIND_COUNT, 1, &changed), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, 0, &changed), RV_E_INVALID_ARGUMENT);
    // Mid-event, stepping back lands on that event's start.
    CHECK_EQ(rv_seek_event(s.s, 10, &changed), RV_OK);
    CHECK_EQ(rv_seek_time(s.s, tl.events()[10].start + tl.events()[10].duration / 2, &changed), RV_OK);
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, -1, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.event, 10u);
    CHECK_EQ(p.time, tl.events()[10].start);
}

TEST(seek_equivalence_play_step_and_seek) {
    std::vector<std::vector<unsigned char>> traces = {synth::build_timeline_sample(), random_run(11), random_run(23),
                                                      synth::build_sample().bytes};
    rvtest::TempDir dir;
    int compared = 0;
    for (std::size_t n = 0; n < traces.size(); ++n) {
        std::string path;
        CHECK(write_temp(dir, "t" + std::to_string(n) + ".grtrace", traces[n], path));
        SessionGuard a, b;
        CHECK_EQ(rv_open(a.s, path.c_str()), RV_OK);
        CHECK_EQ(rv_open(b.s, path.c_str()), RV_OK);
        uint32_t changed = 0;
        // Play from the start at 64x in uneven steps, recording each frame.
        CHECK_EQ(rv_set_speed(a.s, 64), RV_OK);
        CHECK_EQ(rv_play(a.s, 1, &changed), RV_OK);
        std::vector<std::pair<double, State>> frames;
        std::map<uint32_t, std::string> keys; // overlay key -> geometry
        std::mt19937 rng(static_cast<std::uint32_t>(n));
        State last = state_of(a.s);
        for (int k = 0; k < 400; ++k) {
            rv_position p;
            CHECK_EQ(rv_position_get(a.s, &p), RV_OK);
            if (!p.playing) break;
            CHECK_EQ(rv_advance(a.s, 0.002 + (rng() % 100) * 0.0001, &changed), RV_OK);
            CHECK_EQ(rv_position_get(a.s, &p), RV_OK);
            const State st = state_of(a.s);
            // OVERLAY is reported exactly when the overlay content changed
            // (or a slot's content did without the text changing: never).
            if (st.overlay != last.overlay) CHECK((changed & RV_CHANGED_OVERLAY) != 0);
            if (st.board != last.board) CHECK((changed & RV_CHANGED_BOARD) != 0);
            last = st;
            frames.emplace_back(p.time, st);
            rv_display_list d;
            const rv_slot *slots = nullptr;
            uint32_t ns = 0;
            rv_get_overlay(a.s, &d, &slots, &ns);
            for (uint32_t q = 0; q < d.batch_count; ++q) {
                const std::string g = batch_geometry(d, d.batches[q]);
                auto it = keys.find(d.batches[q].key);
                if (it == keys.end()) keys[d.batches[q].key] = g;
                else CHECK(it->second == g);
            }
        }
        CHECK(!frames.empty());
        // Seek there directly in another session, forward then backward.
        for (const auto &fr : frames) {
            CHECK_EQ(rv_seek_time(b.s, fr.first, &changed), RV_OK);
            CHECK(same(state_of(b.s), fr.second));
            ++compared;
        }
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            CHECK_EQ(rv_seek_time(b.s, it->first, &changed), RV_OK);
            CHECK(same(state_of(b.s), it->second));
            ++compared;
        }
        // Stepping event by event equals seeking to each event, both ways.
        rv_timeline tl;
        CHECK_EQ(rv_get_timeline(a.s, &tl), RV_OK);
        CHECK_EQ(rv_jump(a.s, 0, &changed), RV_OK);
        for (uint32_t e = 1; e < tl.event_count; ++e) {
            CHECK_EQ(rv_step(a.s, RV_STEP_EVENT, 1, &changed), RV_OK);
            CHECK_EQ(rv_seek_event(b.s, e, &changed), RV_OK);
            CHECK(same(state_of(a.s), state_of(b.s)));
            ++compared;
        }
        for (uint32_t e = tl.event_count; e-- > 1;) {
            CHECK_EQ(rv_seek_event(b.s, e - 1, &changed), RV_OK);
            CHECK_EQ(rv_step(a.s, RV_STEP_EVENT, -1, &changed), RV_OK);
            CHECK(same(state_of(a.s), state_of(b.s)));
            ++compared;
        }
    }
    std::printf("    %d positions compared\n", compared);
}

TEST(generations_and_borrowed_lists) {
    rvtest::TempDir dir;
    std::string path;
    CHECK(write_temp(dir, "run.grtrace", synth::build_timeline_sample(), path));
    Fixture f(synth::build_timeline_sample());
    const rv::Timeline &tl = *f.tl;
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    uint32_t changed = 0;
    // Two positions on the placed board: the static list is untouched.
    const std::uint32_t c1 = find_event(tl, rv::EvKind::Commit, 0), c2 = find_event(tl, rv::EvKind::Commit, 2);
    CHECK_EQ(rv_seek_event(s.s, c1, &changed), RV_OK);
    rv_display_list st, ov;
    const rv_slot *slots = nullptr;
    uint32_t ns = 0;
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    const uint32_t geo = st.geometry_generation, gen = st.generation;
    const rv_batch *batches = st.batches;
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    const uint32_t ogen = ov.generation, oepoch = ov.geometry_generation;
    CHECK_EQ(rv_seek_event(s.s, c2, &changed), RV_OK);
    CHECK_EQ(changed & RV_CHANGED_BOARD, 0u);
    CHECK((changed & RV_CHANGED_OVERLAY) != 0);
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    CHECK_EQ(st.geometry_generation, geo);
    CHECK_EQ(st.generation, gen);
    CHECK(st.batches == batches);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    CHECK(ov.generation != ogen);
    CHECK_EQ(ov.geometry_generation, oepoch);
    // From a commit to the route_net end after it: only the markers slot
    // (the highlight) changes; the copper slots keep their generation.
    const std::uint32_t c3 = find_event(tl, rv::EvKind::Commit, 3);
    CHECK_EQ(rv_seek_event(s.s, c3, &changed), RV_OK);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    std::vector<uint32_t> slot_gens;
    for (uint32_t k = 0; k < ns; ++k) slot_gens.push_back(slots[k].generation);
    CHECK_EQ(rv_seek_event(s.s, c3 + 1, &changed), RV_OK);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    CHECK_EQ(ns, std::uint32_t(slot_gens.size()));
    for (uint32_t k = 0; k < ns && k < slot_gens.size(); ++k) {
        if (slots[k].kind == RV_SLOT_MARKERS) CHECK(slots[k].generation != slot_gens[k]);
        else CHECK_EQ(slots[k].generation, slot_gens[k]);
    }
    // Slots are sorted, disjoint, and no static batch falls inside one.
    for (uint32_t k = 0; k < ns; ++k) {
        CHECK(slots[k].order_begin < slots[k].order_end);
        if (k > 0) CHECK(slots[k - 1].order_end <= slots[k].order_begin);
        for (uint32_t q = 0; q < st.batch_count; ++q)
            CHECK(st.batches[q].order < slots[k].order_begin || st.batches[q].order >= slots[k].order_end);
        for (uint32_t q = slots[k].first_batch; q < slots[k].first_batch + slots[k].batch_count; ++q)
            CHECK(ov.batches[q].order >= slots[k].order_begin && ov.batches[q].order < slots[k].order_end);
    }
    // Camera calls change neither list.
    const uint32_t ogen2 = ov.generation;
    CHECK_EQ(rv_view_set_size(s.s, 800, 600, 2), RV_OK);
    CHECK_EQ(rv_view_fit(s.s), RV_OK);
    CHECK_EQ(rv_view_zoom(s.s, 2, 10, 10), RV_OK);
    CHECK_EQ(rv_view_pan(s.s, 5, 5), RV_OK);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    CHECK_EQ(ov.generation, ogen2);
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    CHECK_EQ(st.generation, gen);
    // Inside one search event the overlay does not change: only POSITION.
    const std::uint32_t se = find_event(tl, rv::EvKind::Search, 3);
    CHECK_EQ(rv_seek_time(s.s, tl.events()[se].start + 1e-6, &changed), RV_OK);
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK_EQ(rv_set_speed(s.s, 0.25), RV_OK);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    const uint32_t g3 = ov.generation;
    CHECK_EQ(rv_advance(s.s, tl.events()[se].duration, &changed), RV_OK);
    CHECK_EQ(changed, RV_CHANGED_POSITION);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    CHECK_EQ(ov.generation, g3);
    // Hiding a router layer changes the overlay, not the board's geometry.
    CHECK_EQ(rv_layer_set_visible(s.s, "B.Cu", 0), RV_OK);
    CHECK_EQ(rv_get_overlay(s.s, &ov, &slots, &ns), RV_OK);
    CHECK(ov.generation != g3);
    for (uint32_t k = 0; k < ov.batch_count; ++k)
        if (ov.batches[k].cls == RV_OBJ_ROUTED && ov.batches[k].order < rv::order_key(rv::kRankOverlay, 0) &&
            ov.batches[k].layer != RV_NONE) {
            rv_layer_info li;
            bool found = false;
            uint32_t nl = 0;
            rv_layer_count(s.s, &nl);
            for (uint32_t q = 0; q < nl; ++q)
                if (rv_layer(s.s, q, &li) == RV_OK && li.index == ov.batches[k].layer) found = std::string(li.name) == "B.Cu";
            CHECK(!found);
        }
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    CHECK_EQ(st.geometry_generation, geo);
    // Moving across a BRD and back reuses both static lists.
    const std::uint32_t fin = find_event(tl, rv::EvKind::Board, 1);
    CHECK_EQ(rv_seek_event(s.s, fin, &changed), RV_OK);
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    const uint32_t geo_final = st.geometry_generation;
    CHECK(geo_final != geo);
    CHECK_EQ(rv_seek_event(s.s, c1, &changed), RV_OK);
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    CHECK_EQ(st.geometry_generation, geo);
    CHECK_EQ(rv_seek_event(s.s, fin, &changed), RV_OK);
    CHECK_EQ(rv_get_display_list(s.s, &st), RV_OK);
    CHECK_EQ(st.geometry_generation, geo_final);
    // The timeline array.
    rv_timeline t;
    CHECK_EQ(rv_get_timeline(s.s, &t), RV_OK);
    CHECK_EQ(t.event_count, tl.size());
    CHECK_EQ(t.total, 180.0);
    CHECK_EQ(t.phase_count, 4u);
    float big = 0;
    for (uint32_t k = 0; k < t.event_count; ++k) {
        CHECK(t.events[k].size >= 0 && t.events[k].size <= 1);
        big = std::max(big, t.events[k].size);
    }
    CHECK_EQ(big, 1.0f);
    rv_phase_info ph;
    CHECK_EQ(rv_phase(s.s, 3, &ph), RV_OK);
    CHECK_STREQ(ph.label, "second session");
    CHECK_EQ(ph.end, 180.0);
    CHECK_EQ(rv_phase(s.s, 4, &ph), RV_E_INVALID_ARGUMENT);
    // Captions name the phase, the route_net and the counts.
    const char *cap = nullptr;
    CHECK_EQ(rv_seek_event(s.s, c2, &changed), RV_OK);
    CHECK_EQ(rv_text(s.s, RV_TEXT_CAPTION, &cap), RV_OK);
    const std::string capt = cap;
    CHECK(capt.find("route") == 0);
    CHECK(capt.find("route_net 3/6 /CLK") != std::string::npos);
    CHECK(capt.find("commit of /CLK") != std::string::npos);
    CHECK(capt.find("3 commits, 2 vias, 1 rip-ups, 0 failed pads") != std::string::npos);
    CHECK_EQ(rv_text(s.s, RV_TEXT_POSITION, &cap), RV_OK);
    CHECK(std::string(cap).find("event ") == 0);
    CHECK_EQ(rv_text(s.s, 3, &cap), RV_E_INVALID_ARGUMENT);
}

TEST(refresh_keeps_the_position) {
    rvtest::TempDir dir;
    const std::vector<unsigned char> all = synth::build_timeline_sample();
    Fixture f(all);
    const rv::Timeline &tl = *f.tl;
    // The file up to the end of the first route_net, then the rest.
    const std::uint32_t cut_e = find_event(tl, rv::EvKind::RouteEnd);
    const std::uint32_t cut_chunk = tl.events()[cut_e].last + 1;
    std::size_t cut = f.trace->chunks()[cut_chunk].offset;
    const std::string path = dir.path("live.grtrace");
    CHECK(synth::write_file(path, std::vector<unsigned char>(all.begin(), all.begin() + static_cast<long>(cut))));
    SessionGuard s;
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    rv_position p;
    uint32_t changed = 0, added = 0;
    // At the end: stays at the end.
    CHECK(rvtest::append_file(path, all.data() + cut, 16)); // a partial chunk header: nothing new
    CHECK_EQ(rv_refresh(s.s, &added), RV_OK);
    CHECK_EQ(added, 0u);
    CHECK_EQ(rv_seek_event(s.s, 4, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    const uint32_t chunk_before = p.last_chunk;
    CHECK(rvtest::append_file(path, all.data() + cut + 16, all.size() - cut - 16));
    CHECK_EQ(rv_refresh(s.s, &added), RV_OK);
    CHECK(added > 0);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.event, 4u);
    CHECK_EQ(p.last_chunk, chunk_before);
    CHECK_EQ(p.event_count, tl.size());
    CHECK_EQ(p.fraction, 0.0);
    // Reopen: at the end of the whole run.
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK_EQ(p.at_end, 1u);
    CHECK_EQ(p.event, tl.size() - 1);
}

// --- the final state against ROUT and the final board -------------------------------------

TEST(final_state_against_rout_on_the_recorder_sample) {
    const char *sample = "core/tests/data/small.grtrace";
    std::unique_ptr<rv::Trace> t;
    try {
        t = rv::Trace::open_file(sample);
    } catch (const std::exception &) {
        std::printf("    skipped: sample not present (%s)\n", sample);
        return;
    }
    rv::Timeline tl(*t, rv::FitLengthPacing());
    CHECK_EQ(check_against_naive(*t, tl), 0);
    const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
    const RoutCheck rc = check_rout(*t, tl, geo);
    std::printf("    sample: %u events; ROUT %s, %.1f mm of segments, %.4f within router copper, vias %zu of %zu\n",
                tl.size(), rc.has ? "present" : "absent", rc.total_len, rc.seg_cover, rc.vias_matched, rc.vias);
    if (rc.has) {
        CHECK_EQ(rc.seg_cover, 1.0);
        CHECK_EQ(rc.vias_matched, rc.vias);
    }
}

// Optional: real traces named in RV_REAL_TRACES (paths separated by ':').
// Prints how the routed copper at the end compares with ROUT and with the
// last board that holds tracks.
TEST(final_state_on_real_traces) {
    const char *env = std::getenv("RV_REAL_TRACES");
    if (env == nullptr || *env == '\0') {
        std::printf("    skipped: RV_REAL_TRACES not set\n");
        return;
    }
    std::string all = env;
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t end = all.find(':', pos);
        const std::string path = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? all.size() + 1 : end + 1;
        if (path.empty()) continue;
        std::unique_ptr<rv::Trace> t = rv::Trace::open_file(path);
        rv::Timeline tl(*t, rv::FitLengthPacing());
        const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
        const int bad = check_against_naive(*t, tl);
        CHECK_EQ(bad, 0);
        const RoutCheck rc = check_rout(*t, tl, geo);
        std::printf("    %s\n      %u events, naive replay mismatches %d\n", path.c_str(), tl.size(), bad);
        if (rc.has) {
            std::printf("      ROUT: %.3f mm of segments, fraction within router copper %.6f; vias %zu of %zu matched\n",
                        rc.total_len, rc.seg_cover, rc.vias_matched, rc.vias);
            CHECK_EQ(rc.seg_cover, 1.0);
            CHECK_EQ(rc.vias_matched, rc.vias);
        }
        for (std::uint32_t b = t->boards().size(); b-- > 0;) {
            const rv::BoardInfo &bi = t->boards()[b];
            if (bi.records_by_kind[rv::brd_kind::kTrack] == 0) continue;
            const BoardCheck bc = check_board(*t, tl, geo, b);
            if (!bc.has) break;
            std::printf("      board \"%s\": %.3f mm of tracks, %.6f on router copper; router copper %.3f mm, %.6f on "
                        "board tracks; board vias %zu, %zu at router vias (router vias %zu); copper hidden there: %s\n",
                        bi.label.c_str(), bc.board_len, bc.board_in_routed, bc.routed_len, bc.routed_in_board,
                        bc.board_vias, bc.vias_matched, bc.routed_vias,
                        tl.board_holds_routing(tl.event_of_chunk(bi.chunk), b) ? "yes" : "no");
            break;
        }
    }
}

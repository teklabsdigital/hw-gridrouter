// test_live.cpp: M6, following a trace while it is written.
//
//   - equivalence: a session that read a file piece by piece (refresh after
//     each piece, the pieces splitting chunks anywhere, down to one byte)
//     equals a session that opened the same bytes at once: info, timeline
//     (every event field, ticks, phases), nets table, histories, the overlay,
//     the wavefront images, hit tests and the selection at several
//     positions;
//   - the timeline's `unchanged` promise and the position kept by a refresh;
//   - the follow rule, live states and texts, replacement and waiting;
//   - the interval index grown in place, and the dim rectangle's key.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "overlay.hpp"
#include "routed.hpp"
#include "rv.h"
#include "session.hpp"
#include "theme.hpp"
#include "timeline.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

struct Session {
    rv_session *s = nullptr;
    Session() { rv_create(&s); }
    ~Session() { rv_destroy(s); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

using Bytes = std::vector<unsigned char>;

// Chunk boundaries of a trace (offsets of each chunk header and the end).
std::vector<std::size_t> chunk_offsets(const Bytes &b) {
    std::vector<std::size_t> out;
    std::size_t off = 24;
    while (off + 16 <= b.size()) {
        out.push_back(off);
        std::uint32_t len;
        std::memcpy(&len, b.data() + off + 4, 4);
        off += 16 + len + (8 - len % 8) % 8;
    }
    out.push_back(off);
    return out;
}

void add(std::string &o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void add(std::string &o, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    o += buf;
}

std::string bits(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%a", v);
    return buf;
}

std::uint64_t hash_bytes(const void *p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    const unsigned char *c = static_cast<const unsigned char *>(p);
    for (std::size_t k = 0; k < n; ++k) {
        h ^= c[k];
        h *= 1099511628211ull;
    }
    return h;
}

void list_text(std::string &o, const rv_display_list &d, bool keys) {
    add(o, "list %u batches board %u extent %u\n", d.batch_count, d.board, d.has_extent);
    for (uint32_t k = 0; k < d.batch_count; ++k) {
        const rv_batch &b = d.batches[k];
        add(o, " b prim %u cls %u layer %u order %u rgba %08x w %s n %u bbox %s %s %s %s", b.prim, b.cls, b.layer,
            b.order, b.rgba, bits(b.width).c_str(), b.count, bits(b.bbox[0]).c_str(), bits(b.bbox[1]).c_str(),
            bits(b.bbox[2]).c_str(), bits(b.bbox[3]).c_str());
        if (keys) add(o, " key %u", b.key);
        o += "\n";
    }
    std::uint64_t h = hash_bytes(d.xy, 8ull * d.point_count);
    h = hash_bytes(d.contours, 8ull * d.contour_count, h);
    h = hash_bytes(d.polys, 8ull * d.poly_count, h);
    h = hash_bytes(d.segs, 16ull * d.seg_count, h);
    h = hash_bytes(d.circles, 12ull * d.circle_count, h);
    add(o, " geometry %016llx\n", static_cast<unsigned long long>(h));
}

// The state at the current position: position, texts, overlay, wavefront
// images, hit tests, selection, nets table rows.
std::string position_state(rv_session *s) {
    std::string o;
    rv_position p;
    rv_position_get(s, &p);
    add(o, "pos t %s total %s frac %s ev %u/%u last %u end %u cells %u cu %u gh %u fail %u hid %u net %d/%u set %u\n",
        bits(p.time).c_str(), bits(p.total).c_str(), bits(p.fraction).c_str(), p.event, p.event_count, p.last_chunk,
        p.at_end, p.cells_revealed, p.copper_alive, p.ghosts, p.failures, p.routed_hidden, p.net, p.has_net,
        p.settled);
    for (uint32_t w = 0; w <= RV_TEXT_PACING; ++w) {
        if (w == RV_TEXT_SPEED) continue;
        const char *t = nullptr;
        rv_text(s, w, &t);
        add(o, "text %u %s\n", w, t != nullptr ? t : "?");
    }
    rv_display_list d;
    if (rv_get_display_list(s, &d) == RV_OK) list_text(o, d, true);
    const rv_slot *slots = nullptr;
    uint32_t ns = 0;
    if (rv_get_overlay(s, &d, &slots, &ns) == RV_OK) {
        list_text(o, d, false);
        for (uint32_t k = 0; k < ns; ++k)
            add(o, " slot %u %u-%u layer %u batches %u+%u\n", slots[k].kind, slots[k].order_begin, slots[k].order_end,
                slots[k].layer, slots[k].first_batch, slots[k].batch_count);
    }
    uint32_t ni = 0;
    rv_wavefront_image_count(s, &ni);
    for (uint32_t k = 0; k < ni; ++k) {
        rv_wavefront_image im;
        if (rv_get_wavefront_image(s, k, &im) != RV_OK) continue;
        add(o, "wave %u x %u order %u layer %u cells %u search %u px %016llx\n", im.width, im.height, im.order,
            im.layer, im.cells, im.search,
            static_cast<unsigned long long>(im.pixels != nullptr ? hash_bytes(im.pixels, 1ull * im.stride * im.height)
                                                                 : 0));
    }
    // The camera is view state, not the trace's: fit it the same way in both.
    rv_view_set_size(s, 900, 600, 1);
    rv_view_fit(s);
    for (int k = 0; k < 6; ++k) {
        const double vx = 100 + 130 * k, vy = 80 + 70 * k;
        uint32_t nh = 0;
        if (rv_hit_test(s, vx, vy, &nh) != RV_OK) continue;
        for (uint32_t h = 0; h < nh; ++h) {
            rv_hit hit;
            rv_hit_get(s, h, &hit);
            add(o, "hit %d: %u %s ev %u cu %u\n", k, hit.kind, hit.text, hit.event, hit.copper);
        }
    }
    rv_selection sel;
    rv_selection_get(s, &sel);
    add(o, "sel %u %u %d %s %u %u\n", sel.active, sel.has_net, sel.net, sel.name, sel.board_nets, sel.board_items);
    rv_nets_info ni2;
    rv_nets_get(s, &ni2);
    add(o, "nets %u %u sel %u\n", ni2.row_count, ni2.view_count, ni2.selected_position);
    for (uint32_t r = 0; r < ni2.view_count; ++r) {
        for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
            const char *t = nullptr;
            rv_nets_cell(s, r, c, &t);
            add(o, "%s|", t != nullptr ? t : "?");
        }
        o += "\n";
    }
    return o;
}

// Everything a client can read, at several positions.  Moves the position.
std::string session_state(rv_session *s) {
    std::string o;
    rv_trace_info i;
    if (rv_info(s, &i) != RV_OK) return "no trace";
    add(o, "info size %llu end %llu chunks %u issues %u err %u warn %u runs %u nets %u searches %u routes %u cu %u rm %u "
           "complete %u header %u\n",
        static_cast<unsigned long long>(i.file_size), static_cast<unsigned long long>(i.indexed_end), i.chunk_count,
        i.issue_count, i.error_count, i.warning_count, i.run_count, i.net_count, i.search_count, i.route_count,
        i.copper_count, i.copper_removed, i.complete, i.header_complete);
    rv_timeline tl;
    rv_get_timeline(s, &tl);
    add(o, "timeline total %s router %s events %u rips %u fails %u phases %u\n", bits(tl.total).c_str(),
        bits(tl.router_seconds).c_str(), tl.event_count, tl.rip_tick_count, tl.failure_tick_count, tl.phase_count);
    for (uint32_t e = 0; e < tl.event_count; ++e) {
        const rv_event &v = tl.events[e];
        add(o, "e %s %s %s k %u f %u c %u-%u r %u n %d s %u ro %u ph %u b %u\n", bits(v.start).c_str(),
            bits(v.duration).c_str(), bits(v.size).c_str(), v.kind, v.flags, v.chunk, v.last_chunk, v.record, v.net,
            v.session, v.route, v.phase, v.board);
    }
    for (uint32_t k = 0; k < tl.rip_tick_count; ++k) add(o, "rt %s\n", bits(tl.rip_ticks[k]).c_str());
    for (uint32_t k = 0; k < tl.failure_tick_count; ++k) add(o, "ft %s\n", bits(tl.failure_ticks[k]).c_str());
    for (uint32_t k = 0; k < tl.phase_count; ++k) {
        rv_phase_info p;
        rv_phase(s, k, &p);
        add(o, "phase %s %s %s %u\n", bits(p.start).c_str(), bits(p.end).c_str(), p.label, p.event);
    }
    const uint32_t nev = tl.event_count;
    uint32_t nb = 0;
    rv_board_count(s, &nb);
    add(o, "boards %u\n", nb);
    // Histories of every net in the table.
    rv_nets_info ni;
    rv_nets_get(s, &ni);
    std::vector<int32_t> nets;
    for (uint32_t r = 0; r < ni.view_count; ++r) {
        rv_net_row row;
        if (rv_nets_row(s, r, &row) == RV_OK) nets.push_back(row.net);
    }
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    for (int32_t n : nets) {
        uint32_t count = 0;
        rv_net_history(s, n, &count);
        for (uint32_t k = 0; k < count; ++k) {
            rv_history_entry h;
            rv_history_get(s, k, &h);
            add(o, "h %d: %u %u %u %u %s\n", n, h.depth, h.kind, h.event, h.flags, h.text);
        }
        const uint32_t *ev = nullptr;
        uint32_t ne = 0;
        rv_net_events(s, n, &ev, &ne);
        add(o, "net %d events %u\n", n, ne);
        for (uint32_t k = 0; k < ne; ++k) add(o, " %u", ev[k]);
        o += "\n";
    }
    // Positions: a few events, their middles, and the end.
    std::vector<uint32_t> at;
    if (nev > 0) at = {0, nev / 3, nev / 2, (2 * nev) / 3, nev - 1};
    for (uint32_t e : at) {
        rv_seek_event(s, e, nullptr);
        o += position_state(s);
        rv_position p;
        rv_position_get(s, &p);
        rv_event ev = tl.events[e];
        (void)ev;
        rv_seek_time(s, p.time + 0.5 * tl.events[e].duration, nullptr);
        o += position_state(s);
    }
    rv_jump(s, 1, nullptr);
    rv_play(s, 0, nullptr);
    o += position_state(s);
    return o;
}

// Prepare a session as a client would before comparing: a view, a selection.
void prepare(rv_session *s, int32_t select) {
    rv_view_set_size(s, 900, 600, 1);
    rv_view_fit(s);
    if (select >= 0) rv_select_net(s, select, nullptr);
}

std::string fresh_state(const std::string &path, const Bytes &bytes, std::size_t n, const rv_pacing *pacing,
                        int32_t select) {
    synth::write_file(path, Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n)));
    Session f;
    if (pacing != nullptr) rv_set_pacing(f.s, pacing, nullptr);
    if (rv_open_ex(f.s, path.c_str(), RV_OPEN_LIVE) != RV_OK) return std::string("open failed: ") + rv_last_error(f.s);
    prepare(f.s, select);
    return session_state(f.s);
}

// Feed `bytes` to a live session in the pieces `cuts` (ascending, ending at
// bytes.size()), comparing with a fresh open at every cut in `check`.
// Returns the number of comparisons that differed.
int feed_and_compare(const Bytes &bytes, const std::vector<std::size_t> &cuts, const std::vector<bool> &check,
                     const rv_pacing *pacing, int32_t select, bool compare_unchanged) {
    rvtest::TempDir dir;
    const std::string grow = dir.path("grow.grtrace"), fresh = dir.path("fresh.grtrace");
    Session g;
    if (pacing != nullptr) rv_set_pacing(g.s, pacing, nullptr);
    std::size_t have = cuts.empty() ? 0 : cuts[0];
    synth::write_file(grow, Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(have)));
    if (rv_open_ex(g.s, grow.c_str(), RV_OPEN_LIVE) != RV_OK) return 1000;
    prepare(g.s, select);
    int bad = 0;
    std::vector<rv_event> prev;
    double now = 0;
    for (std::size_t k = 0; k < cuts.size(); ++k) {
        const std::size_t to = cuts[k];
        if (to > have) {
            rvtest::append_file(grow, bytes.data() + have, to - have);
            have = to;
        }
        uint32_t added = 0;
        now += 0.25;
        if (rv_poll(g.s, now, &added, nullptr) != RV_OK) {
            std::printf("    poll failed at %zu: %s\n", to, rv_last_error(g.s));
            ++bad;
            continue;
        }
        // Advance a little as a following client would.
        rv_advance(g.s, 1.0 / 60, nullptr);
        if (compare_unchanged) {
            rv_timeline tl;
            rv_get_timeline(g.s, &tl);
            const uint32_t keep = std::min<uint32_t>(tl.unchanged, static_cast<uint32_t>(prev.size()));
            if (tl.unchanged > prev.size() && tl.unchanged != tl.event_count) ++bad;
            for (uint32_t e = 0; e < keep; ++e)
                if (std::memcmp(&prev[e], &tl.events[e], sizeof(rv_event)) != 0) {
                    std::printf("    event %u changed below unchanged = %u at %zu\n", e, tl.unchanged, to);
                    ++bad;
                    break;
                }
            prev.assign(tl.events, tl.events + tl.event_count);
        }
        if (!check[k]) continue;
        const std::string a = session_state(g.s);
        const std::string b = fresh_state(fresh, bytes, to, pacing, select);
        if (a != b) {
            ++bad;
            std::size_t d = 0;
            while (d < a.size() && d < b.size() && a[d] == b[d]) ++d;
            auto line = [&](const std::string &x) {
                const std::size_t lo = x.rfind('\n', d == 0 ? 0 : d - 1);
                const std::size_t start = lo == std::string::npos ? 0 : lo + 1;
                const std::size_t hi = x.find('\n', d);
                return x.substr(start, (hi == std::string::npos ? x.size() : hi) - start);
            };
            std::printf("    differs at %zu bytes (cut %zu), char %zu:\n    grown: %s\n    fresh: %s\n", to, k, d,
                        line(a).c_str(), line(b).c_str());
            if (bad > 2) return bad;
        }
    }
    return bad;
}

} // namespace

TEST(live_equivalence_timeline_sample_at_every_split) {
    const Bytes b = synth::build_timeline_sample();
    const std::vector<std::size_t> off = chunk_offsets(b);
    // Cuts: nothing, shorter than a header, the header, then every chunk
    // boundary and the middle of every chunk header and payload.
    std::vector<std::size_t> cuts = {0, 10, 24};
    for (std::size_t k = 0; k + 1 < off.size(); ++k) {
        cuts.push_back(off[k] + 7);
        cuts.push_back(off[k] + 16 + (off[k + 1] - off[k] - 16) / 2);
        cuts.push_back(off[k + 1]);
    }
    std::vector<bool> check(cuts.size(), true);
    CHECK_EQ(feed_and_compare(b, cuts, check, nullptr, -1, true), 0);
}

TEST(live_equivalence_one_byte_at_a_time) {
    const Bytes b = synth::build_timeline_sample();
    const std::vector<std::size_t> off = chunk_offsets(b);
    std::vector<std::size_t> cuts;
    std::vector<bool> check;
    for (std::size_t n = 0; n <= b.size(); ++n) {
        cuts.push_back(n);
        // Compare at chunk boundaries and at a few bytes inside chunks.
        check.push_back(std::binary_search(off.begin(), off.end(), n) || n % 997 == 0);
    }
    CHECK_EQ(feed_and_compare(b, cuts, check, nullptr, 1, true), 0);
}

TEST(live_equivalence_random_runs_and_pacings) {
    const rv_pacing real = {RV_PACING_REAL_TIME, 0, 180.0, 10.0};
    const rv_pacing nets = {RV_PACING_NETS_ONLY, 0, 60.0, 10.0};
    const rv_pacing one = {RV_PACING_ONE_NET, 2, 180.0, 10.0};
    const rv_pacing *pacings[] = {nullptr, &real, &nets, &one};
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        const Bytes b = synth::random_timeline(seed);
        std::mt19937 rng(seed);
        std::vector<std::size_t> cuts;
        std::size_t at = 0;
        while (at < b.size()) {
            at = std::min(b.size(), at + 1 + rng() % 700);
            cuts.push_back(at);
        }
        std::vector<bool> check(cuts.size(), false);
        for (std::size_t k = 0; k < check.size(); k += 3) check[k] = true;
        check.back() = true;
        const int bad = feed_and_compare(b, cuts, check, pacings[seed % 4], static_cast<int32_t>(seed % 3), true);
        if (bad != 0) std::printf("    seed %u\n", seed);
        CHECK_EQ(bad, 0);
    }
}

TEST(live_equivalence_board_trace) {
    // Boards (BRD) arriving in pieces: the board list and display list.
    const Bytes b = synth::build_stress_trace();
    const std::vector<std::size_t> off = chunk_offsets(b);
    std::vector<std::size_t> cuts;
    for (std::size_t k = 0; k < off.size(); ++k) {
        if (k > 0) cuts.push_back(off[k - 1] + (off[k] - off[k - 1]) / 3);
        cuts.push_back(off[k]);
    }
    std::vector<bool> check(cuts.size(), true);
    CHECK_EQ(feed_and_compare(b, cuts, check, nullptr, -1, false), 0);
}

namespace {
Bytes many_searches(std::uint32_t n, bool end); // below
} // namespace

TEST(live_constants_match_the_header) {
    CHECK_EQ(rv::Session::kFollowLag, RV_FOLLOW_LAG);
    CHECK_EQ(rv::Session::kCatchUp, RV_FOLLOW_CATCH_UP);
    CHECK_EQ(rv::Session::kFollowSettled, RV_FOLLOW_SETTLED);
    CHECK_EQ(rv::Session::kStallSeconds, RV_LIVE_STALL_SECONDS);
    CHECK_EQ(rv::FitLengthPacing::kLiveWeight, RV_LIVE_FIT_WEIGHT);
}

TEST(live_refresh_keeps_the_current_event) {
    rvtest::TempDir dir;
    const std::string path = dir.path("keep.grtrace");
    const Bytes b = synth::random_timeline(5);
    const std::vector<std::size_t> off = chunk_offsets(b);
    const std::size_t half = off[off.size() / 2];
    CHECK(synth::write_file(path, Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(half))));
    Session g;
    CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_LIVE), RV_OK);
    rv_live l;
    CHECK_EQ(rv_live_get(g.s, &l), RV_OK);
    CHECK_EQ(l.following, 1u);
    CHECK_EQ(l.live, 1u);
    // Pause in the middle of an event, then let the file grow in steps.
    uint32_t changed = 0;
    CHECK_EQ(rv_set_following(g.s, 0, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_LIVE) != 0);
    rv_timeline tl;
    rv_get_timeline(g.s, &tl);
    uint32_t e = tl.event_count / 2;
    while (e < tl.event_count && tl.events[e].duration <= 0) ++e;
    CHECK(e < tl.event_count);
    rv_seek_time(g.s, tl.events[e].start + 0.25 * tl.events[e].duration, nullptr);
    rv_position before;
    rv_position_get(g.s, &before);
    std::size_t have = half;
    for (std::size_t k = off.size() / 2 + 1; k < off.size(); ++k) {
        rvtest::append_file(path, b.data() + have, off[k] - have);
        have = off[k];
        CHECK_EQ(rv_poll(g.s, static_cast<double>(k), nullptr, nullptr), RV_OK);
        rv_position p;
        rv_position_get(g.s, &p);
        CHECK_EQ(p.event, before.event);
        CHECK_EQ(p.playing, 0u);
        CHECK(std::fabs(p.fraction - before.fraction) < 1e-9);
        // While it grows (prefix pacing), the time itself does not move.
        rv_live_get(g.s, &l);
        if (l.state != RV_LIVE_COMPLETE) CHECK_EQ(p.time, before.time);
    }
    // Paused inside the last event, the one still being written: its
    // start stays, and so does the time, exactly (not recomputed from the
    // fraction, which could move it by a rounding).
    {
        rvtest::TempDir dir3;
        const std::string p3 = dir3.path("last.grtrace");
        const Bytes m = many_searches(40, false);
        const std::vector<std::size_t> mo = chunk_offsets(m);
        std::size_t at = mo[mo.size() / 3];
        CHECK(synth::write_file(p3, Bytes(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(at))));
        Session k;
        CHECK_EQ(rv_open_ex(k.s, p3.c_str(), RV_OPEN_LIVE), RV_OK);
        CHECK_EQ(rv_set_following(k.s, 0, nullptr), RV_OK);
        rv_timeline t3;
        rv_get_timeline(k.s, &t3);
        CHECK(t3.event_count > 1);
        const rv_event last = t3.events[t3.event_count - 1];
        rv_seek_time(k.s, last.start + 0.3 * last.duration, nullptr);
        rv_position b3;
        rv_position_get(k.s, &b3);
        uint32_t checked = 0;
        for (std::size_t j = mo.size() / 3 + 1; j < mo.size() - 2; ++j) {
            rvtest::append_file(p3, m.data() + at, mo[j] - at);
            at = mo[j];
            CHECK_EQ(rv_poll(k.s, static_cast<double>(j), nullptr, nullptr), RV_OK);
            rv_position q;
            rv_position_get(k.s, &q);
            CHECK_EQ(q.event, b3.event);
            CHECK_EQ(q.time, b3.time);
            ++checked;
        }
        CHECK(checked > 3);
    }
    // At the end of the run (paused), growth keeps the event too.
    rvtest::TempDir dir2;
    const std::string p2 = dir2.path("end.grtrace");
    CHECK(synth::write_file(p2, Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(half))));
    Session h;
    CHECK_EQ(rv_open_ex(h.s, p2.c_str(), RV_OPEN_LIVE), RV_OK);
    CHECK_EQ(rv_play(h.s, 0, nullptr), RV_OK);
    rv_position_get(h.s, &before);
    CHECK_EQ(before.at_end, 1u);
    rvtest::append_file(p2, b.data() + half, off[off.size() / 2 + 3] - half);
    CHECK_EQ(rv_poll(h.s, 1, nullptr, nullptr), RV_OK);
    rv_position p;
    rv_position_get(h.s, &p);
    CHECK_EQ(p.event, before.event);
    CHECK(p.event_count > before.event_count);
    CHECK_EQ(p.at_end, 0u);
}

namespace {
// A run of many searches (and a commit after every fifth), chunks 0.125
// router seconds apart, ending with END when `end`.
Bytes many_searches(std::uint32_t n, bool end) {
    synth::TimelineWriter tw;
    tw.wavefronts = true;
    tw.nets({{1, "/A"}, {2, "/B"}});
    tw.start(16, 12);
    for (std::uint32_t k = 0; k < n; ++k) {
        tw.srch(1 + static_cast<int32_t>(k % 2), {{0, 1, 1}}, {{0, 12, 9}}, {}, 200);
        if (k % 5 == 4) {
            tw.commit(1 + static_cast<int32_t>(k % 2), {{0, 1, static_cast<int32_t>(1 + k % 9)}, {0, 12, 9}});
        }
    }
    if (end) tw.end({});
    return tw.w.out;
}
} // namespace

TEST(live_follow_rule) {
    rvtest::TempDir dir;
    const std::string path = dir.path("follow.grtrace");
    const Bytes full = many_searches(400, true);
    const std::vector<std::size_t> off = chunk_offsets(full);
    std::size_t have = off[20];
    CHECK(synth::write_file(path, Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(have))));
    Session g;
    CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_LIVE), RV_OK);
    rv_live l;
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 1u);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_GROWING));
    rv_position p;
    rv_position_get(g.s, &p);
    CHECK_EQ(p.playing, 1u);
    CHECK_EQ(p.at_end, 1u);
    // Pinned at the end while growing: advancing keeps playing there.
    CHECK_EQ(rv_advance(g.s, 0.1, nullptr), RV_OK);
    rv_position_get(g.s, &p);
    CHECK_EQ(p.playing, 1u);
    CHECK_EQ(p.at_end, 1u);
    // New events pull it along, at the speed of the run (2 chunks, 0.25
    // router seconds, per poll of 0.25 s, 15 frames per poll): at most one
    // new search per advance, none skipped, each shown in its settled part.
    rv_timeline tl;
    uint32_t last_event = 0;
    std::vector<uint32_t> settled; // searches seen in their settled part
    auto frame = [&]() {
        rv_position_get(g.s, &p);
        last_event = p.event;
        rv_advance(g.s, 1.0 / 60, nullptr);
        rv_position_get(g.s, &p);
        uint32_t passed = 0;
        for (uint32_t e = last_event + 1; e <= p.event; ++e)
            if (tl.events[e].kind == RV_EV_SEARCH && tl.events[e].duration > 0) ++passed;
        CHECK(passed <= 1);
        if (p.settled == 1u && (settled.empty() || settled.back() != p.event)) settled.push_back(p.event);
    };
    for (std::size_t c = 22; c <= 60; c += 2) {
        rvtest::append_file(path, full.data() + have, off[c] - have);
        have = off[c];
        CHECK_EQ(rv_poll(g.s, static_cast<double>(c), nullptr, nullptr), RV_OK);
        rv_get_timeline(g.s, &tl);
        for (int k = 0; k < 15; ++k) {
            frame();
            CHECK_EQ(p.playing, 1u);
        }
    }
    for (int k = 0; k < 600 && !p.at_end; ++k) frame();
    CHECK_EQ(p.at_end, 1u);
    CHECK_EQ(p.playing, 1u);
    std::vector<uint32_t> searches;
    for (uint32_t e = 0; e < tl.event_count; ++e)
        if (tl.events[e].kind == RV_EV_SEARCH && tl.events[e].chunk >= 20) searches.push_back(e);
    CHECK(settled == searches); // every search after the start was shown settled
    if (settled != searches) {
        std::printf("    DEBUG settled %zu searches %zu:", settled.size(), searches.size());
        for (std::size_t k = 0; k < std::max(settled.size(), searches.size()) && k < 12; ++k)
            std::printf(" %d/%d", k < settled.size() ? static_cast<int>(settled[k]) : -1,
                        k < searches.size() ? static_cast<int>(searches[k]) : -1);
        std::printf("\n");
    }
    rv_live_get(g.s, &l);
    CHECK_EQ(l.skips, 0u);

    // Far behind (more than RV_FOLLOW_LAG router seconds): jump ahead.
    rvtest::append_file(path, full.data() + have, off[360] - have);
    have = off[360];
    CHECK_EQ(rv_poll(g.s, 2, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK(l.lag_seconds > RV_FOLLOW_LAG);
    rv_advance(g.s, 1.0 / 60, nullptr);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.skips, 1u);
    CHECK(l.lag_seconds <= RV_FOLLOW_LAG);

    // Seeking back stops following and pauses; growth leaves it in place.
    rv_get_timeline(g.s, &tl);
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_event(g.s, tl.event_count / 4, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_LIVE) != 0);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 0u);
    rv_position before;
    rv_position_get(g.s, &before);
    CHECK_EQ(before.playing, 0u);
    rvtest::append_file(path, full.data() + have, off[380] - have);
    have = off[380];
    CHECK_EQ(rv_poll(g.s, 3, nullptr, nullptr), RV_OK);
    rv_advance(g.s, 0.5, nullptr);
    rv_position_get(g.s, &p);
    CHECK_EQ(p.time, before.time);
    CHECK_EQ(p.event, before.event);
    // Stepping and going backward also stop it; jumping to the end resumes.
    CHECK_EQ(rv_jump(g.s, 1, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 1u);
    CHECK_EQ(rv_step(g.s, RV_STEP_SEARCH, -1, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 0u);
    CHECK_EQ(rv_set_following(g.s, 1, nullptr), RV_OK);
    CHECK_EQ(rv_set_direction(g.s, -1), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 0u);
    CHECK_EQ(rv_set_direction(g.s, 1), RV_OK);
    // Playing at the end follows.
    rv_jump(g.s, 1, nullptr);
    rv_play(g.s, 0, nullptr);
    CHECK_EQ(rv_play(g.s, 1, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 1u);
    // Playing forward from inside into the end follows too.
    rv_seek_fraction(g.s, 0.99, nullptr);
    rv_play(g.s, 1, nullptr);
    for (int k = 0; k < 200; ++k) rv_advance(g.s, 0.1, nullptr);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 1u);
    rv_position_get(g.s, &p);
    CHECK_EQ(p.playing, 1u);

    // The rest, with END: following catches up, ends there, pauses.
    rvtest::append_file(path, full.data() + have, full.size() - have);
    uint32_t added = 0;
    CHECK_EQ(rv_poll(g.s, 4, &added, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_TIMELINE) != 0);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_COMPLETE));
    for (int k = 0; k < 5000; ++k) {
        rv_advance(g.s, 1.0 / 60, nullptr);
        rv_live_get(g.s, &l);
        if (l.following == 0) break;
    }
    CHECK_EQ(l.following, 0u);
    rv_position_get(g.s, &p);
    CHECK_EQ(p.at_end, 1u);
    CHECK_EQ(p.playing, 0u);
    // Complete: the fit length again.
    rv_get_timeline(g.s, &tl);
    CHECK(std::fabs(tl.total - 180.0) < 1e-9);
    CHECK_EQ(tl.span, tl.total);
}

TEST(live_pacing_while_growing_and_refit) {
    rvtest::TempDir dir;
    const std::string path = dir.path("pace.grtrace");
    const Bytes full = many_searches(50, true);
    const std::vector<std::size_t> off = chunk_offsets(full);
    CHECK(synth::write_file(path, Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(off[30]))));
    Session g;
    CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_LIVE), RV_OK);
    rv_timeline tl;
    rv_get_timeline(g.s, &tl);
    // While growing: weight * fit_seconds / kLiveWeight (RV_LIVE_FIT_WEIGHT in rv.h).
    CHECK_EQ(rv::FitLengthPacing::kLiveWeight, RV_LIVE_FIT_WEIGHT);
    rv::FitLengthPacing fit;
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(off[30])));
    rv::Timeline ref(*t, fit, true);
    CHECK_EQ(ref.size(), tl.event_count);
    double sum = 0;
    for (const rv::Event &e : ref.events()) sum += fit.weight(e) * 180.0 / rv::FitLengthPacing::kLiveWeight;
    CHECK(std::fabs(tl.total - sum) < 1e-9);
    CHECK(tl.span >= tl.total);
    CHECK(tl.span > tl.total * 1.2);
    const double span0 = tl.span;
    CHECK_EQ(tl.unchanged, 0u); // the first call after an open
    rv_get_timeline(g.s, &tl);
    CHECK_EQ(tl.unchanged, tl.event_count);
    // Not live (rv_open): the fit length, as before M6.
    Session h;
    CHECK_EQ(rv_open(h.s, path.c_str()), RV_OK);
    rv_timeline th;
    rv_get_timeline(h.s, &th);
    CHECK(std::fabs(th.total - 180.0) < 1e-9);
    CHECK_EQ(th.span, th.total);
    rv_live l;
    rv_live_get(h.s, &l);
    CHECK_EQ(l.following, 0u);
    CHECK_EQ(l.live, 0u);
    // Growth: old events keep their times, the span stays until reached.
    std::vector<rv_event> before(tl.events, tl.events + tl.event_count);
    rvtest::append_file(path, full.data() + off[30], off[32] - off[30]);
    CHECK_EQ(rv_poll(g.s, 0, nullptr, nullptr), RV_OK);
    rv_get_timeline(g.s, &tl);
    CHECK(tl.event_count > before.size());
    for (std::size_t e = 0; e + 1 < before.size(); ++e) {
        CHECK_EQ(tl.events[e].start, before[e].start);
        CHECK_EQ(tl.events[e].duration, before[e].duration);
    }
    CHECK(tl.unchanged + 1 >= before.size());
    if (tl.total <= span0) CHECK_EQ(tl.span, span0);
    // END: refit to the fit length, keeping the event.
    rv_set_following(g.s, 0, nullptr);
    rv_seek_event(g.s, 7, nullptr);
    rv_position p0;
    rv_position_get(g.s, &p0);
    rvtest::append_file(path, full.data() + off[32], full.size() - off[32]);
    uint32_t changed = 0;
    CHECK_EQ(rv_poll(g.s, 1, nullptr, &changed), RV_OK);
    rv_get_timeline(g.s, &tl);
    CHECK(std::fabs(tl.total - 180.0) < 1e-9);
    CHECK_EQ(tl.unchanged, 0u);
    rv_position p;
    rv_position_get(g.s, &p);
    CHECK_EQ(p.event, p0.event);
    CHECK(std::fabs(p.fraction - p0.fraction) < 1e-9);
    CHECK((changed & RV_CHANGED_POSITION) != 0);
}

TEST(live_states_texts_and_stall) {
    rvtest::TempDir dir;
    const std::string path = dir.path("later.grtrace");
    Session g;
    // A file that does not exist yet: waiting (with RV_OPEN_WAIT only).
    CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_LIVE), RV_E_IO);
    CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_WAIT | RV_OPEN_LIVE), RV_OK);
    rv_live l;
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_WAITING));
    const char *text = nullptr;
    CHECK_EQ(rv_live_text(g.s, &text), RV_OK);
    CHECK_STREQ(text, "Waiting for the file");
    CHECK_EQ(rv_poll(g.s, 0, nullptr, nullptr), RV_OK);
    // The recorder creates it and writes the header only.
    const Bytes b = synth::build_timeline_sample();
    CHECK(synth::write_file(path, Bytes(b.begin(), b.begin() + 24)));
    CHECK_EQ(rv_poll(g.s, 1, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_GROWING));
    // Chunks arrive.
    const std::vector<std::size_t> off = chunk_offsets(b);
    rvtest::append_file(path, b.data() + 24, off[10] - 24);
    uint32_t added = 0;
    CHECK_EQ(rv_poll(g.s, 2, &added, nullptr), RV_OK);
    CHECK_EQ(added, 10u);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_GROWING));
    CHECK_EQ(l.idle_seconds, 0.0);
    CHECK_EQ(l.chunks, 10u);
    CHECK_EQ(l.following, 1u);
    rv_live_text(g.s, &text);
    CHECK(std::strncmp(text, "Live: growing, ", 15) == 0);
    // Nothing for a while: stalled, with the idle time (no timeout).
    CHECK_EQ(rv_poll(g.s, 4, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_GROWING));
    CHECK_EQ(rv_poll(g.s, 9.5, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_STALLED));
    CHECK_EQ(l.idle_seconds, 7.5);
    rv_live_text(g.s, &text);
    CHECK(std::strncmp(text, "Live: stalled, no new data for 7 s", 34) == 0);
    CHECK_EQ(rv_poll(g.s, 1000, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_STALLED));
    // Growth again; then the rest with END: complete.
    rvtest::append_file(path, b.data() + off[10], 5); // part of a chunk: grows, nothing indexed
    CHECK_EQ(rv_poll(g.s, 1001, &added, nullptr), RV_OK);
    CHECK_EQ(added, 0u);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_GROWING));
    rvtest::append_file(path, b.data() + off[10] + 5, b.size() - off[10] - 5);
    CHECK_EQ(rv_poll(g.s, 1002, nullptr, nullptr), RV_OK);
    rv_live_get(g.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_COMPLETE));
    rv_live_text(g.s, &text);
    CHECK(std::strncmp(text, "Complete: ", 10) == 0);
    // A file that appears with something that is not a trace: failed.
    const std::string junk = dir.path("junk.grtrace");
    Session j;
    CHECK_EQ(rv_open_ex(j.s, junk.c_str(), RV_OPEN_WAIT | RV_OPEN_LIVE), RV_OK);
    CHECK(synth::write_file(junk, Bytes{'n', 'o', 't', ' ', 'a', ' ', 't', 'r', 'a', 'c', 'e'}));
    CHECK_EQ(rv_poll(j.s, 0, nullptr, nullptr), RV_E_FORMAT);
    rv_live_get(j.s, &l);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_FAILED));
    // No trace: nothing.
    Session n;
    CHECK_EQ(rv_live_get(n.s, &l), RV_OK);
    CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_NONE));
    CHECK_EQ(rv_live_text(n.s, &text), RV_OK);
    CHECK_STREQ(text, "");
    CHECK_EQ(rv_reload(n.s), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_open_ex(n.s, path.c_str(), 4u), RV_E_INVALID_ARGUMENT);
}

TEST(live_replacement_is_detected_and_reloads) {
    rvtest::TempDir dir;
    const std::string path = dir.path("run.grtrace");
    const Bytes a = synth::build_timeline_sample();
    const Bytes b = synth::random_timeline(8);
    const std::vector<std::size_t> off = chunk_offsets(a);
    auto open_half = [&](Session &g) {
        CHECK(synth::write_file(path, Bytes(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(off[off.size() / 2]))));
        CHECK_EQ(rv_open_ex(g.s, path.c_str(), RV_OPEN_LIVE | RV_OPEN_WAIT), RV_OK);
    };
    auto check_replaced = [&](Session &g, uint32_t chunks) {
        CHECK_EQ(rv_poll(g.s, 1, nullptr, nullptr), RV_E_REPLACED);
        CHECK_EQ(rv_poll(g.s, 2, nullptr, nullptr), RV_E_REPLACED);
        rv_live l;
        rv_live_get(g.s, &l);
        CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_REPLACED));
        CHECK_EQ(l.chunks, chunks); // nothing of the other run was mixed in
        const char *text = nullptr;
        rv_live_text(g.s, &text);
        CHECK(std::strncmp(text, "Replaced", 8) == 0);
    };
    // 1. Truncated and rewritten in place (shorter).
    {
        Session g;
        open_half(g);
        CHECK(synth::write_file(path, Bytes(b.begin(), b.begin() + 100)));
        check_replaced(g, static_cast<uint32_t>(off.size() / 2));
    }
    // 2. Rewritten in place, longer, with another header (open time).
    {
        Session g;
        open_half(g);
        Bytes other = a;
        other[16] ^= 0x55;
        CHECK(synth::write_file(path, other));
        check_replaced(g, static_cast<uint32_t>(off.size() / 2));
    }
    // 3. The same header but another last chunk: a new run started within
    //    the same second, rewritten in place.
    {
        Session g;
        open_half(g);
        Bytes other(a.begin(), a.begin() + 24);
        other.insert(other.end(), b.begin() + 24, b.end());
        CHECK(synth::write_file(path, other));
        check_replaced(g, static_cast<uint32_t>(off.size() / 2));
    }
    // 4. Another file renamed over the path (identical bytes: only the
    //    file's identity tells).
    {
        Session g;
        open_half(g);
        const std::string tmp = dir.path("next.grtrace");
        CHECK(synth::write_file(tmp, a));
        CHECK_EQ(std::rename(tmp.c_str(), path.c_str()), 0);
        check_replaced(g, static_cast<uint32_t>(off.size() / 2));
        // Reload: the new file, from scratch; camera and visibility kept.
        rv_view_set_size(g.s, 800, 500, 2);
        rv_view_zoom(g.s, 3.0, 100, 100);
        rv_view v0;
        rv_view_get(g.s, &v0);
        rv_class_set_visible(g.s, RV_OBJ_ROUTED, 0);
        CHECK_EQ(rv_reload(g.s), RV_OK);
        rv_live l;
        rv_live_get(g.s, &l);
        CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_COMPLETE));
        CHECK_EQ(l.chunks, static_cast<uint32_t>(off.size() - 1));
        rv_view v1;
        rv_view_get(g.s, &v1);
        CHECK_EQ(v1.scale, v0.scale);
        const char *name = nullptr;
        uint32_t vis = 1;
        rv_class(g.s, RV_OBJ_ROUTED, &name, &vis);
        CHECK_EQ(vis, 0u);
    }
    // 5. Deleted: replaced (a new run will start); reload waits for it.
    {
        Session g;
        open_half(g);
        CHECK_EQ(std::remove(path.c_str()), 0);
        check_replaced(g, static_cast<uint32_t>(off.size() / 2));
        CHECK_EQ(rv_reload(g.s), RV_OK);
        rv_live l;
        rv_live_get(g.s, &l);
        CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_WAITING));
        CHECK(synth::write_file(path, a));
        CHECK_EQ(rv_poll(g.s, 3, nullptr, nullptr), RV_OK);
        rv_live_get(g.s, &l);
        CHECK_EQ(l.state, static_cast<uint32_t>(RV_LIVE_COMPLETE));
    }
    // 6. Appending is not a replacement.
    {
        Session g;
        open_half(g);
        rvtest::append_file(path, a.data() + off[off.size() / 2], a.size() - off[off.size() / 2]);
        CHECK_EQ(rv_poll(g.s, 1, nullptr, nullptr), RV_OK);
    }
}

TEST(live_interval_index_grows_like_a_build) {
    std::mt19937 rng(11);
    for (int round = 0; round < 40; ++round) {
        rv::IntervalIndex inc;
        std::vector<std::uint32_t> births, deaths;
        std::uint32_t b = 0;
        const int n = 1 + static_cast<int>(rng() % 300);
        for (int k = 0; k < n; ++k) {
            const int op = static_cast<int>(rng() % 10);
            if (op < 6 || births.empty()) {
                b += rng() % 3;
                const std::uint32_t d = rng() % 4 == 0 ? 0xFFFFFFFFu : b + rng() % 20;
                births.push_back(b);
                deaths.push_back(d);
                inc.push(b, d);
            } else if (op < 9) {
                const std::size_t i = rng() % births.size();
                deaths[i] = births[i] + rng() % 30;
                inc.set_death(i, deaths[i]);
            } else {
                const std::size_t keep = rng() % births.size();
                births.resize(keep);
                deaths.resize(keep);
                inc.truncate(keep);
                b = births.empty() ? 0 : births.back();
            }
        }
        rv::IntervalIndex built;
        built.build(births, deaths);
        CHECK_EQ(inc.size(), births.size());
        for (std::uint32_t e = 0; e < b + 40; ++e) {
            std::vector<std::uint32_t> x, y;
            inc.query(e, x);
            built.query(e, y);
            CHECK(x == y);
        }
    }
}

TEST(live_dim_rectangle_key_names_its_rectangle) {
    // M5 carry-over: the dim rectangle's key named no geometry, so a client
    // could keep a plane drawn for another rectangle (it changes with the
    // board drawn and the session's grid while its colour stays).
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline tl(*t, rv::FitLengthPacing());
    const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
    rv::Theme theme;
    rv::Overlay ov;
    rv::OverlayInput in;
    in.trace = t.get();
    in.timeline = &tl;
    in.geometry = &geo;
    in.theme = &theme;
    in.event = tl.size() / 2;
    in.select_active = true;
    in.select_has_net = true;
    in.select_net = 1;
    auto dim = [&](float x1, std::uint32_t &key, std::uint32_t &gen) {
        in.dim_rect = {{0.0f, 0.0f, x1, 40.0f}};
        ov.build(in);
        key = 0;
        for (const rv::Batch &b : ov.list().batches)
            if (b.order == rv::order_key(rv::kRankDim, 0)) key = b.key;
        for (const rv::Slot &s : ov.slots())
            if (s.kind == rv::SlotKind::Dim) gen = s.generation;
    };
    std::uint32_t k1 = 0, k2 = 0, k3 = 0, g1 = 0, g2 = 0, g3 = 0;
    dim(50.0f, k1, g1);
    dim(80.0f, k2, g2);
    dim(50.0f, k3, g3);
    CHECK(k1 != 0);
    CHECK(k1 != k2);
    CHECK(g1 != g2);
    CHECK_EQ(k1, k3); // the same rectangle, the same key
}

TEST(live_open_flags_without_live_keep_m5_behaviour) {
    // rv_open of a growing trace: refresh refits, the end stays the end, no
    // following.
    rvtest::TempDir dir;
    const std::string path = dir.path("m5.grtrace");
    const Bytes b = synth::random_timeline(9);
    const std::vector<std::size_t> off = chunk_offsets(b);
    CHECK(synth::write_file(path, Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(off[off.size() / 2]))));
    Session g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    rv_position p;
    rv_position_get(g.s, &p);
    CHECK_EQ(p.playing, 0u);
    CHECK_EQ(p.at_end, 1u);
    rvtest::append_file(path, b.data() + off[off.size() / 2], off[off.size() / 2 + 4] - off[off.size() / 2]);
    CHECK_EQ(rv_refresh(g.s, nullptr), RV_OK);
    rv_position_get(g.s, &p);
    CHECK_EQ(p.at_end, 1u);
    CHECK(std::fabs(p.total - 180.0) < 1e-9);
    CHECK_EQ(rv_set_following(g.s, 1, nullptr), RV_OK);
    rv_live l;
    rv_live_get(g.s, &l);
    CHECK_EQ(l.following, 0u);
}

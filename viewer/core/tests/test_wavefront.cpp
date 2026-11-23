// test_wavefront.cpp: M4 core: the wavefront images and the pacing modes.

#include "check.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "routed.hpp"
#include "rv.h"
#include "synth.hpp"
#include "timeline.hpp"
#include "trace.hpp"
#include "trace_util.hpp"
#include "wavefront.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
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

std::uint32_t find_event(const rv::Timeline &tl, rv::EvKind kind, std::uint32_t nth = 0) {
    for (std::uint32_t e = 0; e < tl.size(); ++e)
        if (tl.events()[e].kind == kind && nth-- == 0) return e;
    return rv::kNone32;
}

bool open_session(rvtest::TempDir &dir, const std::string &name, const std::vector<unsigned char> &bytes,
                  SessionGuard &s) {
    const std::string path = dir.path(name);
    return synth::write_file(path, bytes) && rv_open(s.s, path.c_str()) == RV_OK;
}

const rv::Rgba kColor[2] = {0xC83434FFu, 0x4D7FC4FFu};

// The request the session would make for event e at fraction f.
rv::WaveRequest request(rv::Trace &tr, const rv::Timeline &tl, std::uint32_t e, double f, bool vis0 = true,
                        bool vis1 = true) {
    rv::WaveRequest r;
    const rv::Event &ev = tl.events()[e];
    r.session = ev.session;
    r.source = rv::wavefront_source(tr, tl, e);
    if (r.source != rv::kNone32) {
        r.search = ev.record;
        r.count = rv::revealed_count(tr.searches()[r.source].ntrace, f);
    }
    r.visible[0] = vis0;
    r.visible[1] = vis1;
    r.color[0] = kColor[0];
    r.color[1] = kColor[1];
    r.order[0] = 100;
    r.order[1] = 200;
    return r;
}

// Pixels of an image, transparent if never drawn.
std::vector<std::uint8_t> pixels_of(const rv::WaveImage &im) {
    if (!im.pixels.empty()) return im.pixels;
    return std::vector<std::uint8_t>(static_cast<std::size_t>(im.width) * im.height * 4, 0);
}

bool inside(const std::array<std::int32_t, 4> &b, std::int32_t x, std::int32_t y) {
    return x >= b[0] && x < b[2] && y >= b[1] && y < b[3];
}

// Every non-zero pixel inside bounds; the count of drawn cells right.
int check_bounds(const rv::WaveImage &im) {
    int bad = 0;
    std::uint32_t cells = 0;
    for (std::uint32_t y = 0; y < im.height && !im.pixels.empty(); ++y)
        for (std::uint32_t x = 0; x < im.width; ++x) {
            const std::uint8_t *p = im.pixels.data() + 4 * (static_cast<std::size_t>(y) * im.width + x);
            if (p[0] | p[1] | p[2] | p[3]) {
                ++cells;
                if (!inside(im.bounds, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y))) ++bad;
            }
        }
    if (cells != im.cells) ++bad;
    return bad;
}

std::uint32_t premul(double r, double g, double b, double a) {
    auto ch = [&](double v) { return static_cast<std::uint8_t>(std::lround(v * a)); };
    const std::uint8_t px[4] = {ch(r), ch(g), ch(b), static_cast<std::uint8_t>(std::lround(a * 255))};
    std::uint32_t v;
    std::memcpy(&v, px, 4);
    return v;
}

std::uint32_t pixel(const rv::WaveImage &im, std::uint32_t idx) {
    std::uint32_t v = 0;
    if (!im.pixels.empty()) std::memcpy(&v, im.pixels.data() + 4 * idx, 4);
    return v;
}

// A run built for the wavefront tests: a failed search, its repeat, a found
// search, a commit and an END; nets 1 and 2.
std::vector<unsigned char> wave_run(double gap = 0.125) {
    synth::TimelineWriter tw;
    tw.wavefronts = true;
    tw.nets({{1, "GND"}, {2, "/SIG"}});
    tw.mark(0, "route");
    tw.start(30, 20);
    tw.snap();
    const std::uint32_t r = tw.rnb(2, "/SIG", {{0.5, 0.5, 1, 4, "U1.1"}, {6.0, 4.0, 1, 4, "J1.1"}});
    tw.w.t += gap;
    const std::uint32_t failed = tw.srch(2, {C(0, 2, 2)}, {C(0, 24, 16)}, {}, 5000, 1, r);
    tw.w.t += gap;
    tw.srch_repeat(failed, 1, r);
    std::vector<Cell> path;
    for (std::int32_t i = 2; i <= 24; ++i) path.push_back(C(0, i, 2));
    for (std::int32_t j = 3; j <= 16; ++j) path.push_back(C(0, 24, j));
    tw.w.t += 3 * gap;
    tw.srch(2, {C(0, 2, 2)}, {C(0, 24, 16), C(1, 24, 16)}, path, 1000, 1, r);
    tw.commit(2, path, 0.2, {}, {}, 1, r);
    tw.rne(r, 2, {});
    tw.via(1, 10, 10);
    tw.end({});
    return tw.w.out;
}

} // namespace

// --- the rules ----------------------------------------------------------------------------

TEST(wavefront_reveal_and_frontier_rules) {
    CHECK_EQ(rv::revealed_count(100, 0.0), 0u);
    CHECK_EQ(rv::revealed_count(100, -1.0), 0u);
    CHECK_EQ(rv::revealed_count(100, 0.4), 50u);
    CHECK_EQ(rv::revealed_count(100, 0.79), 98u);
    CHECK_EQ(rv::revealed_count(100, 0.8), 100u);
    CHECK_EQ(rv::revealed_count(100, 1.0), 100u);
    CHECK_EQ(rv::revealed_count(0, 0.5), 0u);
    CHECK_EQ(rv::revealed_count(4294967295u, 0.4), 2147483647u);
    CHECK_EQ(rv::revealed_count(4294967295u, 0.7999999999), 4294967294u);
    CHECK_EQ(rv::revealed_count(10, std::numeric_limits<double>::quiet_NaN()), 0u);
    CHECK_EQ(rv::frontier_cells(0), 1u);
    CHECK_EQ(rv::frontier_cells(1), 1u);
    CHECK_EQ(rv::frontier_cells(50), 1u); // M5: 2 % (M4: 4 %)
    CHECK_EQ(rv::frontier_cells(51), 2u);
    CHECK_EQ(rv::frontier_cells(148027), 2961u);
}

TEST(wavefront_sample_has_wavefronts_and_the_same_timeline) {
    // The M3 sample now stores a wavefront for every search, read without
    // errors (its two warnings are the RIP of 7.14 it was built to have).
    // The weights use `expanded`, so the timeline is the one it had without.
    const std::vector<unsigned char> bytes = synth::build_timeline_sample();
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(bytes);
    std::uint32_t errors = 0;
    for (const rv::Issue &is : t->issues())
        if (is.severity == rv::Issue::Error || is.message.find("RIP") == std::string::npos) ++errors;
    CHECK_EQ(errors, 0u);
    std::uint32_t traced = 0;
    for (const rv::Search &s : t->searches()) {
        if (s.ntrace > 0) ++traced;
        CHECK(static_cast<std::int64_t>(s.ntrace) <= s.expanded);
    }
    CHECK_EQ(traced, static_cast<std::uint32_t>(t->searches().size()));
    CHECK_EQ(t->ends().size(), std::size_t(1));
    rv::Timeline tl(*t, rv::FitLengthPacing());
    CHECK_EQ(tl.total(), 180.0);
    for (std::uint32_t e = 0; e < tl.size(); ++e) CHECK(tl.events()[e].duration > 0);
}

// --- the images -------------------------------------------------------------------------------

TEST(wavefront_incremental_equals_full_rebuild) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline tl(*t, rv::FitLengthPacing());
    rv::WavefrontImages inc, full;
    full.always_rebuild = true;
    std::vector<std::uint32_t> searches;
    for (std::uint32_t e = 0; e < tl.size(); ++e)
        if (tl.events()[e].kind == rv::EvKind::Search) searches.push_back(e);
    CHECK(searches.size() >= 8);
    std::uint32_t positions = 0, bad = 0, dirty_bad = 0;
    std::vector<std::uint8_t> before[2];
    std::uint32_t gen_before[2] = {0, 0};
    auto visit = [&](std::uint32_t e, double f, bool v0, bool v1) {
        const rv::WaveRequest r = request(*t, tl, e, f, v0, v1);
        for (int k = 0; k < 2; ++k) {
            before[k] = pixels_of(inc.image(k));
            gen_before[k] = inc.image(k).generation;
        }
        inc.update(*t, r);
        full.update(*t, r);
        ++positions;
        for (int k = 0; k < 2; ++k) {
            const rv::WaveImage &a = inc.image(k), &b = full.image(k);
            if (pixels_of(a) != pixels_of(b) || a.cells != b.cells) ++bad;
            bad += static_cast<std::uint32_t>(check_bounds(a) + check_bounds(b));
            // The dirty rect holds every pixel that changed, relative to the
            // generation drawn before.
            const std::vector<std::uint8_t> now = pixels_of(a);
            if (a.generation == gen_before[k]) {
                if (now != before[k]) ++dirty_bad;
            } else {
                if (a.dirty_from != gen_before[k]) ++dirty_bad;
                if (before[k].size() == now.size())
                    for (std::uint32_t y = 0; y < a.height; ++y)
                        for (std::uint32_t x = 0; x < a.width; ++x) {
                            const std::size_t o = 4 * (static_cast<std::size_t>(y) * a.width + x);
                            if (std::memcmp(&now[o], &before[k][o], 4) != 0 &&
                                !inside(a.dirty, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)))
                                ++dirty_bad;
                        }
            }
        }
    };
    // Forward through every search, fine steps; then backward.
    for (std::uint32_t e : searches)
        for (int q = 0; q <= 50; ++q) visit(e, q / 50.0, true, true);
    for (auto it = searches.rbegin(); it != searches.rend(); ++it)
        for (int q = 50; q >= 0; --q) visit(*it, q / 50.0, true, true);
    // Random jumps, within and across searches and to other events, with
    // the layers shown and hidden.
    std::mt19937 rng(4);
    for (int k = 0; k < 1500; ++k) {
        const std::uint32_t e = rng() % 4 == 0 ? static_cast<std::uint32_t>(rng() % tl.size())
                                               : searches[rng() % searches.size()];
        const double f = std::uniform_real_distribution<double>(-0.1, 1.1)(rng);
        const bool v0 = rng() % 8 != 0, v1 = rng() % 8 != 0;
        visit(e, f, v0, v1);
        if (rng() % 3 == 0) visit(e, std::min(1.0, f + 0.01), v0, v1); // a small step forward
        if (rng() % 3 == 0) visit(e, std::max(0.0, f - 0.01), v0, v1); // and back
    }
    std::printf("    %u positions compared, %llu incremental and %llu full updates\n", positions,
                static_cast<unsigned long long>(inc.incremental), static_cast<unsigned long long>(inc.rebuilds));
    CHECK_EQ(bad, 0u);
    CHECK_EQ(dirty_bad, 0u);
    CHECK(inc.incremental > 500);
    CHECK_EQ(full.incremental, 0u);
}

TEST(wavefront_colours_frontier_and_hidden_layers) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline tl(*t, rv::FitLengthPacing());
    const std::uint32_t e = find_event(tl, rv::EvKind::Search, 0); // /SIG: F.Cu to a via to B.Cu
    const rv::Search &s = t->searches()[tl.events()[e].record];
    std::shared_ptr<const rv::Wavefront> w = t->wavefront(tl.events()[e].record);
    CHECK(w != nullptr);
    if (!w) return;
    const std::uint32_t n = s.ntrace;
    CHECK(n > 100);
    const std::uint32_t nc = static_cast<std::uint32_t>(t->runs()[0].nc());
    rv::WavefrontImages wi;
    wi.update(*t, request(*t, tl, e, 0.4));
    const std::uint32_t shown = n / 2;
    const std::uint32_t F = rv::frontier_cells(n);
    CHECK_EQ(wi.count(), 2);
    CHECK_EQ(wi.search(), tl.events()[e].record);
    CHECK_EQ(wi.image(0).width, 40u);
    CHECK_EQ(wi.image(0).height, 24u);
    CHECK_EQ(wi.image(0).cells + wi.image(1).cells, shown);
    // The source (cost 0): the near colour; the last revealed: the frontier.
    const double r0 = 200, g0 = 52, b0 = 52;
    auto white = [](double v, double f) { return v + (255 - v) * f; };
    const std::uint32_t near = premul(white(r0, 0.18), white(g0, 0.18), white(b0, 0.18), 0.22);
    const std::uint32_t front = premul(white(r0, 0.50), white(g0, 0.50), white(b0, 0.50), 0.55);
    CHECK_EQ(w->costs[0], 0);
    CHECK_EQ(pixel(wi.image(0), w->cells[0] % nc), near);
    const std::uint32_t last = w->cells[shown - 1];
    CHECK_EQ(pixel(wi.image(last / nc), last % nc), last / nc == 0 ? front : pixel(wi.image(1), last % nc));
    // Frontier cells on F.Cu all have the frontier colour; older ones do not.
    std::uint32_t frontier_seen = 0;
    for (std::uint32_t k = 0; k < shown; ++k) {
        const std::uint32_t c = w->cells[k];
        if (c / nc != 0) continue;
        const bool is_front = k + F >= shown;
        if (is_front) ++frontier_seen;
        CHECK_EQ(pixel(wi.image(0), c % nc) == front, is_front);
    }
    CHECK(frontier_seen > 0);
    // An entry not yet revealed is transparent.
    CHECK_EQ(pixel(wi.image(w->cells[n - 1] / nc), w->cells[n - 1] % nc), 0u);
    // The largest cost, settled and outside the frontier: the far colour.
    wi.update(*t, request(*t, tl, e, 1.0));
    std::int32_t maxc = 0;
    for (std::int32_t c : w->costs) maxc = std::max(maxc, c);
    for (std::uint32_t k = 0; k + F < n; ++k)
        if (w->costs[k] == maxc && w->cells[k] / nc == 0)
            CHECK_EQ(pixel(wi.image(0), w->cells[k] % nc), premul(r0, g0, b0, 0.08));
    CHECK_EQ(wi.image(0).cells + wi.image(1).cells, n);
    // Hiding router layer 1 empties its image only.
    const std::uint32_t gen1 = wi.image(1).generation, gen0 = wi.image(0).generation;
    wi.update(*t, request(*t, tl, e, 1.0, true, false));
    CHECK_EQ(wi.image(1).cells, 0u);
    CHECK(wi.image(1).generation != gen1);
    CHECK(wi.image(0).cells > 0);
    CHECK_EQ(wi.image(0).generation, gen0 + 1); // repainted (another style), same pixels
    // Another event: nothing shown, the arrays released.
    wi.update(*t, request(*t, tl, e + 1, 0.5));
    CHECK_EQ(wi.image(0).cells + wi.image(1).cells, 0u);
    CHECK_EQ(wi.search(), rv::kNone32);
    // reset(): no images.
    wi.reset();
    CHECK_EQ(wi.count(), 0);
    CHECK_EQ(wi.image(0).width, 0u);
}

TEST(wavefront_through_the_api_play_equals_seek) {
    rvtest::TempDir dir;
    const std::vector<unsigned char> bytes = synth::build_timeline_sample();
    SessionGuard a, b;
    CHECK(open_session(dir, "run.grtrace", bytes, a));
    CHECK(open_session(dir, "run.grtrace", bytes, b));
    uint32_t changed = 0, count = 0;
    CHECK_EQ(rv_wavefront_image_count(a.s, &count), RV_OK);
    CHECK_EQ(count, 2u);
    rv_wavefront_image im;
    CHECK_EQ(rv_get_wavefront_image(a.s, 2, &im), RV_E_INVALID_ARGUMENT);
    // At the end (session 1, no search): empty images of the last session.
    CHECK_EQ(rv_get_wavefront_image(a.s, 0, &im), RV_OK);
    CHECK_EQ(im.width, 40u);
    CHECK_EQ(im.cells, 0u);
    CHECK_EQ(im.search, RV_NONE);
    CHECK_EQ(im.stride, 160u);
    CHECK(std::fabs(im.rect[0] - (100.5 - 0.125)) < 1e-9);
    CHECK(std::fabs(im.rect[2] - (100.5 + 39.5 * 0.25)) < 1e-9);
    // The image's order is that of a wavefront slot.
    rv_display_list ov;
    const rv_slot *slots = nullptr;
    uint32_t ns = 0;
    CHECK_EQ(rv_get_overlay(a.s, &ov, &slots, &ns), RV_OK);
    bool slot_found = false;
    for (uint32_t k = 0; k < ns; ++k)
        if (slots[k].kind == RV_SLOT_WAVEFRONT && slots[k].order_begin == im.order) slot_found = true;
    CHECK(slot_found);
    // Play forward in small steps; at each step seek b to the same time.
    CHECK_EQ(rv_play(a.s, 1, &changed), RV_OK); // from the start
    std::uint32_t compared = 0, differ = 0, wave_bits = 0, with_cells = 0;
    rv_position p;
    CHECK_EQ(rv_position_get(a.s, &p), RV_OK);
    CHECK_EQ(p.at_end, 0u);
    auto compare = [&]() {
        CHECK_EQ(rv_position_get(a.s, &p), RV_OK);
        CHECK_EQ(rv_seek_time(b.s, p.time, &changed), RV_OK);
        uint32_t na = 0, nb = 0;
        CHECK_EQ(rv_wavefront_image_count(a.s, &na), RV_OK);
        CHECK_EQ(rv_wavefront_image_count(b.s, &nb), RV_OK);
        if (na != nb) ++differ;
        for (uint32_t k = 0; k < na; ++k) {
            rv_wavefront_image x, y;
            CHECK_EQ(rv_get_wavefront_image(a.s, k, &x), RV_OK);
            CHECK_EQ(rv_get_wavefront_image(b.s, k, &y), RV_OK);
            ++compared;
            if (x.cells != y.cells || x.search != y.search) ++differ;
            const std::size_t bytes_n = static_cast<std::size_t>(x.stride) * x.height;
            const std::vector<uint8_t> zero(bytes_n, 0);
            const uint8_t *px = x.pixels ? x.pixels : zero.data();
            const uint8_t *py = y.pixels ? y.pixels : zero.data();
            if (x.width != y.width || std::memcmp(px, py, bytes_n) != 0) ++differ;
            if (k == 0 && p.cells_revealed > 0) ++with_cells;
        }
    };
    for (int step = 0; step < 4000 && !p.at_end; ++step) {
        CHECK_EQ(rv_advance(a.s, 0.05, &changed), RV_OK);
        if (changed & RV_CHANGED_WAVEFRONT) ++wave_bits;
        compare();
    }
    // And backward.
    CHECK_EQ(rv_seek_fraction(a.s, 0.6, &changed), RV_OK);
    CHECK_EQ(rv_set_direction(a.s, -1), RV_OK);
    CHECK_EQ(rv_play(a.s, 1, &changed), RV_OK);
    for (int step = 0; step < 600; ++step) {
        CHECK_EQ(rv_advance(a.s, 0.07, &changed), RV_OK);
        compare();
    }
    std::printf("    %u images compared (%u with cells), %u frames with RV_CHANGED_WAVEFRONT\n", compared, with_cells,
                wave_bits);
    CHECK_EQ(differ, 0u);
    CHECK(with_cells > 100);
    CHECK(wave_bits > 50);
}

TEST(wavefront_ends_with_the_event_and_follows_visibility) {
    rvtest::TempDir dir;
    SessionGuard s;
    CHECK(open_session(dir, "run.grtrace", synth::build_timeline_sample(), s));
    rv_timeline tl;
    CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
    uint32_t se = RV_NONE;
    for (uint32_t k = 0; k < tl.event_count && se == RV_NONE; ++k)
        if (tl.events[k].kind == RV_EV_SEARCH) se = k;
    CHECK(se != RV_NONE);
    if (se == RV_NONE) return;
    const rv_event ev = tl.events[se];
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_time(s.s, ev.start + 0.5 * ev.duration, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_WAVEFRONT) != 0);
    rv_position p;
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK(p.cells_revealed > 0);
    CHECK_EQ(p.settled, 0u);
    CHECK_EQ(p.has_net, 1u);
    CHECK_EQ(p.net, 2);
    rv_wavefront_image a, b;
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &a), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &b), RV_OK);
    CHECK_EQ(a.cells + b.cells, p.cells_revealed);
    CHECK_EQ(a.search, ev.record);
    // The class hidden: both images empty; shown again: the same pixels.
    std::vector<uint8_t> keep(a.pixels, a.pixels + static_cast<std::size_t>(a.stride) * a.height);
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_WAVEFRONT, 0), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &a), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &b), RV_OK);
    CHECK_EQ(a.cells + b.cells, 0u);
    CHECK_EQ(a.search, RV_NONE);
    CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
    CHECK(p.cells_revealed > 0); // counted whether or not shown
    CHECK_EQ(rv_class_set_visible(s.s, RV_OBJ_WAVEFRONT, 1), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &a), RV_OK);
    CHECK(a.pixels != nullptr && std::memcmp(a.pixels, keep.data(), keep.size()) == 0);
    // F.Cu hidden: image 0 empty, image 1 kept.
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &b), RV_OK);
    const uint32_t b_cells = b.cells;
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 0), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &a), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &b), RV_OK);
    CHECK_EQ(a.cells, 0u);
    CHECK_EQ(b.cells, b_cells);
    CHECK_EQ(rv_layer_set_visible(s.s, "F.Cu", 1), RV_OK);
    // The next event: the wavefront is gone.
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, 1, &changed), RV_OK);
    CHECK((changed & RV_CHANGED_WAVEFRONT) != 0);
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &a), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &b), RV_OK);
    CHECK_EQ(a.cells + b.cells, 0u);
    CHECK_EQ(a.search, RV_NONE);
    CHECK_EQ(a.bounds[0] >= a.bounds[2] || a.bounds[1] >= a.bounds[3], true);
    // Moving between two non-search events does not report a wavefront change.
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, 1, &changed), RV_OK);
    CHECK_EQ(changed & RV_CHANGED_WAVEFRONT, 0u);
}

TEST(wavefront_repeat_failed_and_settled) {
    const std::vector<unsigned char> bytes = wave_run();
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(bytes);
    CHECK(t->issues().empty());
    rv::Timeline tl(*t, rv::FitLengthPacing());
    const std::uint32_t ef = find_event(tl, rv::EvKind::Search, 0); // failed
    const std::uint32_t er = find_event(tl, rv::EvKind::Search, 1); // its repeat
    const std::uint32_t eo = find_event(tl, rv::EvKind::Search, 2); // found
    CHECK(eo != rv::kNone32);
    if (eo == rv::kNone32) return;
    const rv::Search &sf = t->searches()[tl.events()[ef].record];
    const rv::Search &sr = t->searches()[tl.events()[er].record];
    CHECK_EQ(sr.repeat_of, tl.events()[ef].record);
    CHECK_EQ(sr.ntrace, 0u);
    CHECK_EQ(rv::wavefront_source(*t, tl, er), tl.events()[ef].record);
    CHECK(sf.ntrace >= 1000u); // the whole grid except the target
    // The failed search (5000 states) plays as long as the found one with
    // 1000: its weight is capped at 1000 states.
    CHECK(std::fabs(tl.events()[ef].duration - tl.events()[eo].duration) < 1e-12);
    CHECK(std::fabs(tl.events()[er].duration - tl.events()[eo].duration) < 1e-12);
    rv::FitLengthPacing fit;
    CHECK_EQ(fit.weight(tl.events()[ef]), 1.0 + std::log1p(1000.0));
    // The repeat shows exactly the wavefront of the search it repeats.
    rv::WavefrontImages a, b;
    for (double f : {0.1, 0.5, 0.79, 0.9}) {
        a.update(*t, request(*t, tl, ef, f));
        b.update(*t, request(*t, tl, er, f));
        for (int k = 0; k < 2; ++k) {
            CHECK(pixels_of(a.image(k)) == pixels_of(b.image(k)));
            CHECK_EQ(a.image(k).cells, b.image(k).cells);
        }
        CHECK_EQ(b.search(), tl.events()[er].record);
    }
    // Settled: the path once a search found one; the targets in red once it
    // did not.  Before: neither.
    rv::Theme theme;
    const std::vector<rv::CopperGeom> geo = rv::build_copper_geometry(*t);
    auto overlay_at = [&](std::uint32_t e, bool settled) {
        rv::Overlay ov;
        rv::OverlayInput in;
        in.trace = t.get();
        in.timeline = &tl;
        in.geometry = &geo;
        in.theme = &theme;
        in.event = e;
        in.time = tl.events()[e].start;
        in.settled = settled;
        ov.build(in);
        return ov.list().batches;
    };
    auto path_strokes = [&](const std::vector<rv::Batch> &bs) {
        std::uint32_t n = 0;
        for (const rv::Batch &b2 : bs)
            if (b2.order == rv::order_key(rv::kRankOverlay, rv::sub::kMarkCells) && b2.prim == rv::Prim::Stroke) n += b2.count;
        return n;
    };
    auto target_colour = [&](const std::vector<rv::Batch> &bs) {
        for (const rv::Batch &b2 : bs)
            if (b2.order == rv::order_key(rv::kRankOverlay, rv::sub::kMarkCells + 2)) return b2.color;
        return rv::Rgba(0);
    };
    CHECK_EQ(path_strokes(overlay_at(eo, false)), 0u);
    CHECK_EQ(path_strokes(overlay_at(eo, true)), 1u);
    CHECK_EQ(target_colour(overlay_at(eo, true)), target_colour(overlay_at(eo, false)));
    CHECK_EQ(path_strokes(overlay_at(ef, true)), 0u);
    CHECK(target_colour(overlay_at(ef, true)) != target_colour(overlay_at(ef, false)));
    CHECK_EQ(target_colour(overlay_at(ef, true)), rv::OverlayColors().failure);
    CHECK(rv::search_settled(tl, eo, tl.events()[eo].start + 0.8 * tl.events()[eo].duration));
    CHECK(!rv::search_settled(tl, eo, tl.events()[eo].start + 0.79 * tl.events()[eo].duration));
    CHECK(!rv::search_settled(tl, eo + 1, tl.events()[eo + 1].start + 0.9 * tl.events()[eo + 1].duration));
}

TEST(wavefront_unreadable_arrays_are_an_error_and_empty) {
    // A search whose stored arrays are cut short: rv_open reads the file
    // lazily; the image call reports the error and gives empty images.
    rvtest::TempDir dir;
    std::vector<unsigned char> bytes = wave_run();
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(bytes);
    rv::Timeline tl(*t, rv::FitLengthPacing());
    const std::uint32_t ef = find_event(tl, rv::EvKind::Search, 0);
    const std::size_t off = t->chunks()[tl.events()[ef].chunk].offset;
    // Overwrite the first trace cell (after the 32-byte chunk header... and
    // the fixed part, the src, tgt) with a cell outside the grid; the
    // reader checks cells lazily, when the wavefront is loaded.
    const std::size_t cell0 = off + 16 + 96 + 4 * (1 + 1 + 0);
    bytes[cell0 + 3] = 0x7F;
    SessionGuard s;
    const std::string path = dir.path("bad.grtrace");
    CHECK(synth::write_file(path, bytes));
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    uint32_t changed = 0;
    CHECK_EQ(rv_seek_time(s.s, tl.events()[ef].start + 0.5 * tl.events()[ef].duration, &changed), RV_OK);
    rv_wavefront_image im;
    const rv_status st = rv_get_wavefront_image(s.s, 0, &im);
    std::printf("    status %d: %s\n", static_cast<int>(st), rv_last_error(s.s));
    CHECK_EQ(st, RV_E_FORMAT);
    CHECK_EQ(im.cells, 0u);
    CHECK_EQ(im.search, RV_NONE);
    // The next call does not fail again until the position changes, and
    // other searches still draw.
    rv_wavefront_image im2;
    CHECK_EQ(rv_get_wavefront_image(s.s, 1, &im2), RV_OK);
    const std::uint32_t eo = find_event(tl, rv::EvKind::Search, 2);
    CHECK_EQ(rv_seek_time(s.s, tl.events()[eo].start + 0.5 * tl.events()[eo].duration, &changed), RV_OK);
    CHECK_EQ(rv_get_wavefront_image(s.s, 0, &im), RV_OK);
    CHECK(im.cells > 0);
}

// --- pacing -----------------------------------------------------------------------------------

TEST(pacing_mode_names_and_text) {
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_FIT), "fit length");
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_NETS_ONLY), "nets only");
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_ONE_NET), "one net");
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_SKIP_FAILED), "skip failed searches");
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_REAL_TIME), "real time");
    CHECK_STREQ(rv_pacing_mode_name(RV_PACING_MODE_COUNT), "");
    CHECK_STREQ(rv_pacing_mode_name(0xFFFFFFFFu), "");
    rv::PacingSettings p;
    CHECK_EQ(rv::pacing_text(p, nullptr, 180), std::string("fit length 3:00"));
    p.mode = rv::PacingMode::OneNet;
    p.net = 4;
    CHECK_EQ(rv::pacing_text(p, nullptr, 41.9), std::string("one net: net 4, 0:41"));
    p.mode = rv::PacingMode::RealTime;
    p.slowdown = 10;
    CHECK_EQ(rv::pacing_text(p, nullptr, 287), std::string("real time \xC3\x97") + "10, 4:47");
    p.mode = rv::PacingMode::NetsOnly;
    CHECK_EQ(rv::pacing_text(p, nullptr, 59.99), std::string("nets only, 0:59"));
    p.mode = rv::PacingMode::SkipFailed;
    CHECK_EQ(rv::pacing_text(p, nullptr, 3600), std::string("skip failed searches, 60:00"));
}

TEST(pacing_fit_length_and_nets_only) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline fit(*t, rv::FitLengthPacing());
    rv::Timeline f60(*t, rv::FitLengthPacing(60));
    CHECK_EQ(f60.total(), 60.0);
    for (std::uint32_t e = 0; e < fit.size(); ++e)
        CHECK(std::fabs(f60.events()[e].duration * 3 - fit.events()[e].duration) < 1e-9);
    rv::Timeline nets(*t, rv::FitLengthPacing(180, rv::PacingMode::NetsOnly));
    CHECK_EQ(nets.total(), 180.0);
    CHECK_EQ(nets.size(), fit.size());
    std::uint32_t zero = 0;
    for (std::uint32_t e = 0; e < nets.size(); ++e) {
        const rv::Event &ev = nets.events()[e];
        if (ev.kind == rv::EvKind::Search) {
            CHECK_EQ(ev.duration, 0.0);
            ++zero;
            // Takes effect with the next event: never current.
            CHECK(nets.event_at(ev.start) != e);
        } else {
            CHECK(ev.duration > 0);
        }
        if (e > 0) CHECK(ev.start >= nets.events()[e - 1].start);
    }
    CHECK(zero >= 8);
    // No time inside the run shows a search.
    for (int k = 0; k <= 2000; ++k) {
        const std::uint32_t e = nets.event_at(180.0 * k / 2000);
        CHECK(nets.events()[e].kind != rv::EvKind::Search);
    }
    // Ticks and phases follow the pacing.
    CHECK_EQ(nets.rip_ticks().size(), fit.rip_ticks().size());
    for (std::size_t k = 0; k < nets.phases().size(); ++k)
        CHECK_EQ(nets.phases()[k].start, nets.events()[nets.phases()[k].event].start);
    CHECK(nets.rip_ticks()[0] != fit.rip_ticks()[0]);
    // The same copper at the end.
    std::vector<std::uint32_t> a, b;
    fit.alive_copper(fit.size() - 1, a);
    nets.alive_copper(nets.size() - 1, b);
    CHECK(a == b);
}

TEST(pacing_one_net_and_skip_failed) {
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(synth::build_timeline_sample());
    rv::Timeline one(*t, rv::FitLengthPacing(180, rv::PacingMode::OneNet, 4)); // /CLK
    CHECK_EQ(one.total(), 180.0);
    std::uint32_t timed = 0;
    for (const rv::Event &ev : one.events()) {
        const bool mine = (ev.flags & rv::evf::kHasNet) != 0 && ev.net == 4;
        CHECK_EQ(ev.duration > 0, mine);
        if (mine) ++timed;
    }
    CHECK(timed >= 4); // RNB, two searches, commit, RNE...
    rv::Timeline none(*t, rv::FitLengthPacing(180, rv::PacingMode::OneNet, 99));
    CHECK_EQ(none.total(), 0.0);
    CHECK_EQ(none.event_at(0), none.size() - 1);
    rv::Timeline skip(*t, rv::FitLengthPacing(180, rv::PacingMode::SkipFailed));
    std::uint32_t failed = 0;
    for (const rv::Event &ev : skip.events()) {
        if (ev.kind != rv::EvKind::Search) {
            CHECK(ev.duration > 0);
        } else if ((ev.flags & rv::evf::kFound) == 0) {
            CHECK_EQ(ev.duration, 0.0);
            ++failed;
        } else {
            CHECK(ev.duration > 0);
        }
    }
    CHECK_EQ(failed, 4u);
}

TEST(pacing_real_time) {
    const std::vector<unsigned char> bytes = wave_run(0.5);
    std::unique_ptr<rv::Trace> t = rvtest::open_bytes(bytes);
    rv::Timeline rt(*t, rv::RealTimePacing(10));
    const std::vector<rv::Chunk> &ch = t->chunks();
    double sum = 0;
    for (std::uint32_t e = 0; e < rt.size(); ++e) {
        const rv::Event &ev = rt.events()[e];
        double want = e == 0 ? 0 : ch[ev.last].t - ch[rt.events()[e - 1].last].t;
        if (ev.kind == rv::EvKind::Search && (ev.flags & rv::evf::kFound) == 0) want = std::min(want, 0.010);
        CHECK(std::fabs(ev.duration - 10 * want) < 1e-9);
        sum += ev.duration;
    }
    CHECK(std::fabs(rt.total() - sum) < 1e-9);
    const std::uint32_t ef = find_event(rt, rv::EvKind::Search, 0);
    const std::uint32_t eo = find_event(rt, rv::EvKind::Search, 2);
    CHECK(std::fabs(rt.events()[ef].duration - 0.1) < 1e-9);  // 0.625 s of router time, capped at 0.010
    CHECK(std::fabs(rt.events()[eo].duration - 16.25) < 1e-9); // 1.625 s x 10
    CHECK_EQ(rt.events()[0].duration, 0.0);
    // The last event with time ends the run.
    CHECK(std::fabs(rt.events().back().start + rt.events().back().duration - rt.total()) < 1e-9);
}

TEST(pacing_through_the_api) {
    rvtest::TempDir dir;
    const std::vector<unsigned char> bytes = synth::build_timeline_sample();
    SessionGuard s;
    CHECK(open_session(dir, "run.grtrace", bytes, s));
    rv_pacing p;
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    CHECK_EQ(p.mode, static_cast<uint32_t>(RV_PACING_FIT));
    CHECK_EQ(p.fit_seconds, 180.0);
    CHECK_EQ(p.slowdown, 10.0);
    rv_timeline tl;
    CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
    const uint32_t gen0 = tl.generation;
    // Setting the same pacing changes nothing.
    uint32_t changed = 9;
    CHECK_EQ(rv_set_pacing(s.s, &p, &changed), RV_OK);
    CHECK_EQ(changed, 0u);
    CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
    CHECK_EQ(tl.generation, gen0);
    // A commit at fraction 0.3: the mode changes, the event and fraction stay.
    uint32_t commit = RV_NONE, search = RV_NONE;
    for (uint32_t k = 0; k < tl.event_count; ++k) {
        if (commit == RV_NONE && tl.events[k].kind == RV_EV_COMMIT && k > 5) commit = k;
        if (search == RV_NONE && tl.events[k].kind == RV_EV_SEARCH && k > 5) search = k;
    }
    CHECK(commit != RV_NONE && search != RV_NONE);
    CHECK_EQ(rv_seek_time(s.s, tl.events[commit].start + 0.3 * tl.events[commit].duration, &changed), RV_OK);
    const rv_pacing modes[] = {
        {RV_PACING_NETS_ONLY, 0, 180, 10}, {RV_PACING_ONE_NET, 2, 180, 10}, {RV_PACING_SKIP_FAILED, 0, 180, 10},
        {RV_PACING_REAL_TIME, 0, 180, 3},  {RV_PACING_FIT, 0, 60, 10},
    };
    uint32_t gen = gen0;
    for (const rv_pacing &m : modes) {
        CHECK_EQ(rv_set_pacing(s.s, &m, &changed), RV_OK);
        CHECK((changed & RV_CHANGED_POSITION) != 0);
        CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
        CHECK(tl.generation != gen);
        gen = tl.generation;
        rv_position pos;
        CHECK_EQ(rv_position_get(s.s, &pos), RV_OK);
        CHECK_EQ(pos.event, commit);
        CHECK(std::fabs(pos.fraction - 0.3) < 1e-9);
        CHECK_EQ(pos.total, tl.total);
        rv_pacing back;
        CHECK_EQ(rv_get_pacing(s.s, &back), RV_OK);
        CHECK_EQ(back.mode, m.mode);
        CHECK_EQ(back.net, m.net);
        CHECK_EQ(back.fit_seconds, m.fit_seconds);
        CHECK_EQ(back.slowdown, m.slowdown);
    }
    CHECK_EQ(tl.total, 60.0);
    const char *text = nullptr;
    CHECK_EQ(rv_text(s.s, RV_TEXT_PACING, &text), RV_OK);
    CHECK_STREQ(text, "fit length 1:00");
    // A search, then nets only: the search gets no time; the position shows
    // the next event with time, from its start.
    CHECK_EQ(rv_seek_time(s.s, tl.events[search].start + 0.5 * tl.events[search].duration, &changed), RV_OK);
    const rv_pacing nets = {RV_PACING_NETS_ONLY, 0, 60, 10};
    CHECK_EQ(rv_set_pacing(s.s, &nets, &changed), RV_OK);
    rv_position pos;
    CHECK_EQ(rv_position_get(s.s, &pos), RV_OK);
    CHECK_EQ(pos.event, search + 1);
    CHECK_EQ(pos.fraction, 0.0);
    CHECK_EQ(pos.cells_revealed, 0u);
    // Stepping by search finds none; by event never lands on a search.
    CHECK_EQ(rv_seek_time(s.s, 0, &changed), RV_OK);
    CHECK_EQ(rv_step(s.s, RV_STEP_SEARCH, 1, &changed), RV_OK);
    CHECK_EQ(changed, 0u);
    CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
    for (int k = 0; k < 200; ++k) {
        CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, 1, &changed), RV_OK);
        CHECK_EQ(rv_position_get(s.s, &pos), RV_OK);
        CHECK(tl.events[pos.event].kind != RV_EV_SEARCH);
        if (pos.at_end) break;
    }
    // In the "one net" text the net is named.
    const rv_pacing one = {RV_PACING_ONE_NET, 4, 180, 10};
    CHECK_EQ(rv_set_pacing(s.s, &one, &changed), RV_OK);
    CHECK_EQ(rv_text(s.s, RV_TEXT_PACING, &text), RV_OK);
    CHECK_STREQ(text, "one net: /CLK, 3:00");
    // A net with no events: the run lasts 0 s and shows its end.
    const rv_pacing nobody = {RV_PACING_ONE_NET, 99, 180, 10};
    CHECK_EQ(rv_set_pacing(s.s, &nobody, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &pos), RV_OK);
    CHECK_EQ(pos.total, 0.0);
    CHECK_EQ(pos.at_end, 1u);
    CHECK_EQ(pos.event, pos.event_count - 1);
    CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK);
    CHECK_EQ(rv_advance(s.s, 1.0, &changed), RV_OK);
    CHECK_EQ(rv_seek_fraction(s.s, 0.5, &changed), RV_OK);
    CHECK_EQ(rv_step(s.s, RV_STEP_EVENT, -1, &changed), RV_OK);
    CHECK_EQ(rv_position_get(s.s, &pos), RV_OK);
    CHECK_EQ(pos.event, pos.event_count - 1);
    CHECK_EQ(rv_text(s.s, RV_TEXT_POSITION, &text), RV_OK);
    // Invalid values change nothing.
    rv_pacing bad = {RV_PACING_FIT, 0, 0.5, 10};
    CHECK_EQ(rv_set_pacing(s.s, &bad, &changed), RV_E_INVALID_ARGUMENT);
    bad = {RV_PACING_FIT, 0, 180, 0};
    CHECK_EQ(rv_set_pacing(s.s, &bad, &changed), RV_E_INVALID_ARGUMENT);
    bad = {RV_PACING_FIT, 0, std::numeric_limits<double>::quiet_NaN(), 10};
    CHECK_EQ(rv_set_pacing(s.s, &bad, &changed), RV_E_INVALID_ARGUMENT);
    bad = {RV_PACING_FIT, 0, 180, std::numeric_limits<double>::infinity()};
    CHECK_EQ(rv_set_pacing(s.s, &bad, &changed), RV_E_INVALID_ARGUMENT);
    bad = {RV_PACING_MODE_COUNT, 0, 180, 10};
    CHECK_EQ(rv_set_pacing(s.s, &bad, &changed), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_set_pacing(s.s, nullptr, &changed), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_get_pacing(s.s, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    CHECK_EQ(p.net, 99);
    // The pacing is kept across rv_open.
    const std::string path = dir.path("run.grtrace");
    CHECK_EQ(rv_open(s.s, path.c_str()), RV_OK);
    CHECK_EQ(rv_get_pacing(s.s, &p), RV_OK);
    CHECK_EQ(p.net, 99);
    CHECK_EQ(rv_get_timeline(s.s, &tl), RV_OK);
    CHECK_EQ(tl.total, 0.0);
}

TEST(pacing_every_mode_plays_to_the_end) {
    rvtest::TempDir dir;
    SessionGuard s;
    CHECK(open_session(dir, "run.grtrace", synth::build_timeline_sample(), s));
    const rv_pacing modes[] = {
        {RV_PACING_FIT, 0, 180, 10},         {RV_PACING_NETS_ONLY, 0, 180, 10}, {RV_PACING_ONE_NET, 1, 180, 10},
        {RV_PACING_SKIP_FAILED, 0, 180, 10}, {RV_PACING_REAL_TIME, 0, 180, 10},
    };
    uint32_t copper_end = RV_NONE;
    for (const rv_pacing &m : modes) {
        uint32_t changed = 0;
        CHECK_EQ(rv_set_pacing(s.s, &m, &changed), RV_OK);
        CHECK_EQ(rv_play(s.s, 1, &changed), RV_OK); // at the end: starts over
        CHECK_EQ(rv_set_speed(s.s, 64), RV_OK);
        rv_position p;
        CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
        CHECK_EQ(p.time, 0.0);
        int frames = 0;
        uint32_t searches_seen = 0;
        while (!p.at_end && frames < 100000) {
            CHECK_EQ(rv_advance(s.s, 1.0 / 60, &changed), RV_OK);
            CHECK_EQ(rv_position_get(s.s, &p), RV_OK);
            rv_wavefront_image im;
            CHECK_EQ(rv_get_wavefront_image(s.s, 0, &im), RV_OK);
            if (im.search != RV_NONE) ++searches_seen;
            ++frames;
        }
        CHECK_EQ(p.at_end, 1u);
        CHECK_EQ(p.playing, 0u);
        if (m.mode == RV_PACING_NETS_ONLY) CHECK_EQ(searches_seen, 0u);
        if (copper_end == RV_NONE) copper_end = p.copper_alive;
        CHECK_EQ(p.copper_alive, copper_end);
    }
}

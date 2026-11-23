// timeline.cpp: events, pacing and the state at any event (timeline.hpp).

#include "timeline.hpp"

#include "routed.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace rv {

const char *event_kind_name(std::uint32_t kind) noexcept {
    static const char *const names[kEvKindCount] = {
        "session", "snapshot", "search", "commit", "via", "rip-up",
        "route_net begins", "route_net ends", "drop_to_plane", "phase", "board", "end",
    };
    return kind < kEvKindCount ? names[kind] : "";
}

// --- pacing -------------------------------------------------------------------

const char *pacing_mode_name(std::uint32_t mode) noexcept {
    static const char *const names[kPacingModeCount] = {"fit length", "nets only", "one net", "skip failed searches",
                                                        "real time"};
    return mode < kPacingModeCount ? names[mode] : "";
}

double FitLengthPacing::weight(const Event &e) const {
    const bool failed_search = e.kind == EvKind::Search && (e.flags & evf::kFound) == 0;
    switch (mode_) {
    case PacingMode::NetsOnly:
        if (e.kind == EvKind::Search) return 0.0;
        break;
    case PacingMode::SkipFailed:
        if (failed_search) return 0.0;
        break;
    case PacingMode::OneNet:
        if ((e.flags & evf::kHasNet) == 0 || e.net != net_) return 0.0;
        break;
    default: break;
    }
    switch (e.kind) {
    case EvKind::Session: return 1.0;
    case EvKind::Snapshot: return 0.5;
    case EvKind::Search: {
        double size = std::max(0.0, e.size);
        if (failed_search) size = std::min(size, kFailedSizeCap);
        return 1.0 + std::log1p(size);
    }
    case EvKind::Commit: return 2.0;
    case EvKind::Via: return 1.0;
    case EvKind::RipUp: return 3.0;
    case EvKind::RouteBegin: return 0.5;
    case EvKind::RouteEnd: return (e.flags & evf::kFailed) != 0 ? 2.0 : 0.5;
    case EvKind::Drop: return 1.0;
    case EvKind::Phase: return 4.0;
    case EvKind::Board: return 2.0;
    case EvKind::End: return 1.0;
    }
    return 1.0;
}

double FitLengthPacing::durations(const Trace &, const std::vector<Event> &events, std::vector<double> &out) const {
    out.assign(events.size(), 0.0);
    double sum = 0;
    for (std::size_t e = 0; e < events.size(); ++e) {
        const double w = weight(events[e]);
        out[e] = w > 0 && std::isfinite(w) ? w : 0.0;
        sum += out[e];
    }
    if (!(sum > 0)) {
        std::fill(out.begin(), out.end(), 0.0);
        return 0.0;
    }
    for (double &d : out) d = d * total_ / sum;
    return total_;
}

double RealTimePacing::durations(const Trace &t, const std::vector<Event> &events, std::vector<double> &out) const {
    out.assign(events.size(), 0.0);
    const std::vector<Chunk> &ch = t.chunks();
    double prev = 0, sum = 0;
    for (std::size_t e = 0; e < events.size(); ++e) {
        const double end = events[e].last < ch.size() ? ch[events[e].last].t : prev;
        double d = e == 0 ? 0.0 : std::max(0.0, end - prev);
        if (events[e].kind == EvKind::Search && (events[e].flags & evf::kFound) == 0)
            d = std::min(d, kFailedSecondsCap);
        out[e] = std::isfinite(d) ? d * slowdown_ : 0.0;
        sum += out[e];
        if (std::isfinite(end)) prev = std::max(prev, end);
    }
    return sum;
}

double FitLengthPacing::step(const Trace &, const std::vector<Event> &events, std::size_t e, double &) const {
    const double w = weight(events[e]);
    return w > 0 && std::isfinite(w) ? w * total_ / kLiveWeight : 0.0;
}

double RealTimePacing::step(const Trace &t, const std::vector<Event> &events, std::size_t e, double &prev) const {
    const std::vector<Chunk> &ch = t.chunks();
    const double end = events[e].last < ch.size() ? ch[events[e].last].t : prev;
    double d = e == 0 ? 0.0 : std::max(0.0, end - prev);
    if (events[e].kind == EvKind::Search && (events[e].flags & evf::kFound) == 0) d = std::min(d, kFailedSecondsCap);
    if (std::isfinite(end)) prev = std::max(prev, end);
    return std::isfinite(d) ? d * slowdown_ : 0.0;
}

std::unique_ptr<Pacing> make_pacing(const PacingSettings &s) {
    if (s.mode == PacingMode::RealTime) return std::unique_ptr<Pacing>(new RealTimePacing(s.slowdown));
    return std::unique_ptr<Pacing>(new FitLengthPacing(s.fit_seconds, s.mode, s.net));
}

// --- interval index ---------------------------------------------------------------

void IntervalIndex::clear() {
    births_.clear();
    tree_.clear();
    size_ = cap_ = 0;
}

void IntervalIndex::build(const std::vector<std::uint32_t> &births, const std::vector<std::uint32_t> &deaths) {
    clear();
    births_ = births;
    size_ = births.size();
    cap_ = 1;
    while (cap_ < size_) cap_ <<= 1;
    tree_.assign(2 * cap_, 0);
    for (std::size_t k = 0; k < size_; ++k) tree_[cap_ + k] = deaths[k];
    for (std::size_t k = cap_ - 1; k >= 1; --k) tree_[k] = std::max(tree_[2 * k], tree_[2 * k + 1]);
}

void IntervalIndex::set_leaf(std::size_t k, std::uint32_t v) {
    std::size_t i = cap_ + k;
    tree_[i] = v;
    for (i >>= 1; i >= 1; i >>= 1) {
        const std::uint32_t m = std::max(tree_[2 * i], tree_[2 * i + 1]);
        if (tree_[i] == m) break;
        tree_[i] = m;
    }
}

void IntervalIndex::push(std::uint32_t birth, std::uint32_t death) {
    if (size_ == cap_) {
        // Grow: twice the leaves, the tree built again (amortised O(1)).
        const std::size_t cap = cap_ == 0 ? 64 : 2 * cap_;
        std::vector<std::uint32_t> tree(2 * cap, 0);
        for (std::size_t k = 0; k < size_; ++k) tree[cap + k] = tree_[cap_ + k];
        for (std::size_t k = cap - 1; k >= 1; --k) tree[k] = std::max(tree[2 * k], tree[2 * k + 1]);
        tree_.swap(tree);
        cap_ = cap;
    }
    births_.push_back(birth);
    ++size_;
    set_leaf(size_ - 1, death);
}

void IntervalIndex::set_death(std::size_t k, std::uint32_t death) {
    if (k < size_) set_leaf(k, death);
}

void IntervalIndex::truncate(std::size_t n) {
    while (size_ > n) {
        --size_;
        set_leaf(size_, 0);
        births_.pop_back();
    }
}

void IntervalIndex::collect(std::size_t node, std::size_t lo, std::size_t hi, std::size_t n, std::uint32_t e,
                            std::vector<std::uint32_t> &out) const {
    if (lo >= n || tree_[node] <= e) return;
    if (hi - lo == 1) {
        out.push_back(static_cast<std::uint32_t>(lo));
        return;
    }
    const std::size_t mid = (lo + hi) / 2;
    collect(2 * node, lo, mid, n, e, out);
    collect(2 * node + 1, mid, hi, n, e, out);
}

void IntervalIndex::query(std::uint32_t e, std::vector<std::uint32_t> &out) const {
    if (size_ == 0) return;
    // Items born at or before e form a prefix.
    const std::size_t n = static_cast<std::size_t>(std::upper_bound(births_.begin(), births_.end(), e) - births_.begin());
    if (n == 0) return;
    collect(1, 0, cap_, n, e, out);
}

// --- timeline -----------------------------------------------------------------------

namespace {
constexpr std::uint32_t kForever = std::numeric_limits<std::uint32_t>::max();
}

Timeline::Timeline(const Trace &t, const Pacing &pacing, bool live) : t_(t) {
    extend(pacing, live);
}

Timeline::Change Timeline::extend(const Pacing &pacing, bool live) {
    const std::vector<Chunk> &ch = t_.chunks();
    const auto n = static_cast<std::uint32_t>(ch.size());
    changed_copper_.clear();
    changed_failures_.clear();
    const bool was_live = live_;
    live_ = live;
    // The last event of the previous build is recomputed (the chunks after
    // it may now make an event of their own or join it).
    const std::uint32_t e0 = p2_done_;

    // Pass 1: which chunks make events, and the chunk ranges.
    if (!events_.empty()) events_.back().last = last_real_;
    for (std::uint32_t k = scanned_; k < n; ++k) scan_chunk(k);
    scanned_ = n;
    if (!events_.empty()) {
        last_real_ = events_.back().last;
        if (pending_ < n) events_.back().last = n - 1; // trailing absorbed chunks extend the last event
    }
    const auto size = static_cast<std::uint32_t>(events_.size());
    chunk_event_.resize(n, kNone32);
    for (std::uint32_t e = e0; e < size; ++e)
        for (std::uint32_t k = events_[e].first; k <= events_[e].last && k < n; ++k) chunk_event_[k] = e;

    // Pass 2: the state after each event, from the checkpoint before e0.
    state_ = ckpt_;
    last_change_ = ckpt_last_change_;
    last_change_.resize(t_.runs().size(), kNone32);
    drop_search_.resize(t_.drops().size(), kNone32);
    counts_.resize(size);
    for (std::uint32_t e = e0; e < size; ++e) {
        if (e + 1 == size) {
            ckpt_ = state_;
            ckpt_last_change_ = last_change_;
        }
        apply_event(e);
    }
    p2_done_ = size == 0 ? 0 : size - 1;

    extend_lives(e0);
    extend_failures();
    // Times: from e0 on, or all of them when the trace stopped growing
    // (refitted) or the pacing is global.
    const std::uint32_t from = was_live == live ? e0 : 0;
    const bool global = pace(pacing, from);
    Change c;
    c.repaced = global || from < e0;
    c.first = c.repaced ? 0 : e0;
    return c;
}

void Timeline::scan_chunk(std::uint32_t k) {
    const Chunk &c = t_.chunks()[k];
    auto add = [&](EvKind kind, std::uint32_t record) -> Event & {
        Event ev;
        ev.kind = kind;
        ev.chunk = k;
        ev.first = pending_;
        ev.last = k;
        ev.record = record;
        events_.push_back(ev);
        pending_ = k + 1;
        return events_.back();
    };
    auto with_net = [](Event &ev, std::int32_t net) {
        ev.net = net;
        ev.flags |= evf::kHasNet;
    };
    if (c.kind == Kind::Run) {
        add(EvKind::Session, c.record); // a corrupt RUN still starts a session (6 rule 2)
        return;
    }
    if (c.corrupt || c.record == kNone32) return;
    switch (c.kind) {
    case Kind::Snap: add(EvKind::Snapshot, c.record); break;
    case Kind::Srch: {
        const Search &s = t_.searches()[c.record];
        if (!s.valid) break;
        Event &ev = add(EvKind::Search, c.record);
        with_net(ev, s.net);
        if (s.has(srch_flags::kFound)) ev.flags |= evf::kFound;
        else ev.flags |= evf::kFailed;
        if (s.has(srch_flags::kError)) ev.flags |= evf::kError;
        ev.size = s.expanded >= 0 ? static_cast<double>(s.expanded) : static_cast<double>(s.ntrace_total);
        max_size_ = std::max(max_size_, ev.size);
        break;
    }
    case Kind::Cmit: with_net(add(EvKind::Commit, c.record), t_.commits()[c.record].net); break;
    case Kind::Via: {
        const Via &v = t_.vias()[c.record];
        if (!v.added || v.copper == kNone32) break; // added = 0 changes nothing
        with_net(add(EvKind::Via, c.record), v.net);
        break;
    }
    case Kind::Rbld: add(EvKind::RipUp, c.record).flags |= evf::kRebuild; break;
    case Kind::Rip: {
        const Rip &r = t_.rips()[c.record];
        if (!events_.empty() && pending_ == k && events_.back().kind == EvKind::RipUp &&
            (events_.back().flags & evf::kRebuild) != 0 && r.ctx == 1) {
            events_.back().last = k; // part of the rebuild's rip-up
            pending_ = k + 1;
            break;
        }
        with_net(add(EvKind::RipUp, kNone32), r.net);
        break;
    }
    case Kind::Rnb: {
        const Route &r = t_.routes()[c.record];
        if (!r.valid) break;
        with_net(add(EvKind::RouteBegin, c.record), r.net);
        break;
    }
    case Kind::Rne: {
        const RouteEnd &re = t_.route_ends()[c.record];
        Event &ev = add(EvKind::RouteEnd, c.record);
        with_net(ev, re.net);
        if (!re.failed.empty()) ev.flags |= evf::kFailed;
        if (re.status == 1) ev.flags |= evf::kError;
        break;
    }
    case Kind::Drop: {
        const Drop &d = t_.drops()[c.record];
        Event &ev = add(EvKind::Drop, c.record);
        with_net(ev, d.net);
        if (d.ok == 0) ev.flags |= evf::kFailed;
        if (d.ok == 2) ev.flags |= evf::kError;
        break;
    }
    case Kind::Mark:
        if (t_.marks()[c.record].level == 0) {
            Phase p;
            p.event = static_cast<std::uint32_t>(events_.size());
            p.label = t_.marks()[c.record].text;
            phases_.push_back(p);
            add(EvKind::Phase, c.record);
        }
        break;
    case Kind::Brd: add(EvKind::Board, c.record); break;
    case Kind::End: add(EvKind::End, c.record); break;
    default: break; // absorbed
    }
}

void Timeline::apply_event(std::uint32_t e) {
    const std::vector<Chunk> &ch = t_.chunks();
    Event &ev = events_[e];
    State &st = state_;
    bool closes = false;
    for (std::uint32_t k = ev.first; k <= ev.last; ++k) {
        const Chunk &c = ch[k];
        if (c.kind == Kind::Run) {
            st.open_route = kNone32;
            st.drop_search = kNone32;
            continue;
        }
        if (c.corrupt || c.record == kNone32) continue;
        switch (c.kind) {
        case Kind::Srch:
            if (!t_.searches()[c.record].valid) break;
            ++st.cnt.searches;
            if (t_.searches()[c.record].ctx == 2) st.drop_search = c.record;
            break;
        case Kind::Cmit:
            ++st.cnt.commits;
            st.cnt.vias += static_cast<std::uint32_t>(t_.commits()[c.record].vias.size());
            if (c.session < last_change_.size()) last_change_[c.session] = k;
            break;
        case Kind::Via:
            if (t_.vias()[c.record].added) {
                ++st.cnt.vias;
                if (c.session < last_change_.size()) last_change_[c.session] = k;
            }
            break;
        case Kind::Rip: {
            const Rip &r = t_.rips()[c.record];
            ++st.cnt.rips;
            st.cnt.removed += static_cast<std::uint32_t>(r.applied.size());
            if (!r.applied.empty() && c.session < last_change_.size()) last_change_[c.session] = k;
            break;
        }
        case Kind::Rnb:
            if (!t_.routes()[c.record].valid) break;
            ++st.cnt.routes;
            st.open_route = c.record;
            break;
        case Kind::Rne: closes = true; break;
        case Kind::Drop:
            drop_search_[c.record] = st.drop_search;
            st.drop_search = kNone32;
            break;
        case Kind::Brd: st.board = c.record; break;
        default: break;
        }
    }
    if (ev.kind == EvKind::Phase) {
        for (std::uint32_t p = 0; p < phases_.size(); ++p)
            if (phases_[p].event == e) st.phase = p;
    }
    ev.session = ch[ev.last].session;
    ev.board = st.board;
    ev.phase = st.phase;
    ev.route = st.open_route;
    if (ev.kind == EvKind::RouteEnd) {
        const std::uint32_t ri = t_.route_index(t_.route_ends()[ev.record].route_id);
        if (ri != kNone32 && ri < t_.routes().size()) ev.route = ri;
    }
    if (ev.kind == EvKind::RipUp && ev.record != kNone32 && st.open_route != kNone32) {
        // An RBLD rip-up takes the net of the route_net call in progress.
        ev.net = t_.routes()[st.open_route].net;
        ev.flags |= evf::kHasNet;
    }
    if (closes) st.open_route = kNone32;
    counts_[e] = st.cnt;
}

void Timeline::repace(const Pacing &pacing) {
    pace(pacing, 0);
}

bool Timeline::pace(const Pacing &pacing, std::uint32_t e0) {
    const auto size = static_cast<std::uint32_t>(events_.size());
    const bool global = !live_ && !pacing.prefix_only();
    if (global) e0 = 0;
    if (e0 > size) e0 = size;
    // Ticks of events before e0 stay.
    while (!rip_tick_events_.empty() && rip_tick_events_.back() >= e0) {
        rip_tick_events_.pop_back();
        rip_ticks_.pop_back();
    }
    while (!failure_tick_events_.empty() && failure_tick_events_.back() >= e0) {
        failure_tick_events_.pop_back();
        failure_ticks_.pop_back();
    }
    if (global) {
        total_ = 0;
        if (size > 0) {
            std::vector<double> d;
            const double want = pacing.durations(t_, events_, d);
            d.resize(events_.size(), 0.0);
            double start = 0;
            std::size_t last_timed = events_.size();
            for (std::size_t e = 0; e < events_.size(); ++e) {
                events_[e].start = start;
                events_[e].duration = d[e] > 0 ? d[e] : 0.0;
                if (events_[e].duration > 0) last_timed = e;
                start += events_[e].duration;
            }
            total_ = last_timed < events_.size() && want > 0 && std::isfinite(want) ? want : start;
            if (last_timed < events_.size()) {
                // The last event with time ends exactly at total; the events after it
                // (without time) start there.
                Event &last = events_[last_timed];
                if (!(last.start < total_)) last.start = std::nextafter(total_, 0.0);
                last.duration = total_ - last.start;
                for (std::size_t e = last_timed + 1; e < events_.size(); ++e) events_[e].start = total_;
            }
        }
    } else {
        // Prefix pacing: each event's time from the events up to it.
        pace_state_.resize(size + 1, 0.0);
        double start = e0 > 0 ? events_[e0 - 1].start + events_[e0 - 1].duration : 0.0;
        double st = e0 > 0 ? pace_state_[e0] : 0.0;
        for (std::uint32_t e = e0; e < size; ++e) {
            pace_state_[e] = st;
            const double d = pacing.step(t_, events_, e, st);
            events_[e].start = start;
            events_[e].duration = d > 0 && std::isfinite(d) ? d : 0.0;
            start += events_[e].duration;
        }
        pace_state_[size] = st;
        total_ = start;
    }
    for (std::uint32_t e = e0; e < size; ++e) {
        const Event &ev = events_[e];
        if (ev.kind == EvKind::RipUp) {
            rip_ticks_.push_back(ev.start);
            rip_tick_events_.push_back(e);
        }
        if ((ev.kind == EvKind::RouteEnd || ev.kind == EvKind::Drop) && (ev.flags & evf::kFailed) != 0) {
            failure_ticks_.push_back(ev.start);
            failure_tick_events_.push_back(e);
        }
    }
    // Phases from the one before the first changed event (its end) on.
    std::size_t p0 = phases_.size();
    while (p0 > 0 && phases_[p0 - 1].event >= e0) --p0;
    if (p0 > 0) --p0;
    for (std::size_t p = p0; p < phases_.size(); ++p) phases_[p].start = size > 0 ? events_[phases_[p].event].start : 0;
    for (std::size_t p = p0; p < phases_.size(); ++p)
        phases_[p].end = p + 1 < phases_.size() ? phases_[p + 1].start : total_;
    // Ghosts whose rip-up is at or after e0 (sorted by it: a suffix).
    std::size_t g0 = ghost_order_.size();
    while (g0 > 0 && life_[ghost_order_[g0 - 1]].death >= e0) --g0;
    ghost_start_.resize(ghost_order_.size());
    for (std::size_t k = g0; k < ghost_order_.size(); ++k) ghost_start_[k] = events_[life_[ghost_order_[k]].death].start;
    return global;
}

std::uint32_t Timeline::event_at(double t) const {
    if (events_.empty()) return kNone32;
    if (t >= total_) return size() - 1;
    if (!(t > 0)) t = 0;
    auto it = std::upper_bound(events_.begin(), events_.end(), t,
                               [](double v, const Event &ev) { return v < ev.start; });
    const auto k = it - events_.begin();
    return k == 0 ? 0 : static_cast<std::uint32_t>(k) - 1;
}

std::uint32_t Timeline::event_of_chunk(std::uint32_t c) const {
    if (c < chunk_event_.size()) return chunk_event_[c];
    return events_.empty() ? kNone32 : size() - 1;
}

void Timeline::extend_lives(std::uint32_t e0) {
    const std::vector<Copper> &cu = t_.copper();
    const std::vector<Chunk> &ch = t_.chunks();
    const auto m = static_cast<std::uint32_t>(cu.size());
    // Copper from k0 on is computed again: its chunks are in events from e0 on.
    const std::uint32_t c0 = e0 < events_.size() ? events_[e0].first : static_cast<std::uint32_t>(ch.size());
    std::uint32_t k0 = static_cast<std::uint32_t>(life_.size());
    while (k0 > 0 && cu[k0 - 1].chunk >= c0) --k0;
    auto life_of = [&](std::uint32_t k, CopperLife &l) {
        const Copper &c = cu[k];
        l = CopperLife();
        const std::uint32_t b = c.valid ? event_of_chunk(c.chunk) : kNone32;
        if (b == kNone32) return;
        l.birth = b;
        if (c.removed_by != kNone32) {
            l.death = event_of_chunk(c.removed_by);
            l.ripped = true;
        } else if (c.session != kNone32 && c.session + 1 < t_.runs().size()) {
            l.death = event_of_chunk(t_.runs()[c.session + 1].chunk);
        }
    };
    std::vector<std::uint32_t> new_ghosts;
    // Copper born earlier that was alive: removed since, or its session ended?
    {
        std::size_t w = 0;
        for (std::uint32_t k : open_copper_) {
            if (k >= k0) continue; // computed again below
            CopperLife l;
            life_of(k, l);
            if (l.death != life_[k].death || l.ripped != life_[k].ripped) {
                life_[k] = l;
                copper_index_.set_death(k, l.death == kNone32 ? kForever : l.death);
                changed_copper_.push_back(k);
                if (l.ripped && l.death != kNone32) new_ghosts.push_back(k);
            }
            if (life_[k].death == kNone32) open_copper_[w++] = k;
        }
        open_copper_.resize(w);
    }
    // Copper from k0 on.
    life_.resize(m);
    copper_index_.truncate(k0);
    std::uint32_t prev = k0 > 0 ? copper_index_.birth(k0 - 1) : 0;
    for (std::uint32_t k = k0; k < m; ++k) {
        CopperLife &l = life_[k];
        life_of(k, l);
        changed_copper_.push_back(k);
        if (l.birth == kNone32) {
            copper_index_.push(prev, prev); // empty interval
            continue;
        }
        copper_index_.push(l.birth, l.death == kNone32 ? kForever : l.death);
        prev = l.birth;
        if (l.death == kNone32) open_copper_.push_back(k);
        else if (l.ripped) new_ghosts.push_back(k);
    }
    std::sort(changed_copper_.begin(), changed_copper_.end());
    // Ghosts: ripped copper by death event, then index.  Ghosts already
    // listed keep their place (a death never changes once set), except those
    // computed again.
    std::size_t g0 = ghost_order_.size();
    while (g0 > 0 && life_[ghost_order_[g0 - 1]].death >= e0) --g0;
    std::vector<std::uint32_t> tail;
    for (std::size_t k = g0; k < ghost_order_.size(); ++k)
        if (ghost_order_[k] < k0) tail.push_back(ghost_order_[k]);
    tail.insert(tail.end(), new_ghosts.begin(), new_ghosts.end());
    std::sort(tail.begin(), tail.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (life_[a].death != life_[b].death) return life_[a].death < life_[b].death;
        return a < b;
    });
    tail.erase(std::unique(tail.begin(), tail.end()), tail.end());
    ghost_order_.resize(g0);
    ghost_order_.insert(ghost_order_.end(), tail.begin(), tail.end());
    ghost_start_.resize(ghost_order_.size(), 0.0); // times set by pace()
}

void Timeline::alive_copper(std::uint32_t e, std::vector<std::uint32_t> &out) const {
    if (e >= events_.size()) return;
    copper_index_.query(e, out);
}

void Timeline::ghosts(double t, std::vector<std::pair<std::uint32_t, double>> &out) const {
    const auto lo = std::upper_bound(ghost_start_.begin(), ghost_start_.end(), t - kGhostSeconds) - ghost_start_.begin();
    const auto hi = std::upper_bound(ghost_start_.begin(), ghost_start_.end(), t) - ghost_start_.begin();
    const std::size_t first = out.size();
    for (auto k = lo; k < hi; ++k) {
        const double a = 1.0 - (t - ghost_start_[static_cast<std::size_t>(k)]) / kGhostSeconds;
        if (a > 0) out.emplace_back(ghost_order_[static_cast<std::size_t>(k)], std::min(1.0, a));
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
}

void Timeline::extend_failures() {
    auto kill = [&](std::uint32_t e, const std::string *name) {
        std::size_t w = 0;
        for (std::uint32_t f : fail_alive_) {
            if (name == nullptr || failures_[f].name == *name) {
                failures_[f].death = e;
                failure_index_.set_death(f, e);
                changed_failures_.push_back(f);
            } else {
                fail_alive_[w++] = f;
            }
        }
        fail_alive_.resize(w);
    };
    auto pad_position = [&](const Route &r, const std::string &label, Failure &f) {
        if (f.session >= t_.runs().size() || !t_.runs()[f.session].valid) return;
        const Frame fr(t_.runs()[f.session]);
        for (const RoutePad &p : r.pads)
            if (p.label == label) {
                f.x = fr.x(p.cx);
                f.y = fr.y(p.cy);
                f.placed = true;
                return;
            }
    };
    auto born = [&](const Failure &f) {
        const auto idx = static_cast<std::uint32_t>(failures_.size());
        fail_alive_.push_back(idx);
        failures_.push_back(f);
        failure_index_.push(f.birth, kForever);
        changed_failures_.push_back(idx);
    };
    // An event's failures depend on the event alone (its kind, record,
    // route and session never change once it exists), so each event is
    // taken in once.
    for (std::uint32_t e = fail_done_; e < events_.size(); ++e) {
        const Event &ev = events_[e];
        switch (ev.kind) {
        case EvKind::Session: kill(e, nullptr); break;
        case EvKind::RouteBegin: {
            const Route &r = t_.routes()[ev.record];
            kill(e, &r.name);
            latest_route_[r.name] = ev.record;
            break;
        }
        case EvKind::RouteEnd: {
            const RouteEnd &re = t_.route_ends()[ev.record];
            const Route *r = ev.route != kNone32 ? &t_.routes()[ev.route] : nullptr;
            for (const std::string &label : re.failed) {
                Failure f;
                f.birth = e;
                f.session = ev.session;
                f.net = re.net;
                f.name = r != nullptr ? r->name : std::string();
                f.label = label;
                if (r != nullptr) pad_position(*r, label, f);
                born(f);
            }
            break;
        }
        case EvKind::Drop: {
            const Drop &d = t_.drops()[ev.record];
            if (d.ok != 0) break;
            Failure f;
            f.birth = e;
            f.session = ev.session;
            f.net = d.net;
            f.name = d.name;
            f.label = d.pad_label;
            f.from_drop = true;
            const std::uint32_t s = drop_search_[ev.record];
            if (s != kNone32 && t_.searches()[s].valid && t_.searches()[s].nsrc_kept > 0 &&
                ev.session < t_.runs().size() && t_.runs()[ev.session].valid) {
                // The centre of the pad's cells: the sources of its search.
                const Search &sr = t_.searches()[s];
                const Frame fr(t_.runs()[ev.session]);
                double sx = 0, sy = 0;
                for (std::uint32_t k = 0; k < sr.nsrc_kept; ++k) {
                    const CellPos p = fr.cell(sr.src()[k]);
                    sx += fr.cx(p.i);
                    sy += fr.cy(p.j);
                }
                f.x = sx / sr.nsrc_kept;
                f.y = sy / sr.nsrc_kept;
                f.placed = true;
            } else {
                auto it = latest_route_.find(d.name);
                if (it != latest_route_.end()) pad_position(t_.routes()[it->second], d.pad_label, f);
            }
            born(f);
            break;
        }
        default: break;
        }
    }
    fail_done_ = static_cast<std::uint32_t>(events_.size());
    std::sort(changed_failures_.begin(), changed_failures_.end());
    changed_failures_.erase(std::unique(changed_failures_.begin(), changed_failures_.end()), changed_failures_.end());
}

void Timeline::alive_failures(std::uint32_t e, std::vector<std::uint32_t> &out) const {
    if (e >= events_.size()) return;
    failure_index_.query(e, out);
}

bool Timeline::board_holds_routing(std::uint32_t e, std::uint32_t board) const {
    if (e >= events_.size() || board >= t_.boards().size()) return false;
    const BoardInfo &b = t_.boards()[board];
    if (b.records_by_kind[brd_kind::kTrack] + b.records_by_kind[brd_kind::kVia] == 0) return false;
    const std::uint32_t change = last_copper_change(events_[e].session);
    if (change == kNone32) return false;
    return b.chunk > change && b.chunk <= events_[e].last;
}

} // namespace rv

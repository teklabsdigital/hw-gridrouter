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

std::unique_ptr<Pacing> make_pacing(const PacingSettings &s) {
    if (s.mode == PacingMode::RealTime) return std::unique_ptr<Pacing>(new RealTimePacing(s.slowdown));
    return std::unique_ptr<Pacing>(new FitLengthPacing(s.fit_seconds, s.mode, s.net));
}

// --- interval index ---------------------------------------------------------------

void IntervalIndex::build(const std::vector<std::uint32_t> &births, const std::vector<std::uint32_t> &deaths) {
    births_ = births;
    size_ = births.size();
    std::size_t cap = 1;
    while (cap < size_) cap <<= 1;
    tree_.assign(2 * cap, 0);
    for (std::size_t k = 0; k < size_; ++k) tree_[cap + k] = deaths[k];
    for (std::size_t k = cap - 1; k >= 1; --k) tree_[k] = std::max(tree_[2 * k], tree_[2 * k + 1]);
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
    collect(1, 0, tree_.size() / 2, n, e, out);
}

// --- timeline -----------------------------------------------------------------------

Timeline::Timeline(const Trace &t, const Pacing &pacing) : t_(t) {
    build_events();
    pace(pacing);
    build_lives();
    build_failures();
}

void Timeline::build_events() {
    const std::vector<Chunk> &ch = t_.chunks();
    const auto n = static_cast<std::uint32_t>(ch.size());
    std::uint32_t pending = 0; // first chunk not yet in an event

    auto add = [&](EvKind kind, std::uint32_t k, std::uint32_t record) -> Event & {
        Event ev;
        ev.kind = kind;
        ev.chunk = k;
        ev.first = pending;
        ev.last = k;
        ev.record = record;
        events_.push_back(ev);
        pending = k + 1;
        return events_.back();
    };
    auto with_net = [](Event &ev, std::int32_t net) {
        ev.net = net;
        ev.flags |= evf::kHasNet;
    };

    // Pass 1: which chunks make events, and the chunk ranges.
    for (std::uint32_t k = 0; k < n; ++k) {
        const Chunk &c = ch[k];
        if (c.kind == Kind::Run) {
            add(EvKind::Session, k, c.record); // a corrupt RUN still starts a session (6 rule 2)
            continue;
        }
        if (c.corrupt || c.record == kNone32) continue;
        switch (c.kind) {
        case Kind::Snap: add(EvKind::Snapshot, k, c.record); break;
        case Kind::Srch: {
            const Search &s = t_.searches()[c.record];
            if (!s.valid) break;
            Event &ev = add(EvKind::Search, k, c.record);
            with_net(ev, s.net);
            if (s.has(srch_flags::kFound)) ev.flags |= evf::kFound;
            else ev.flags |= evf::kFailed;
            if (s.has(srch_flags::kError)) ev.flags |= evf::kError;
            ev.size = s.expanded >= 0 ? static_cast<double>(s.expanded) : static_cast<double>(s.ntrace_total);
            max_size_ = std::max(max_size_, ev.size);
            break;
        }
        case Kind::Cmit: with_net(add(EvKind::Commit, k, c.record), t_.commits()[c.record].net); break;
        case Kind::Via: {
            const Via &v = t_.vias()[c.record];
            if (!v.added || v.copper == kNone32) break; // added = 0 changes nothing
            with_net(add(EvKind::Via, k, c.record), v.net);
            break;
        }
        case Kind::Rbld: add(EvKind::RipUp, k, c.record).flags |= evf::kRebuild; break;
        case Kind::Rip: {
            const Rip &r = t_.rips()[c.record];
            if (!events_.empty() && pending == k && events_.back().kind == EvKind::RipUp &&
                (events_.back().flags & evf::kRebuild) != 0 && r.ctx == 1) {
                events_.back().last = k; // part of the rebuild's rip-up
                pending = k + 1;
                break;
            }
            with_net(add(EvKind::RipUp, k, kNone32), r.net);
            break;
        }
        case Kind::Rnb: {
            const Route &r = t_.routes()[c.record];
            if (!r.valid) break;
            with_net(add(EvKind::RouteBegin, k, c.record), r.net);
            break;
        }
        case Kind::Rne: {
            const RouteEnd &re = t_.route_ends()[c.record];
            Event &ev = add(EvKind::RouteEnd, k, c.record);
            with_net(ev, re.net);
            if (!re.failed.empty()) ev.flags |= evf::kFailed;
            if (re.status == 1) ev.flags |= evf::kError;
            break;
        }
        case Kind::Drop: {
            const Drop &d = t_.drops()[c.record];
            Event &ev = add(EvKind::Drop, k, c.record);
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
                add(EvKind::Phase, k, c.record);
            }
            break;
        case Kind::Brd: add(EvKind::Board, k, c.record); break;
        case Kind::End: add(EvKind::End, k, c.record); break;
        default: break; // absorbed
        }
    }
    if (!events_.empty() && pending < n) events_.back().last = n - 1;

    chunk_event_.assign(n, kNone32);
    for (std::uint32_t e = 0; e < events_.size(); ++e)
        for (std::uint32_t k = events_[e].first; k <= events_[e].last && k < n; ++k) chunk_event_[k] = e;

    // Pass 2: the state after each event.
    last_change_.assign(t_.runs().size(), kNone32);
    drop_search_.assign(t_.drops().size(), kNone32);
    counts_.assign(events_.size(), Counts());
    Counts cnt;
    std::uint32_t open_route = kNone32, board = kNone32, phase = kNone32, drop_search = kNone32;
    for (std::uint32_t e = 0; e < events_.size(); ++e) {
        Event &ev = events_[e];
        bool closes = false;
        for (std::uint32_t k = ev.first; k <= ev.last; ++k) {
            const Chunk &c = ch[k];
            if (c.kind == Kind::Run) {
                open_route = kNone32;
                drop_search = kNone32;
                continue;
            }
            if (c.corrupt || c.record == kNone32) continue;
            switch (c.kind) {
            case Kind::Srch:
                if (!t_.searches()[c.record].valid) break;
                ++cnt.searches;
                if (t_.searches()[c.record].ctx == 2) drop_search = c.record;
                break;
            case Kind::Cmit:
                ++cnt.commits;
                cnt.vias += static_cast<std::uint32_t>(t_.commits()[c.record].vias.size());
                if (c.session < last_change_.size()) last_change_[c.session] = k;
                break;
            case Kind::Via:
                if (t_.vias()[c.record].added) {
                    ++cnt.vias;
                    if (c.session < last_change_.size()) last_change_[c.session] = k;
                }
                break;
            case Kind::Rip: {
                const Rip &r = t_.rips()[c.record];
                ++cnt.rips;
                cnt.removed += static_cast<std::uint32_t>(r.applied.size());
                if (!r.applied.empty() && c.session < last_change_.size()) last_change_[c.session] = k;
                break;
            }
            case Kind::Rnb:
                if (!t_.routes()[c.record].valid) break;
                ++cnt.routes;
                open_route = c.record;
                break;
            case Kind::Rne: closes = true; break;
            case Kind::Drop:
                drop_search_[c.record] = drop_search;
                drop_search = kNone32;
                break;
            case Kind::Brd: board = c.record; break;
            default: break;
            }
        }
        if (ev.kind == EvKind::Phase) {
            for (std::uint32_t p = 0; p < phases_.size(); ++p)
                if (phases_[p].event == e) phase = p;
        }
        ev.session = ch[ev.last].session;
        ev.board = board;
        ev.phase = phase;
        ev.route = open_route;
        if (ev.kind == EvKind::RouteEnd) {
            const std::uint32_t ri = t_.route_index(t_.route_ends()[ev.record].route_id);
            if (ri != kNone32 && ri < t_.routes().size()) ev.route = ri;
        }
        if (ev.kind == EvKind::RipUp && ev.record != kNone32 && open_route != kNone32 &&
            (ev.flags & evf::kHasNet) == 0) {
            ev.net = t_.routes()[open_route].net;
            ev.flags |= evf::kHasNet;
        }
        if (closes) open_route = kNone32;
        counts_[e] = cnt;
    }
}

void Timeline::repace(const Pacing &pacing) {
    pace(pacing);
    for (std::size_t k = 0; k < ghost_order_.size(); ++k) ghost_start_[k] = events_[life_[ghost_order_[k]].death].start;
}

void Timeline::pace(const Pacing &pacing) {
    rip_ticks_.clear();
    failure_ticks_.clear();
    total_ = 0;
    if (events_.empty()) {
        for (Phase &p : phases_) p.start = p.end = 0;
        return;
    }
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
    for (const Event &ev : events_) {
        if (ev.kind == EvKind::RipUp) rip_ticks_.push_back(ev.start);
        if ((ev.kind == EvKind::RouteEnd || ev.kind == EvKind::Drop) && (ev.flags & evf::kFailed) != 0)
            failure_ticks_.push_back(ev.start);
    }
    for (Phase &p : phases_) p.start = events_[p.event].start;
    for (std::size_t p = 0; p < phases_.size(); ++p)
        phases_[p].end = p + 1 < phases_.size() ? phases_[p + 1].start : total_;
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

void Timeline::build_lives() {
    const std::vector<Copper> &cu = t_.copper();
    life_.assign(cu.size(), CopperLife());
    std::vector<std::uint32_t> births(cu.size()), deaths(cu.size());
    std::uint32_t prev = 0;
    for (std::size_t k = 0; k < cu.size(); ++k) {
        const Copper &c = cu[k];
        CopperLife &l = life_[k];
        const std::uint32_t b = c.valid ? event_of_chunk(c.chunk) : kNone32;
        if (b == kNone32) {
            births[k] = prev;
            deaths[k] = prev; // empty interval
            continue;
        }
        l.birth = b;
        if (c.removed_by != kNone32) {
            l.death = event_of_chunk(c.removed_by);
            l.ripped = true;
        } else if (c.session != kNone32 && c.session + 1 < t_.runs().size()) {
            l.death = event_of_chunk(t_.runs()[c.session + 1].chunk);
        }
        births[k] = b;
        deaths[k] = l.death == kNone32 ? std::numeric_limits<std::uint32_t>::max() : l.death;
        prev = b;
    }
    copper_index_.build(births, deaths);

    for (std::uint32_t k = 0; k < life_.size(); ++k)
        if (life_[k].ripped && life_[k].birth != kNone32 && life_[k].death != kNone32) ghost_order_.push_back(k);
    std::stable_sort(ghost_order_.begin(), ghost_order_.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return life_[a].death < life_[b].death; });
    ghost_start_.resize(ghost_order_.size());
    for (std::size_t k = 0; k < ghost_order_.size(); ++k) ghost_start_[k] = events_[life_[ghost_order_[k]].death].start;
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

void Timeline::build_failures() {
    std::vector<std::uint32_t> alive;                   // failure indices
    std::map<std::string, std::uint32_t> latest_route;  // name -> routes() index
    auto kill = [&](std::uint32_t e, const std::string *name) {
        std::size_t w = 0;
        for (std::uint32_t f : alive) {
            if (name == nullptr || failures_[f].name == *name) failures_[f].death = e;
            else alive[w++] = f;
        }
        alive.resize(w);
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
    for (std::uint32_t e = 0; e < events_.size(); ++e) {
        const Event &ev = events_[e];
        switch (ev.kind) {
        case EvKind::Session: kill(e, nullptr); break;
        case EvKind::RouteBegin: {
            const Route &r = t_.routes()[ev.record];
            kill(e, &r.name);
            latest_route[r.name] = ev.record;
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
                alive.push_back(static_cast<std::uint32_t>(failures_.size()));
                failures_.push_back(f);
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
                auto it = latest_route.find(d.name);
                if (it != latest_route.end()) pad_position(t_.routes()[it->second], d.pad_label, f);
            }
            alive.push_back(static_cast<std::uint32_t>(failures_.size()));
            failures_.push_back(f);
            break;
        }
        default: break;
        }
    }
    std::vector<std::uint32_t> births(failures_.size()), deaths(failures_.size());
    for (std::size_t k = 0; k < failures_.size(); ++k) {
        births[k] = failures_[k].birth;
        deaths[k] = failures_[k].death == kNone32 ? std::numeric_limits<std::uint32_t>::max() : failures_[k].death;
    }
    failure_index_.build(births, deaths);
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

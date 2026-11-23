// inspect.cpp: the nets table and the per-net history (inspect.hpp).

#include "inspect.hpp"

#include "timeline.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace rv {

namespace {

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// Digits grouped by thin spaces: "1 182 754".
std::string grouped(long long v) {
    std::string d = std::to_string(v < 0 ? -v : v), out;
    for (std::size_t k = 0; k < d.size(); ++k) {
        if (k > 0 && (d.size() - k) % 3 == 0) out += " ";
        out += d[k];
    }
    return v < 0 ? "-" + out : out;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int compare_names(const std::string &a, const std::string &b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t k = 0; k < n; ++k) {
        const char x = lower(a[k]), y = lower(b[k]);
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
}

bool contains_folded(const std::string &hay, const std::string &needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (std::size_t k = 0; k + needle.size() <= hay.size(); ++k) {
        std::size_t q = 0;
        while (q < needle.size() && lower(hay[k + q]) == lower(needle[q])) ++q;
        if (q == needle.size()) return true;
    }
    return false;
}

// Does a chunk of the net's list make one of the net's events?
bool event_chunk(const Trace &t, const Chunk &c) {
    if (c.corrupt || c.record == kNone32) return false;
    switch (c.kind) {
    case Kind::Pad:
    case Kind::Rect:
    case Kind::Disc: return false;
    case Kind::Via: return t.vias()[c.record].added;
    case Kind::Srch: return t.searches()[c.record].valid;
    case Kind::Rnb: return t.routes()[c.record].valid;
    default: return true;
    }
}

// The chunks about a net plus the RIP chunks that removed its copper,
// ascending and unique.
std::vector<std::uint32_t> chunks_of(const Trace &t, std::int32_t net) {
    std::vector<std::uint32_t> out;
    auto it = t.net_chunks().find(net);
    if (it != t.net_chunks().end()) out = it->second;
    for (const Copper &c : t.copper())
        if (c.net == net && c.removed_by != kNone32) out.push_back(c.removed_by);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const char *ctx_name(std::uint32_t ctx) {
    return ctx == 1 ? "route_net" : ctx == 2 ? "drop_to_plane" : "direct";
}

} // namespace

std::string router_net_name(const Trace &t, std::int32_t net) {
    auto it = t.net_names().find(net);
    if (it != t.net_names().end() && !it->second.empty()) return it->second;
    return fmt("net %d", net);
}

const char *net_state_name(std::uint32_t state) noexcept {
    static const char *const names[kNetStateCount] = {"not started", "routing", "routed", "complete", "ripped",
                                                      "failed"};
    return state < kNetStateCount ? names[state] : "";
}

const char *net_column_title(std::uint32_t column) noexcept {
    static const char *const names[netcol::Count] = {
        "Net", "Code", "Session", "State", "Routes", "Searches", "Found", "Not found", "Commits", "Vias",
        "Rip-ups", "Failed pads", "Expanded", "Router s", "First event", "Last event", "Copper", "Failures",
    };
    return column < netcol::Count ? names[column] : "";
}

std::uint32_t net_column_align(std::uint32_t column) noexcept {
    return column == netcol::Name || column == netcol::State ? 0u : 1u;
}

// --- the table ---------------------------------------------------------------------

void NetTable::clear() {
    built_ = false;
    rows_.clear();
    view_.clear();
    now_.clear();
    event_ = kNone32 - 1;
    view_dirty_ = true;
    ++generation_;
}

void NetTable::build(const Trace &t, const Timeline &tl) {
    clear();
    const std::vector<Chunk> &ch = t.chunks();
    const std::uint32_t nev = tl.size();
    std::map<std::pair<std::uint32_t, std::int32_t>, std::uint32_t> index; // (session, net) -> row
    auto row = [&](std::uint32_t session, std::int32_t net) -> NetRow & {
        auto key = std::make_pair(session, net);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, static_cast<std::uint32_t>(rows_.size())).first;
            NetRow r;
            r.net = net;
            r.session = session;
            auto nm = t.net_names().find(net);
            r.named = nm != t.net_names().end() && !nm->second.empty();
            r.name = router_net_name(t, net);
            rows_.push_back(r);
        }
        return rows_[it->second];
    };
    auto session_end_time = [&](std::uint32_t s) {
        if (s >= t.runs().size()) return ch.empty() ? 0.0 : ch.back().t;
        const std::uint32_t end = t.runs()[s].end_chunk;
        return end > 0 && end <= ch.size() ? ch[end - 1].t : (ch.empty() ? 0.0 : ch.back().t);
    };

    // Totals and events, chunk by chunk.
    std::vector<std::set<std::uint32_t>> rip_events; // per row
    for (const auto &kv : t.net_chunks()) {
        const std::int32_t net = kv.first;
        for (std::uint32_t k : kv.second) {
            if (k >= ch.size()) continue;
            const Chunk &c = ch[k];
            if (c.session == kNone32) continue;
            NetRow &r = row(c.session, net);
            if (event_chunk(t, c) && nev > 0) r.events.push_back(tl.event_of_chunk(k));
            if (c.corrupt || c.record == kNone32) continue;
            switch (c.kind) {
            case Kind::Srch: {
                const Search &s = t.searches()[c.record];
                if (!s.valid) break;
                ++r.searches;
                if (s.has(srch_flags::kFound)) ++r.found;
                else ++r.not_found;
                if (s.expanded > 0) r.expanded += s.expanded;
                if (s.ctx != 1 && s.duration_s > 0) r.seconds += s.duration_s;
                break;
            }
            case Kind::Cmit:
                ++r.commits;
                r.vias += static_cast<std::uint32_t>(t.commits()[c.record].vias.size());
                break;
            case Kind::Via:
                if (t.vias()[c.record].added) ++r.vias;
                break;
            case Kind::Rnb: {
                const Route &ro = t.routes()[c.record];
                if (!ro.valid) break;
                ++r.routes;
                if (ro.end == kNone32 || ro.end >= t.route_ends().size()) {
                    const double d = session_end_time(c.session) - c.t;
                    if (d > 0) r.seconds += d;
                }
                break;
            }
            case Kind::Rne: {
                const RouteEnd &re = t.route_ends()[c.record];
                r.failed_pads += static_cast<std::uint32_t>(re.failed.size());
                const std::uint32_t ri = t.route_index(re.route_id);
                if (ri != kNone32 && ri < t.routes().size() && t.routes()[ri].chunk < ch.size()) {
                    const double d = c.t - ch[t.routes()[ri].chunk].t;
                    if (d > 0) r.seconds += d;
                }
                break;
            }
            case Kind::Drop:
                if (t.drops()[c.record].ok == 0) ++r.failed_pads;
                break;
            default: break;
            }
        }
    }
    // Rip-ups suffered: the events of the RIPs that removed the row's copper.
    rip_events.resize(rows_.size());
    for (const Copper &cu : t.copper()) {
        if (cu.removed_by == kNone32 || cu.session == kNone32 || nev == 0) continue;
        NetRow &r = row(cu.session, cu.net);
        const std::uint32_t idx = static_cast<std::uint32_t>(&r - rows_.data());
        if (rip_events.size() < rows_.size()) rip_events.resize(rows_.size());
        const std::uint32_t e = tl.event_of_chunk(cu.removed_by);
        rip_events[idx].insert(e);
        r.events.push_back(e);
    }
    // Nets named but never mentioned.
    {
        std::set<std::int32_t> seen;
        for (const NetRow &r : rows_) seen.insert(r.net);
        for (const auto &kv : t.net_names())
            if (seen.count(kv.first) == 0) row(kNone32, kv.first);
    }
    rip_events.resize(rows_.size());

    // Session ends: the last event of each session.
    std::vector<std::uint32_t> session_last(t.runs().size(), nev == 0 ? kNone32 : nev - 1);
    for (std::size_t s = 0; s + 1 < t.runs().size(); ++s) {
        const std::uint32_t e = tl.event_of_chunk(t.runs()[s + 1].chunk);
        session_last[s] = e == kNone32 || e == 0 ? 0 : e - 1;
    }

    // Changes of the at-position values, per row.
    struct Change {
        std::uint32_t event;
        int kind; // 0 copper born, 1 copper removed, 2 failure born, 3 failure gone, 4 route begins, 5 route ends
        bool complete;
    };
    std::vector<std::vector<Change>> changes(rows_.size());
    const std::vector<CopperLife> &life = tl.copper_life();
    for (std::size_t k = 0; k < t.copper().size() && k < life.size(); ++k) {
        const Copper &cu = t.copper()[k];
        const CopperLife &l = life[k];
        if (l.birth == kNone32 || cu.session == kNone32) continue;
        auto it = index.find(std::make_pair(cu.session, cu.net));
        if (it == index.end()) continue;
        changes[it->second].push_back({l.birth, 0, false});
        if (l.death != kNone32) changes[it->second].push_back({l.death, 1, false});
    }
    for (const Failure &f : tl.failures()) {
        auto it = index.find(std::make_pair(f.session, f.net));
        if (it == index.end()) continue;
        changes[it->second].push_back({f.birth, 2, false});
        if (f.death != kNone32) changes[it->second].push_back({f.death, 3, false});
    }
    for (std::uint32_t e = 0; e < nev; ++e) {
        const Event &ev = tl.events()[e];
        if (ev.kind != EvKind::RouteBegin && ev.kind != EvKind::RouteEnd) continue;
        auto it = index.find(std::make_pair(ev.session, ev.net));
        if (it == index.end()) continue;
        if (ev.kind == EvKind::RouteBegin) {
            changes[it->second].push_back({e, 4, false});
        } else {
            const RouteEnd &re = t.route_ends()[ev.record];
            changes[it->second].push_back({e, 5, re.status == 0 && re.failed.empty()});
        }
    }

    for (std::size_t ri = 0; ri < rows_.size(); ++ri) {
        NetRow &r = rows_[ri];
        std::sort(r.events.begin(), r.events.end());
        r.events.erase(std::unique(r.events.begin(), r.events.end()), r.events.end());
        if (!r.events.empty()) {
            r.first_event = r.events.front();
            r.last_event = r.events.back();
        }
        r.rips = static_cast<std::uint32_t>(rip_events[ri].size());
        r.session_last = r.session < session_last.size() ? session_last[r.session] : kNone32;

        std::vector<Change> &cs = changes[ri];
        std::stable_sort(cs.begin(), cs.end(), [](const Change &a, const Change &b) { return a.event < b.event; });
        std::int64_t copper = 0, failures = 0;
        bool routing = false, complete = false, last_removed = false;
        for (std::size_t k = 0; k < cs.size();) {
            const std::uint32_t e = cs[k].event;
            for (; k < cs.size() && cs[k].event == e; ++k) {
                switch (cs[k].kind) {
                case 0:
                    ++copper;
                    last_removed = false;
                    break;
                case 1:
                    --copper;
                    last_removed = true;
                    complete = false;
                    break;
                case 2: ++failures; break;
                case 3: --failures; break;
                case 4:
                    routing = true;
                    complete = false;
                    break;
                case 5:
                    routing = false;
                    complete = cs[k].complete;
                    break;
                default: break;
                }
            }
            NetRow::Point p;
            p.event = e;
            p.now.copper = static_cast<std::uint32_t>(std::max<std::int64_t>(0, copper));
            p.now.failures = static_cast<std::uint32_t>(std::max<std::int64_t>(0, failures));
            if (routing) p.now.state = NetState::Routing;
            else if (failures > 0) p.now.state = NetState::Failed;
            else if (last_removed) p.now.state = NetState::Ripped;
            else if (complete) p.now.state = NetState::Complete;
            else if (copper > 0) p.now.state = NetState::Routed;
            else p.now.state = NetState::NotStarted;
            if (!r.points.empty() && r.points.back().now == p.now) continue;
            r.points.push_back(p);
        }
    }
    built_ = true;
    now_.assign(rows_.size(), NetNow());
    view_dirty_ = true;
}

NetNow NetTable::at(std::uint32_t r, std::uint32_t e) const {
    if (r >= rows_.size() || e == kNone32) return NetNow();
    const NetRow &row = rows_[r];
    if (row.session == kNone32) return NetNow();
    if (row.session_last != kNone32 && e > row.session_last) e = row.session_last;
    auto it = std::upper_bound(row.points.begin(), row.points.end(), e,
                               [](std::uint32_t v, const NetRow::Point &p) { return v < p.event; });
    if (it == row.points.begin()) return NetNow();
    return (it - 1)->now;
}

bool NetTable::update(std::uint32_t e) {
    bool changed = false;
    if (e != event_ || now_.size() != rows_.size()) {
        event_ = e;
        std::vector<NetNow> next(rows_.size());
        for (std::uint32_t r = 0; r < rows_.size(); ++r) next[r] = at(r, e);
        if (next != now_ || now_.size() != rows_.size()) {
            now_ = std::move(next);
            changed = true;
            if (net_column_live(column_)) view_dirty_ = true;
        }
    }
    if (view_dirty_) {
        resort();
        view_dirty_ = false;
        changed = true;
    }
    if (changed) ++generation_;
    return changed;
}

void NetTable::set_view(std::uint32_t column, bool descending, const std::string &filter) {
    if (column == column_ && descending == descending_ && filter == filter_) return;
    column_ = column;
    descending_ = descending;
    filter_ = filter;
    view_dirty_ = true;
}

void NetTable::resort() {
    view_.clear();
    for (std::uint32_t r = 0; r < rows_.size(); ++r)
        if (contains_folded(rows_[r].name, filter_)) view_.push_back(r);
    const std::uint32_t c = column_;
    auto num = [&](std::uint32_t r) -> double {
        const NetRow &x = rows_[r];
        const NetNow &n = r < now_.size() ? now_[r] : NetNow();
        switch (c) {
        case netcol::Net: return x.net;
        case netcol::Session: return x.session == kNone32 ? 1e18 : x.session;
        case netcol::State: return static_cast<double>(n.state);
        case netcol::Routes: return x.routes;
        case netcol::Searches: return x.searches;
        case netcol::Found: return x.found;
        case netcol::NotFound: return x.not_found;
        case netcol::Commits: return x.commits;
        case netcol::Vias: return x.vias;
        case netcol::Rips: return x.rips;
        case netcol::FailedPads: return x.failed_pads;
        case netcol::Expanded: return static_cast<double>(x.expanded);
        case netcol::Seconds: return x.seconds;
        case netcol::First: return x.first_event == kNone32 ? 1e18 : x.first_event;
        case netcol::Last: return x.last_event == kNone32 ? 1e18 : x.last_event;
        case netcol::Copper: return n.copper;
        case netcol::Failures: return n.failures;
        default: return 0;
        }
    };
    auto tie = [&](std::uint32_t a, std::uint32_t b) {
        const NetRow &x = rows_[a], &y = rows_[b];
        if (x.session != y.session) return x.session < y.session;
        return x.net < y.net;
    };
    std::stable_sort(view_.begin(), view_.end(), [&](std::uint32_t a, std::uint32_t b) {
        int cmp = 0;
        if (c == netcol::Name) {
            cmp = compare_names(rows_[a].name, rows_[b].name);
        } else {
            const double x = num(a), y = num(b);
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        }
        if (cmp != 0) return descending_ ? cmp > 0 : cmp < 0;
        return tie(a, b);
    });
}

std::uint32_t NetTable::position_of(std::uint32_t r) const {
    for (std::uint32_t k = 0; k < view_.size(); ++k)
        if (view_[k] == r) return k;
    return kNone32;
}

std::string NetTable::cell(std::uint32_t r, std::uint32_t c) const {
    if (r >= rows_.size()) return "";
    const NetRow &x = rows_[r];
    const NetNow n = r < now_.size() ? now_[r] : NetNow();
    auto event = [](std::uint32_t e) { return e == kNone32 ? std::string("—") : std::to_string(e + 1); };
    switch (c) {
    case netcol::Name: return x.name;
    case netcol::Net: return std::to_string(x.net);
    case netcol::Session: return x.session == kNone32 ? "—" : std::to_string(x.session + 1);
    case netcol::State: return net_state_name(static_cast<std::uint32_t>(n.state));
    case netcol::Routes: return std::to_string(x.routes);
    case netcol::Searches: return std::to_string(x.searches);
    case netcol::Found: return std::to_string(x.found);
    case netcol::NotFound: return std::to_string(x.not_found);
    case netcol::Commits: return std::to_string(x.commits);
    case netcol::Vias: return std::to_string(x.vias);
    case netcol::Rips: return std::to_string(x.rips);
    case netcol::FailedPads: return std::to_string(x.failed_pads);
    case netcol::Expanded: return grouped(static_cast<long long>(x.expanded));
    case netcol::Seconds: return fmt("%.3f", x.seconds);
    case netcol::First: return event(x.first_event);
    case netcol::Last: return event(x.last_event);
    case netcol::Copper: return std::to_string(n.copper);
    case netcol::Failures: return std::to_string(n.failures);
    default: return "";
    }
}

// --- history -------------------------------------------------------------------------

std::vector<HistoryEntry> net_history(const Trace &t, const Timeline &tl, std::int32_t net) {
    std::vector<HistoryEntry> out;
    const std::vector<Chunk> &ch = t.chunks();
    std::uint32_t session = kNone32 - 1;
    std::uint32_t open_route = kNone32;  // routes() index of the net's call in progress
    std::size_t open_entry = 0;          // its entry
    auto ev_of = [&](std::uint32_t k) { return tl.size() == 0 ? kNone32 : tl.event_of_chunk(k); };
    auto add = [&](std::uint32_t depth, std::uint32_t kind, std::uint32_t k, std::string text, std::uint32_t flags) {
        HistoryEntry h;
        h.text = std::move(text);
        h.depth = depth;
        h.kind = kind;
        h.chunk = k;
        h.event = ev_of(k);
        h.session = ch[k].session;
        h.flags = flags;
        out.push_back(std::move(h));
    };
    const std::uint32_t kFound = 1u << 0, kFailed = 1u << 1, kError = 1u << 2; // RV_EVF_*

    for (std::uint32_t k : chunks_of(t, net)) {
        if (k >= ch.size()) continue;
        const Chunk &c = ch[k];
        if (!event_chunk(t, c) || c.session == kNone32) continue;
        if (c.session != session) {
            session = c.session;
            open_route = kNone32;
            const std::uint32_t rc = session < t.runs().size() ? t.runs()[session].chunk : k;
            add(0, hist::Session, rc, fmt("router session %u", session + 1), 0);
        }
        const bool in_route = open_route != kNone32;
        const std::uint32_t d = in_route ? 2 : 1;
        switch (c.kind) {
        case Kind::Rnb: {
            const Route &r = t.routes()[c.record];
            open_route = c.record;
            open_entry = out.size();
            add(1, hist::Route, k,
                fmt("route_net %u/%zu %s, %zu pads: not finished", c.record + 1, t.routes().size(), r.name.c_str(),
                    r.pads.size()),
                0);
            break;
        }
        case Kind::Srch: {
            const Search &s = t.searches()[c.record];
            const bool mine = in_route && s.ctx == 1 && t.route_index(s.route_id) == open_route;
            std::string x = fmt("search %u (%s): ", s.id_field, ctx_name(s.ctx));
            std::uint32_t f = 0;
            if (s.has(srch_flags::kError)) {
                x += "error";
                f |= kError;
            } else if (s.has(srch_flags::kFound)) {
                x += fmt("found, cost %lld, %u path cells", static_cast<long long>(s.cost), s.npath);
                f |= kFound;
            } else {
                x += "not found";
                f |= kFailed;
            }
            if (s.expanded >= 0) x += ", " + grouped(s.expanded) + " states";
            x += fmt(", %.4f s", s.duration_s);
            if (s.repeat_of != kNone32) x += fmt(", repeats search %u", s.repeat_of);
            add(mine ? 2 : 1, hist::Search, k, x, f);
            break;
        }
        case Kind::Cmit: {
            const Commit &cm = t.commits()[c.record];
            const std::uint32_t id = cm.copper < t.copper().size() ? t.copper()[cm.copper].id : kNone32;
            std::string x = fmt("commit: copper %u, %zu path cells", id, cm.path.size());
            if (!cm.vias.empty()) x += fmt(", %zu vias", cm.vias.size());
            if (!cm.stubs.empty()) x += fmt(", %zu stubs", cm.stubs.size());
            add(d, hist::Commit, k, x, 0);
            break;
        }
        case Kind::Via: {
            const Via &v = t.vias()[c.record];
            add(d, hist::Via, k, fmt("via at cell (%d, %d)", v.i, v.j), 0);
            break;
        }
        case Kind::Rbld: {
            const Rebuild &rb = t.rebuilds()[c.record];
            std::string names;
            for (std::size_t q = 0; q < rb.names.size(); ++q) names += (q ? ", " : "") + rb.names[q];
            add(d, hist::RipCaused, k, "rebuild: rips up " + (names.empty() ? std::string("nothing") : names), 0);
            break;
        }
        case Kind::Rip: {
            const Rip &r = t.rips()[c.record];
            std::uint32_t mine = 0, others = 0;
            for (std::uint32_t ci : r.applied)
                if (ci < t.copper().size()) (t.copper()[ci].net == net ? mine : others) += 1;
            std::string cause;
            const std::uint32_t cr = t.route_index(r.route_id);
            if (r.ctx == 1 && cr != kNone32 && cr < t.routes().size()) {
                const Route &ro = t.routes()[cr];
                cause = ro.net == net ? "its own route_net" : "the route_net of " + ro.name;
            } else if (r.ctx == 2) {
                cause = "drop_to_plane";
            } else {
                cause = "a direct rip";
            }
            if (mine > 0) {
                std::string x = fmt("ripped up by %s: %u copper items removed", cause.c_str(), mine);
                if (r.net != net) x += " (a rip of " + router_net_name(t, r.net) + ")";
                add(cr == open_route && cr != kNone32 ? 2 : 1, hist::RipSuffered, k, x, kFailed);
            } else {
                add(d, hist::RipOwn, k, fmt("rip-up of this net (%s): nothing of it removed", cause.c_str()), 0);
            }
            break;
        }
        case Kind::Rne: {
            const RouteEnd &re = t.route_ends()[c.record];
            const std::uint32_t ri = t.route_index(re.route_id);
            std::string result;
            std::uint32_t f = 0;
            if (re.status == 1) {
                result = "raised";
                f |= kError;
            } else if (re.failed.empty()) {
                result = "ok";
            }
            if (!re.failed.empty()) {
                result += fmt("%s%zu pads failed", result.empty() ? "" : ", ", re.failed.size());
                f |= kFailed;
            }
            if (!re.ripped.empty()) result += fmt(", ripped %zu nets", re.ripped.size());
            if (ri == open_route && open_route != kNone32 && open_entry < out.size()) {
                const Route &r = t.routes()[ri];
                out[open_entry].text = fmt("route_net %u/%zu %s, %zu pads: ", ri + 1, t.routes().size(),
                                           r.name.c_str(), r.pads.size()) + result;
                out[open_entry].flags = f;
            }
            add(2, hist::RouteEnd, k, "returns: " + result, f);
            for (const std::string &label : re.failed) add(2, hist::Failure, k, "failed pad " + label, kFailed);
            open_route = kNone32;
            break;
        }
        case Kind::Drop: {
            const Drop &dr = t.drops()[c.record];
            const std::uint32_t f = dr.ok == 1 ? 0 : (dr.ok == 0 ? kFailed : kError);
            add(1, hist::Drop, k,
                fmt("drop_to_plane pad %s: %s", dr.pad_label.c_str(), dr.ok == 1 ? "ok" : dr.ok == 0 ? "failed" : "raised"),
                f);
            if (dr.ok == 0) add(2, hist::Failure, k, "failed pad " + dr.pad_label, kFailed);
            break;
        }
        default: break;
        }
    }
    return out;
}

std::vector<std::uint32_t> net_events(const Trace &t, const Timeline &tl, std::int32_t net) {
    std::vector<std::uint32_t> out;
    for (const HistoryEntry &h : net_history(t, tl, net))
        if (h.kind != hist::Session && h.event != kNone32) out.push_back(h.event);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace rv

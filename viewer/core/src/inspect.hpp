// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// inspect.hpp: the nets table and the per-net attempt history (M5).
//
// Both are functions of the trace and the timeline; the table's "now"
// columns are functions of the playback position as well.
//
// M6: the table grows with the trace (extend): only the rows the appended
// chunks touch are computed again, and the rows equal those of a table built
// at once from the same chunks.  Rows are stored by (net, session).
//
// The table has one row per (router session, router net) that any chunk is
// about (Trace::net_chunks, plus the RIP chunks that removed the net's
// copper), and one row with session kNone32 for each net named by NETS that
// no chunk is about.  Totals per row (a pure function of the file):
//   routes       RNB chunks (valid) of the net in the session
//   searches     SRCH chunks (valid); found / not found by RV_SRCH_FOUND
//   commits      CMIT chunks
//   vias         vias of its CMITs plus VIA chunks with added = 1
//   rips         rip-up events that removed at least one copper item of the
//                net in the session (the net "suffered" them)
//   failed pads  RNE failed labels plus DROP chunks with ok = 0
//   expanded     sum of its searches' expanded states (>= 0 only)
//   seconds      router time: for each route_net call t(RNE) - t(RNB)
//                (an unfinished call: up to the session's last chunk), plus
//                duration_s of its searches outside route_net (ctx != 1)
//   first, last  the first and last of the row's events: the events of its
//                chunks other than PAD, RECT, DISC and VIA with added = 0,
//                and the rip-ups that removed its copper
// At the position (event e; a row of an earlier session is evaluated at that
// session's last event, a row without a session is always "not started"):
//   copper       its copper items alive
//   failures     its failed pads shown (Timeline::alive_failures)
//   state        the first that holds of
//                  routing      a route_net call of the net is in progress
//                               (its RNB event at or before e, its RNE after)
//                  failed       failures > 0
//                  ripped       the last change of its copper at or before e
//                               removed copper
//                  complete     its latest route_net call returned status 0
//                               with no failed pads, and none of its copper
//                               was removed since
//                  routed       copper > 0
//                  not started  otherwise (also: searched, nothing committed)
#ifndef RV_INSPECT_HPP
#define RV_INSPECT_HPP

#include "records.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rv {

class Trace;
class Timeline;

// Values match RV_NET_* in rv.h.
enum class NetState : std::uint32_t { NotStarted = 0, Routing, Routed, Complete, Ripped, Failed };
constexpr std::uint32_t kNetStateCount = 6;
const char *net_state_name(std::uint32_t state) noexcept;

// Columns; values match RV_NETCOL_* in rv.h.
namespace netcol {
enum : std::uint32_t {
    Name = 0, Net, Session, State, Routes, Searches, Found, NotFound, Commits, Vias, Rips, FailedPads, Expanded,
    Seconds, First, Last, Copper, Failures, Count
};
} // namespace netcol
const char *net_column_title(std::uint32_t column) noexcept;
// 0 left aligned (text), 1 right aligned (numbers).
std::uint32_t net_column_align(std::uint32_t column) noexcept;
// Columns whose values depend on the position.
inline bool net_column_live(std::uint32_t c) { return c == netcol::State || c == netcol::Copper || c == netcol::Failures; }

struct NetNow {
    NetState state = NetState::NotStarted;
    std::uint32_t copper = 0, failures = 0;
    bool operator==(const NetNow &o) const { return state == o.state && copper == o.copper && failures == o.failures; }
    bool operator!=(const NetNow &o) const { return !(*this == o); }
};

struct NetRow {
    std::string name;            // the NETS name, else "net <code>"
    bool named = false;          // NETS gives the net a non-empty name
    std::int32_t net = 0;
    std::uint32_t session = kNone32;
    std::uint32_t routes = 0, searches = 0, found = 0, not_found = 0, commits = 0, vias = 0, rips = 0,
                  failed_pads = 0;
    std::int64_t expanded = 0;
    double seconds = 0;
    std::uint32_t first_event = kNone32, last_event = kNone32;
    std::vector<std::uint32_t> events;      // ascending, unique
    std::uint32_t session_last = kNone32;   // the session's last event
    struct Point {
        std::uint32_t event = 0;
        NetNow now;
    };
    std::vector<Point> points;              // the state after each event that changes it
    bool open_route = false;                // M6: a route_net call without RNE (seconds grow)
};

class NetTable {
public:
    void clear();
    void build(const Trace &t, const Timeline &tl);
    // M6: take in what the trace and the timeline gained since the build or
    // the last extend (the timeline extended once since then).  Nothing if
    // not built.
    void extend(const Trace &t, const Timeline &tl);
    bool built() const { return built_; }

    const std::vector<NetRow> &rows() const { return rows_; }
    // Values of row r at event e (kNone32: before any event).
    NetNow at(std::uint32_t r, std::uint32_t e) const;

    // --- the view: sorting, filtering and the values at the position ---------
    // Bring the values to event e.  Returns true if any value (or the view)
    // changed since the last call; the generation then changes.
    bool update(std::uint32_t e);
    void set_view(std::uint32_t column, bool descending, const std::string &filter);
    std::uint32_t sort_column() const { return column_; }
    bool descending() const { return descending_; }
    const std::string &filter() const { return filter_; }
    const std::vector<std::uint32_t> &view() const { return view_; } // row indices, in view order
    const std::vector<NetNow> &now() const { return now_; }          // per row, at the last update
    std::uint32_t generation() const { return generation_; }
    // Position of row r in the view, kNone32 if filtered out.
    std::uint32_t position_of(std::uint32_t r) const;
    // The cell text of row r, column c, at the last update.
    std::string cell(std::uint32_t r, std::uint32_t c) const;

private:
    void resort();
    void sync(const Trace &t, const Timeline &tl, bool fresh);
    std::uint32_t find_row(std::uint32_t session, std::int32_t net) const;
    bool row_exists(const Trace &t, std::uint32_t session, std::int32_t net) const;
    void build_row(const Trace &t, const Timeline &tl, NetRow &r) const;
    void place_row(const Trace &t, const Timeline &tl, std::uint32_t session, std::int32_t net, bool exists);

    bool built_ = false;
    // (session, net) -> copper indices and failure indices, all of them
    std::map<std::pair<std::uint32_t, std::int32_t>, std::vector<std::uint32_t>> copper_by_, failures_by_;
    std::uint32_t chunks_seen_ = 0, copper_seen_ = 0, failures_seen_ = 0;
    std::vector<NetRow> rows_;
    std::uint32_t column_ = netcol::Name;
    bool descending_ = false;
    std::string filter_;
    std::vector<std::uint32_t> view_;
    std::vector<NetNow> now_;
    std::uint32_t event_ = kNone32 - 1; // never a real event: the first update computes
    std::uint32_t generation_ = 1;
    bool view_dirty_ = true;
};

// --- history ----------------------------------------------------------------------

// Entry kinds; values match RV_HIST_* in rv.h.
namespace hist {
enum : std::uint32_t {
    Session = 0, Route, Search, Commit, Via, RipSuffered, RipCaused, Failure, RouteEnd, Drop, RipOwn
};
} // namespace hist

struct HistoryEntry {
    std::string text;
    std::uint32_t depth = 0, kind = 0;
    std::uint32_t event = kNone32, chunk = kNone32, session = kNone32;
    std::uint32_t flags = 0; // RV_EVF_* bits: found, failed, error
};

// The attempts of router net `net`, in file order (see rv_net_history in
// rv.h for the layout and the texts).
std::vector<HistoryEntry> net_history(const Trace &t, const Timeline &tl, std::int32_t net);

// The events of a net (for timeline marks): those of its history entries
// other than session headers, ascending and unique.
std::vector<std::uint32_t> net_events(const Trace &t, const Timeline &tl, std::int32_t net);

// The display name of a router net: its NETS name, else "net <code>".
std::string router_net_name(const Trace &t, std::int32_t net);

} // namespace rv

#endif // RV_INSPECT_HPP

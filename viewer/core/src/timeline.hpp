// timeline.hpp: the routing run as a sequence of events on a playback clock.
//
// Built once from the reader's tables (and again after a refresh).  The
// state at any event is a pure function of the event index, computed
// without keyframes:
//   - copper: each copper item is alive from the event of its creating chunk
//     until the event of the RIP that removed it, or the next RUN (spec 8);
//   - failures: each failed pad is shown from the RNE or DROP that reported
//     it until the next route_net of the same name, or the next RUN (7.10);
//   - board: the latest BRD at or before the event's last chunk.
// Each of these is an interval per item; an interval index enumerates the
// items alive at an event in time proportional to what it returns.
//
// What counts as an event (one per chunk unless noted):
//   RUN -> Session, SNAP -> Snapshot, SRCH -> Search, CMIT -> Commit,
//   VIA with added = 1 -> Via, RBLD and the RIP chunks with ctx = 1 that
//   immediately follow it -> one RipUp, any other RIP -> RipUp,
//   RNB -> RouteBegin, RNE -> RouteEnd, DROP -> Drop, MARK level 0 -> Phase,
//   BRD -> Board, END -> End.
// Every other chunk (NETS, PAD, RECT, DISC, GRDS, ROUT, MARK notes, VIA with
// added = 0, corrupt and unknown chunks) is absorbed: it is applied with the
// next event, and chunks after the last event extend it.
#ifndef RV_TIMELINE_HPP
#define RV_TIMELINE_HPP

#include "records.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rv {

class Trace;

// Values match rv_event_kind in rv.h.
enum class EvKind : std::uint32_t {
    Session = 0, Snapshot, Search, Commit, Via, RipUp, RouteBegin, RouteEnd, Drop, Phase, Board, End,
};
constexpr std::uint32_t kEvKindCount = 12;
const char *event_kind_name(std::uint32_t kind) noexcept;

// Event flags; values match RV_EVF_* in rv.h.
namespace evf {
constexpr std::uint32_t kFound = 1u << 0;   // a search that found a path
constexpr std::uint32_t kFailed = 1u << 1;  // search not found, RNE with failed pads, DROP not ok
constexpr std::uint32_t kError = 1u << 2;   // SRCH error, RNE status 1, DROP raised
constexpr std::uint32_t kHasNet = 1u << 3;  // `net` is meaningful
constexpr std::uint32_t kRebuild = 1u << 4; // a RipUp that starts with an RBLD
} // namespace evf

struct Event {
    EvKind kind = EvKind::End;
    std::uint32_t flags = 0;
    std::uint32_t chunk = kNone32;   // the chunk that made the event
    std::uint32_t first = 0;         // chunks applied by this event: [first, last]
    std::uint32_t last = 0;
    // Session: run index; Snapshot: snaps(); Search: searches(); Commit:
    // commits(); Via: vias(); RipUp: rebuilds() index or kNone32 for a lone
    // RIP (the RIP chunks are those in [chunk, last]); RouteBegin: routes();
    // RouteEnd: route_ends(); Drop: drops(); Phase: marks(); Board: boards();
    // End: ends().
    std::uint32_t record = kNone32;
    std::int32_t net = 0;
    std::uint32_t session = kNone32; // session of the last chunk applied
    std::uint32_t route = kNone32;   // route_net in progress (routes() index), RNB to RNE inclusive
    std::uint32_t phase = kNone32;   // index into Timeline::phases()
    std::uint32_t board = kNone32;   // latest BRD (boards() index) at or before `last`
    double size = 0;                 // Search: expanded states (else ntrace_total)
    double start = 0, duration = 0;  // playback seconds
};

// Running totals after an event has been applied.
struct Counts {
    std::uint32_t searches = 0, commits = 0, vias = 0, rips = 0, removed = 0, routes = 0;
};

struct Phase {
    std::uint32_t event = 0;
    std::string label;
    double start = 0, end = 0;
};

// One failed pad (RNE failed label, or DROP with ok = 0).
struct Failure {
    std::uint32_t birth = 0, death = kNone32; // alive at events [birth, death)
    std::uint32_t session = kNone32;
    std::string name, label;
    bool placed = false;      // position known
    bool from_drop = false;
    double x = 0, y = 0;      // board mm
};

// The life of one copper item (index into Trace::copper()).
struct CopperLife {
    std::uint32_t birth = kNone32;  // kNone32: never drawn (corrupt CMIT)
    std::uint32_t death = kNone32;  // event that removed it; kNone32: alive to the end
    bool ripped = false;            // death is a RipUp (it leaves a ghost)
};

// Pacing: how long each event lasts in playback seconds.  Replaceable.
// An event may get no time (duration 0): it is applied together with the
// next event that has time and is never the current event itself.
class Pacing {
public:
    virtual ~Pacing() = default;
    // Playback seconds of every event (>= 0), in event order.  Returns the
    // length of the run: the sum of the durations (rounding aside; the last
    // event with time is stretched or shrunk to end there), 0 if none.
    virtual double durations(const Trace &t, const std::vector<Event> &events, std::vector<double> &out) const = 0;
};

// Pacing modes; values match rv_pacing_mode in rv.h.
enum class PacingMode : std::uint32_t { Fit = 0, NetsOnly, OneNet, SkipFailed, RealTime };
constexpr std::uint32_t kPacingModeCount = 5;
const char *pacing_mode_name(std::uint32_t mode) noexcept;

struct PacingSettings {
    PacingMode mode = PacingMode::Fit;
    std::int32_t net = 0;        // OneNet
    double fit_seconds = 180.0;  // the weight modes: length of the run at 1x
    double slowdown = 10.0;      // RealTime: playback seconds per router second
    bool operator==(const PacingSettings &o) const {
        return mode == o.mode && net == o.net && fit_seconds == o.fit_seconds && slowdown == o.slowdown;
    }
    bool operator!=(const PacingSettings &o) const { return !(*this == o); }
};
// Limits of the settings (rv_set_pacing rejects values outside).
constexpr double kFitSecondsMin = 1.0, kFitSecondsMax = 86400.0;
constexpr double kSlowdownMin = 0.01, kSlowdownMax = 100000.0;

// The weight modes ("fit length" and its variants): every event gets a
// weight, and the weights are scaled so that the run lasts fit_seconds.
// Weights: a search 1 + ln(1 + size), where a search that found nothing
// counts at most kFailedSizeCap states (its wavefront plays shortened); a
// commit 2, a via 1, a rip-up 3, a route_net begin 0.5, an end 0.5 (2 with
// failed pads), a drop 1, a phase 4, a snapshot 0.5, a session 1, a board
// 2, the end 1.  The variants set some weights to 0: NetsOnly every search,
// SkipFailed every search that found nothing, OneNet every event that is not
// of `net`.  When every weight is 0 the run lasts 0 s.
class FitLengthPacing : public Pacing {
public:
    static constexpr double kTotal = 180.0;
    static constexpr double kFailedSizeCap = 1000.0;
    explicit FitLengthPacing(double total = kTotal, PacingMode mode = PacingMode::Fit, std::int32_t net = 0)
        : total_(total), mode_(mode), net_(net) {}
    // The weight of an event in this mode (>= 0).
    double weight(const Event &e) const;
    double total_seconds() const { return total_; }
    double durations(const Trace &t, const std::vector<Event> &events, std::vector<double> &out) const override;

private:
    double total_;
    PacingMode mode_;
    std::int32_t net_;
};

// Real time: an event lasts as long as the router took from the end of the
// event before it to the end of this one (the times of their last chunks;
// the first event lasts 0), times the slowdown.  A search that found nothing
// lasts at most kFailedSecondsCap router seconds (its wavefront plays
// shortened).
class RealTimePacing : public Pacing {
public:
    static constexpr double kFailedSecondsCap = 0.010;
    explicit RealTimePacing(double slowdown) : slowdown_(slowdown) {}
    double durations(const Trace &t, const std::vector<Event> &events, std::vector<double> &out) const override;

private:
    double slowdown_;
};

std::unique_ptr<Pacing> make_pacing(const PacingSettings &s);

// Items alive at an event: births non-decreasing in item order, each alive
// for events [birth, death).
class IntervalIndex {
public:
    void build(const std::vector<std::uint32_t> &births, const std::vector<std::uint32_t> &deaths);
    // Appends the items alive at event e, ascending.
    void query(std::uint32_t e, std::vector<std::uint32_t> &out) const;

private:
    void collect(std::size_t node, std::size_t lo, std::size_t hi, std::size_t n, std::uint32_t e,
                 std::vector<std::uint32_t> &out) const;
    std::vector<std::uint32_t> births_;
    std::vector<std::uint32_t> tree_; // max death per node
    std::size_t size_ = 0;
};

class Timeline {
public:
    // Ghost of ripped copper: visible for kGhostSeconds of playback time
    // after the rip-up, alpha falling linearly from 1 to 0.
    static constexpr double kGhostSeconds = 1.5;

    Timeline(const Trace &t, const Pacing &pacing);

    // Give the events new times (another pacing); everything else stays.
    void repace(const Pacing &pacing);

    const std::vector<Event> &events() const { return events_; }
    std::uint32_t size() const { return static_cast<std::uint32_t>(events_.size()); }
    double total() const { return total_; }
    // The event at playback time t (clamped): the last event starting at or
    // before t, so an event without time is never the result unless it is
    // the last one; kNone32 with no events.
    std::uint32_t event_at(double t) const;
    // The event containing chunk c (an absorbed chunk belongs to the next
    // event), kNone32 with no events.
    std::uint32_t event_of_chunk(std::uint32_t c) const;
    const Counts &counts(std::uint32_t e) const { return counts_[e]; }
    const std::vector<Phase> &phases() const { return phases_; }
    const std::vector<double> &rip_ticks() const { return rip_ticks_; }
    const std::vector<double> &failure_ticks() const { return failure_ticks_; }
    double max_search_size() const { return max_size_; }

    // Copper.
    const std::vector<CopperLife> &copper_life() const { return life_; }
    void alive_copper(std::uint32_t e, std::vector<std::uint32_t> &out) const;
    // Ghosts at playback time t: (copper index, alpha 0..1], ascending index.
    void ghosts(double t, std::vector<std::pair<std::uint32_t, double>> &out) const;

    // Failures.
    const std::vector<Failure> &failures() const { return failures_; }
    void alive_failures(std::uint32_t e, std::vector<std::uint32_t> &out) const;

    // The last chunk of session s that created or removed copper, kNone32
    // if none.
    std::uint32_t last_copper_change(std::uint32_t session) const {
        return session < last_change_.size() ? last_change_[session] : kNone32;
    }
    // Is the routed copper hidden because the board drawn already holds it?
    // True when the board (BRD index) is written after the last copper
    // change of the event's session, at or before the event's last chunk,
    // and has at least one TRACK or VIA record.
    bool board_holds_routing(std::uint32_t e, std::uint32_t board) const;

private:
    void build_events();
    void build_lives();
    void build_failures();
    void pace(const Pacing &pacing);

    const Trace &t_;
    std::vector<Event> events_;
    std::vector<std::uint32_t> chunk_event_; // per chunk
    std::vector<Counts> counts_;
    std::vector<Phase> phases_;
    std::vector<double> rip_ticks_, failure_ticks_;
    std::vector<CopperLife> life_;
    IntervalIndex copper_index_;
    std::vector<std::uint32_t> ghost_order_;   // ripped copper, by death event
    std::vector<double> ghost_start_;          // start time of that death event
    std::vector<Failure> failures_;
    IntervalIndex failure_index_;
    std::vector<std::uint32_t> last_change_;   // per session
    std::vector<std::uint32_t> drop_search_;   // per DROP record: preceding ctx-2 search, or kNone32
    double total_ = 0;
    double max_size_ = 0;
};

} // namespace rv

#endif // RV_TIMELINE_HPP

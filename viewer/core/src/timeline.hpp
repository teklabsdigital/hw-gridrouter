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
class Pacing {
public:
    virtual ~Pacing() = default;
    // Relative weight of an event, > 0.
    virtual double weight(const Event &e) const = 0;
    // Length of the whole run in playback seconds at 1x.
    virtual double total_seconds() const = 0;
};

// "Fit length": the whole run lasts kTotal seconds at 1x; a search's weight
// grows with the log of its size, so big searches get more time.
class FitLengthPacing : public Pacing {
public:
    static constexpr double kTotal = 180.0;
    double weight(const Event &e) const override;
    double total_seconds() const override { return kTotal; }
};

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

    const std::vector<Event> &events() const { return events_; }
    std::uint32_t size() const { return static_cast<std::uint32_t>(events_.size()); }
    double total() const { return total_; }
    // The event at playback time t (clamped), kNone32 with no events.
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

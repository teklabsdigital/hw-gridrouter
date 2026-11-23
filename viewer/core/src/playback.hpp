// playback.hpp: the playback clock, stepping, and the strings a client shows
// for a position.
//
// The position is a playback time t in [0, total] (seconds at 1x).  The
// event at t and the fraction of it played are derived from t, so every way
// of reaching a position (playing, stepping, seeking either way) gives the
// same state.
#ifndef RV_PLAYBACK_HPP
#define RV_PLAYBACK_HPP

#include "timeline.hpp"

#include <cstdint>
#include <string>

namespace rv {

class Trace;

// Speeds offered, slowest first; values of rv_set_speed snap to these.
constexpr double kSpeeds[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};
constexpr int kSpeedCount = 9;
constexpr int kDefaultSpeed = 2; // 1x

struct Player {
    double t = 0;
    bool playing = false;
    int speed = kDefaultSpeed; // index into kSpeeds
    int direction = 1;         // +1 forward, -1 backward
    double speed_value() const { return kSpeeds[speed]; }
};

// Step kinds; values match RV_STEP_* in rv.h.
enum class StepKind : std::uint32_t { Event = 0, Search, Commit, Net, RipUp, Failure, Phase };
constexpr std::uint32_t kStepKindCount = 7;
const char *step_kind_name(std::uint32_t kind) noexcept;
bool step_matches(const Event &e, StepKind kind);

// The event a step from playback time t lands on: forward, the first
// matching event with time starting after t; backward, the last matching
// event with time starting before t.  kNone32 if there is none.
std::uint32_t step_target(const Timeline &tl, double t, StepKind kind, int direction);

// Fraction of event e played at time t, in [0, 1].
double event_fraction(const Timeline &tl, std::uint32_t e, double t);

// A search event plays in two parts.  Over the first kRevealEnd of its
// time the wavefront's cells appear in expansion order, linearly: at
// fraction f, floor(n * f / kRevealEnd) of its n stored entries.  For the
// rest it is "settled": every cell is shown, with the path if one was found.
constexpr double kRevealEnd = 0.8;
std::uint32_t revealed_count(std::uint32_t n, double fraction);

// The search whose stored arrays are event e's wavefront (repeat_of
// resolved), kNone32 if e is not a search or has no stored wavefront.
std::uint32_t wavefront_source(const Trace &tr, const Timeline &tl, std::uint32_t e);

// Wavefront cells revealed at time t of a search event.
std::uint32_t cells_revealed(const Trace &tr, const Timeline &tl, std::uint32_t e, double t);

// Is event e a search in its settled part at time t?
bool search_settled(const Timeline &tl, std::uint32_t e, double t);

// One line about the position: phase, route_net, what the event does,
// counts so far and router time.
std::string caption(const Trace &tr, const Timeline &tl, std::uint32_t e, std::uint32_t failures_shown);
// "event 1234 of 9000 · 1:23.4 / 3:00.0"
std::string position_text(const Timeline &tl, std::uint32_t e, double t);
// "1×", "0.25×"
std::string speed_text(double speed);
// "fit length 3:00", "one net: /CLK, 0:41", "real time ×10, 4:47" (the
// length of the run at 1x after the comma).
std::string pacing_text(const PacingSettings &p, const Trace *tr, double total);

} // namespace rv

#endif // RV_PLAYBACK_HPP

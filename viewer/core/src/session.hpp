// session.hpp: the C++ object behind an rv_session handle.
//
// Portable C++17, standard library only.  The trace reader; since M2 the
// scene (static display list, visibility, camera); since M3 the timeline,
// the playback clock and the overlay.  The C boundary in api.cpp stays a
// thin translation layer.
#ifndef RV_SESSION_HPP
#define RV_SESSION_HPP

#include "hittest.hpp"
#include "inspect.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "routed.hpp"
#include "rv.h"
#include "scene.hpp"
#include "timeline.hpp"
#include "trace.hpp"
#include "wavefront.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rv {

class Session {
public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Message for the most recent failure, "" after a successful call.
    // Never null.
    const char *last_error() const noexcept {
        return fallback_ != nullptr ? fallback_ : last_error_.c_str();
    }

    // Record a failure.  Never throws: if the message cannot be stored, a
    // static message is reported instead.
    void set_error(const char *message) noexcept {
        try {
            last_error_.assign(message != nullptr ? message : "unknown error");
            fallback_ = nullptr;
        } catch (...) {
            last_error_.clear();
            fallback_ = "out of memory while recording an error";
        }
    }

    void clear_error() noexcept {
        last_error_.clear();
        fallback_ = nullptr;
    }

    // --- the open trace -------------------------------------------------

    // Replace the open trace (on success only: a failed open throws before
    // anything changes).  M6: wait opens a path with no file yet (waiting
    // for it); live paces the trace as growing while it grows (prefix
    // pacing) and follows it (see follow_step) when it is not complete.
    void open(const std::string &path, bool wait = false, bool live = false) {
        std::unique_ptr<Trace> t = Trace::open_file(path, wait);
        drop_borrowed();
        trace_ = std::move(t);
        path_ = path;
        open_wait_ = wait;
        live_ = live;
        scene.reset();
        player = Player();
        timeline_.reset();
        waves.reset();
        picker.reset();
        select_board_.reset();
        have_clock_ = false;
        growth_now_ = poll_now_ = 0;
        following_ = false;
        skips_ = 0;
        rebuild_timeline(true);
        span_ = live_paced() ? std::max(1.0, timeline_->total() * kSpanGrowth) : timeline_->total();
        if (live_paced()) set_following(true);
    }

    // M6: open the path of the last open again with its flags, keeping the
    // camera, the visibility, the pacing, the speed and the selection (a
    // replaced file).
    void reload() {
        if (path_.empty()) throw std::invalid_argument("invalid argument: no trace was opened in this session");
        const int speed = player.speed;
        const std::string path = path_;
        open(path, open_wait_, live_);
        player.speed = speed;
        if (selection_.active && selection_.has_net) select_net(selection_.net);
    }

    void close() noexcept {
        drop_borrowed();
        trace_.reset();
        scene.reset();
        timeline_.reset();
        geometry_.clear();
        overlay.reset();
        waves.reset();
        picker.reset();
        nets.clear();
        select_board_.reset();
        player = Player();
        following_ = false;
        ++timeline_generation_;
        sig_valid_ = false;
    }

    // --- pacing ---------------------------------------------------------------

    const PacingSettings &pacing() const { return pacing_; }
    // New pacing settings (validated by the caller).  The events get new
    // times; the position stays in the same event at the same fraction (at
    // its start if it gets no time now, which shows the next event with
    // time), or at the end if it was there.
    void set_pacing(const PacingSettings &p);

    // Read appended chunks and extend the timeline (M6: incrementally),
    // keeping the position (refresh_trace in session.cpp).  poll does the
    // same with the client's clock `now` (seconds, any origin), which tells
    // how long the file has not grown.
    std::uint32_t refresh() { return refresh_trace(nullptr); }
    std::uint32_t poll(double now) { return refresh_trace(&now); }

    // --- M6: following a growing trace ---------------------------------------

    // Is the trace still growing (no END, not stopped, not failed)?
    bool growing() const { return trace_ && !trace_->complete() && !trace_->stopped() && !trace_->failed(); }
    // Paced as a growing trace: opened live and still growing.
    bool live_paced() const { return live_ && growing(); }
    bool live() const { return live_; }
    bool following() const { return following_; }
    // Following on: to the end, playing forward, pulled along by new events
    // (follow_step).  Off: pause.  On a trace that is not growing or was not opened live,
    // on only moves the position to the end.
    void set_following(bool on);
    // Leave following (scrubbing, seeking, stepping): pause.
    void stop_following() {
        if (!following_) return;
        following_ = false;
        player.playing = false;
    }
    // Seeking (the rv_seek_* calls): leaves following.
    void seek(double t) {
        stop_following();
        set_time(t);
    }
    // The width of the timeline axis a client shows (rv_timeline.span).
    double span() const { return span_; }

    struct LiveInfo {
        std::uint32_t state = 0; // RV_LIVE_*
        double idle = 0;         // client seconds since the file last grew
        double lag = 0;          // router seconds between the position and the newest chunk
        std::uint32_t skips = 0; // times following jumped ahead
    };
    LiveInfo live_info() const;
    std::string live_text() const;

    bool has_trace() const noexcept { return trace_ != nullptr; }

    // The open trace; throws std::invalid_argument if there is none.
    Trace &trace() {
        if (!trace_) throw std::invalid_argument("no trace is open in this session");
        return *trace_;
    }

    // The open trace, or nullptr.
    Trace *trace_or_null() noexcept { return trace_.get(); }

    // The timeline, or nullptr with no trace open.
    const Timeline *timeline() const noexcept { return timeline_.get(); }
    std::uint32_t timeline_generation() const { return timeline_generation_; }
    const std::vector<CopperGeom> &geometry() const { return geometry_; }

    // --- position -----------------------------------------------------------

    // The current event (kNone32 without events).
    std::uint32_t event() const { return timeline_ ? timeline_->event_at(player.t) : kNone32; }
    double total() const { return timeline_ ? timeline_->total() : 0.0; }

    // A snapshot of what a client has drawn, to report changes.
    struct Mark {
        double t = 0;
        std::uint32_t board = kNone32, outline = kNone32;
        std::uint64_t overlay = 0, wave = 0, selection = 0;
        std::uint32_t timeline = 0;
        std::uint32_t lists = 0;  // M6: scene lists built (an outline rebuilt as the trace grew)
        bool following = false, playing = false;
    };
    Mark mark() {
        sync();
        Mark m{player.t, scene.effective_board(trace_.get()), scene.effective_outline(trace_.get()), sig_count_,
               wave_count_, selection_serial_, timeline_generation_};
        m.lists = scene.builds();
        m.following = following_;
        m.playing = player.playing;
        return m;
    }
    std::uint32_t changed_since(const Mark &m) {
        const Mark now = mark();
        std::uint32_t f = 0;
        if (now.t != m.t) f |= RV_CHANGED_POSITION;
        if (now.board != m.board || now.outline != m.outline || now.lists != m.lists) f |= RV_CHANGED_BOARD;
        if (now.following != m.following || now.playing != m.playing) f |= RV_CHANGED_LIVE;
        if (now.overlay != m.overlay) f |= RV_CHANGED_OVERLAY;
        if (now.wave != m.wave) f |= RV_CHANGED_WAVEFRONT;
        if (now.selection != m.selection) f |= RV_CHANGED_SELECTION;
        if (now.timeline != m.timeline) f |= RV_CHANGED_TIMELINE;
        return f;
    }

    void set_time(double t) {
        const double total_t = total();
        player.t = t < 0 ? 0 : (t > total_t ? total_t : t);
        sync();
    }

    // Advance the clock by dt seconds of wall time (see rv_advance and, M6,
    // follow_step).
    void advance(double dt) {
        if (!player.playing || !timeline_) return;
        if (following_) {
            follow_step(dt);
            return;
        }
        if (timeline_->size() == 0) return;
        const double t = player.t + dt * player.speed_value() * player.direction;
        if (player.direction > 0 && t >= total()) {
            set_time(total());
            // Playing into the end of a growing trace follows it (M6).
            if (live_paced()) following_ = true;
            else player.playing = false;
        } else if (player.direction < 0 && t <= 0) {
            set_time(0);
            player.playing = false;
        } else {
            set_time(t);
        }
    }

    void play(bool on) {
        if (!on) {
            following_ = false;
            player.playing = false;
            return;
        }
        if (!timeline_) return;
        if (player.direction > 0 && player.t >= total() && live_paced()) {
            set_following(true); // M6: play at the end of a growing trace: follow it
            return;
        }
        if (timeline_->size() == 0) {
            player.playing = false;
            return;
        }
        if (player.direction > 0 && player.t >= total()) set_time(0);
        else if (player.direction < 0 && player.t <= 0) set_time(total());
        player.playing = true;
    }

    void set_direction(int direction) {
        player.direction = direction > 0 ? 1 : -1;
        if (player.direction < 0) following_ = false;
    }

    // Bring the scene's board choice and the overlay signature up to date.
    void sync();

    // The overlay at the current position, built if needed.
    const Overlay &overlay_now();

    // The wavefront images at the current position, brought up to date.
    const WavefrontImages &wavefront_now();

    // Failed pads, ghosts and copper at the current position (for
    // rv_position and the caption).
    std::uint32_t failures_now() const;

    // Is the routed copper hidden because the board drawn holds it?
    bool routed_hidden() const;

    // --- M5: selection, nets table, picking ----------------------------------

    // The selection: nothing, a router net (with its name, if NETS names
    // it), or a name alone (a board net that is no router net).
    struct Selection {
        bool active = false;
        bool has_net = false;
        std::int32_t net = 0;
        std::string name; // "" for an unnamed router net: then no board item matches
        bool operator==(const Selection &o) const {
            return active == o.active && has_net == o.has_net && net == o.net && name == o.name;
        }
    };
    const Selection &selection() const { return selection_; }
    // Change the selection; in "one net" pacing a selected router net
    // becomes the paced net.
    void select(const Selection &s);
    void select_net(std::int32_t net);
    void select_name(const std::string &name); // "" clears
    // Board net codes of the board drawn whose name is the selected name.
    std::vector<std::int32_t> selected_board_nets();
    // Board items (pads, zone fills, tracks, vias) of the selection on the board drawn.
    std::uint32_t selected_board_items();

    // The nets table, built on first use, values brought to the position.
    NetTable &nets_now();

    // What is under view point (vx, vy), topmost first.
    std::vector<Hit> pick(double vx, double vy);

    // Board view state: selection, visibility, theme, camera, display list.
    Scene scene;
    Player player;
    Overlay overlay;
    WavefrontImages waves;
    Picker picker;
    NetTable nets;

    // Buffers that back borrowed pointers handed out through rv.h.
    std::string text;                              // rv_chunk_text
    std::string status;                            // rv_view_status
    std::string text_out;                          // rv_text
    std::shared_ptr<const Wavefront> wavefront;    // rv_wavefront
    std::vector<rv_batch> c_batches;               // rv_get_display_list
    std::uint32_t c_batches_generation = 0xFFFFFFFFu;
    std::vector<rv_batch> c_overlay;               // rv_get_overlay
    std::vector<rv_slot> c_slots;
    std::uint32_t c_overlay_generation = 0xFFFFFFFFu;
    std::uint32_t c_overlay_epoch = 0xFFFFFFFFu;
    std::vector<rv_event> c_events;                // rv_get_timeline
    std::uint32_t c_events_generation = 0xFFFFFFFFu;
    std::string cell_text;                         // rv_nets_cell
    std::vector<Hit> hits;                         // rv_hit_test
    std::string hover;                             // rv_hover_status
    std::vector<HistoryEntry> history;             // rv_net_history
    std::vector<std::uint32_t> net_event_list;     // rv_net_events

    void drop_borrowed() noexcept {
        text.clear();
        status.clear();
        text_out.clear();
        wavefront.reset();
        cell_text.clear();
        hover.clear();
    }

    // M6, following: constants of the rule (see rv.h, "Following").
    static constexpr double kFollowLag = 2.0;     // router seconds behind before jumping
    static constexpr double kCatchUp = 0.5;       // wall seconds to close the gap
    static constexpr double kFollowSettled = 0.9; // fraction of a search shown at least
    static constexpr double kStallSeconds = 5.0;  // idle this long: "stalled"
    static constexpr double kSpanGrowth = 1.5;

private:
    void rebuild_timeline(bool to_end);
    std::uint32_t refresh_trace(const double *now);
    void follow_step(double dt);
    // Put the position at fraction frac of event e (just inside it at its
    // end, so that the event stays the current one).
    void keep_position(std::uint32_t e, double frac);

    struct Signature {
        std::uint32_t timeline = 0, event = kNone32, visibility = 0, board = kNone32, outline = kNone32;
        bool hidden = false, settled = false;
        std::vector<std::pair<std::uint32_t, int>> ghosts;
        std::uint64_t selection = 0; // M5: selection_serial_
        bool operator==(const Signature &o) const {
            return timeline == o.timeline && event == o.event && visibility == o.visibility && board == o.board &&
                   outline == o.outline && hidden == o.hidden && settled == o.settled && ghosts == o.ghosts &&
                   selection == o.selection;
        }
    };
    // What the wavefront images show (wavefront.hpp): a function of these.
    struct WaveSignature {
        std::uint32_t timeline = 0, session = kNone32, source = kNone32, search = kNone32, count = 0, visibility = 0;
        bool shown = false;
        bool operator==(const WaveSignature &o) const {
            return timeline == o.timeline && session == o.session && source == o.source && search == o.search &&
                   count == o.count && visibility == o.visibility && shown == o.shown;
        }
    };

    std::string last_error_;
    const char *fallback_ = nullptr;
    std::unique_ptr<Trace> trace_;
    std::unique_ptr<Timeline> timeline_;
    std::vector<CopperGeom> geometry_;
    std::uint32_t timeline_generation_ = 0;
    Signature sig_;
    bool sig_valid_ = false;
    std::uint64_t sig_count_ = 0;   // bumps when the overlay signature changes
    std::uint64_t built_sig_ = ~0ull; // sig_count_ the overlay was built for
    WaveSignature wave_sig_;
    bool wave_sig_valid_ = false;
    std::uint64_t wave_count_ = 0;     // bumps when the wavefront signature changes
    std::uint64_t built_wave_ = ~0ull; // wave_count_ the images were brought to
    std::uint64_t built_wave_overlay_ = ~0ull; // sig_count_ then (the board's layer table may differ)
    PacingSettings pacing_;
    // M5
    Selection selection_;
    std::uint64_t selection_serial_ = 0;
    // The board's copper of the selected name: built for (board, serial).
    std::shared_ptr<const DisplayList> select_board_;
    std::uint32_t select_board_for_ = kNone32;
    std::uint64_t select_board_serial_ = ~0ull;
    std::uint32_t select_build_ = 0;
    const DisplayList *selected_board_list();
    // M6
    std::string path_;
    bool open_wait_ = false, live_ = false;
    bool following_ = false;
    bool have_clock_ = false;
    double poll_now_ = 0, growth_now_ = 0;
    double span_ = 0;
    std::uint32_t skips_ = 0;

public:
    // rv_get_timeline: leading entries of c_events still valid, and the
    // largest search size they were scaled with.
    std::uint32_t c_events_valid = 0;
    double c_events_lmax = -1;
};

} // namespace rv

#endif // RV_SESSION_HPP

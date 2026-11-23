// session.hpp: the C++ object behind an rv_session handle.
//
// Portable C++17, standard library only.  The trace reader; since M2 the
// scene (static display list, visibility, camera); since M3 the timeline,
// the playback clock and the overlay.  The C boundary in api.cpp stays a
// thin translation layer.
#ifndef RV_SESSION_HPP
#define RV_SESSION_HPP

#include "overlay.hpp"
#include "playback.hpp"
#include "routed.hpp"
#include "rv.h"
#include "scene.hpp"
#include "timeline.hpp"
#include "trace.hpp"

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
    // anything changes).
    void open(const std::string &path) {
        std::unique_ptr<Trace> t = Trace::open_file(path);
        drop_borrowed();
        trace_ = std::move(t);
        scene.reset();
        player = Player();
        timeline_.reset();
        rebuild_timeline(true);
    }

    void close() noexcept {
        drop_borrowed();
        trace_.reset();
        scene.reset();
        timeline_.reset();
        geometry_.clear();
        overlay.reset();
        player = Player();
        ++timeline_generation_;
        sig_valid_ = false;
    }

    // Read appended chunks and rebuild the timeline, keeping the position.
    std::uint32_t refresh() {
        Trace &t = trace();
        drop_borrowed();
        const std::uint32_t n = t.refresh();
        if (n > 0) rebuild_timeline(false);
        return n;
    }

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
        std::uint64_t overlay = 0;
    };
    Mark mark() {
        sync();
        return Mark{player.t, scene.effective_board(trace_.get()), scene.effective_outline(trace_.get()), sig_count_};
    }
    std::uint32_t changed_since(const Mark &m) {
        const Mark now = mark();
        std::uint32_t f = 0;
        if (now.t != m.t) f |= RV_CHANGED_POSITION;
        if (now.board != m.board || now.outline != m.outline) f |= RV_CHANGED_BOARD;
        if (now.overlay != m.overlay) f |= RV_CHANGED_OVERLAY;
        return f;
    }

    void set_time(double t) {
        const double total_t = total();
        player.t = t < 0 ? 0 : (t > total_t ? total_t : t);
        sync();
    }

    // Advance the clock by dt seconds of wall time (see rv_advance).
    void advance(double dt) {
        if (!player.playing || !timeline_ || timeline_->size() == 0) return;
        const double t = player.t + dt * player.speed_value() * player.direction;
        if (player.direction > 0 && t >= total()) {
            set_time(total());
            player.playing = false;
        } else if (player.direction < 0 && t <= 0) {
            set_time(0);
            player.playing = false;
        } else {
            set_time(t);
        }
    }

    void play(bool on) {
        if (!on || !timeline_ || timeline_->size() == 0) {
            player.playing = false;
            return;
        }
        if (player.direction > 0 && player.t >= total()) set_time(0);
        else if (player.direction < 0 && player.t <= 0) set_time(total());
        player.playing = true;
    }

    // Bring the scene's board choice and the overlay signature up to date.
    void sync();

    // The overlay at the current position, built if needed.
    const Overlay &overlay_now();

    // Failed pads, ghosts and copper at the current position (for
    // rv_position and the caption).
    std::uint32_t failures_now() const;

    // Is the routed copper hidden because the board drawn holds it?
    bool routed_hidden() const;

    // Board view state: selection, visibility, theme, camera, display list.
    Scene scene;
    Player player;
    Overlay overlay;

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

    void drop_borrowed() noexcept {
        text.clear();
        status.clear();
        text_out.clear();
        wavefront.reset();
    }

private:
    void rebuild_timeline(bool to_end);

    struct Signature {
        std::uint32_t timeline = 0, event = kNone32, visibility = 0, board = kNone32, outline = kNone32;
        bool hidden = false;
        std::vector<std::pair<std::uint32_t, int>> ghosts;
        bool operator==(const Signature &o) const {
            return timeline == o.timeline && event == o.event && visibility == o.visibility && board == o.board &&
                   outline == o.outline && hidden == o.hidden && ghosts == o.ghosts;
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
};

} // namespace rv

#endif // RV_SESSION_HPP

// session.cpp: timeline rebuilds, position sync and the overlay (session.hpp).

#include "session.hpp"

#include <algorithm>

namespace rv {

void Session::rebuild_timeline(bool to_end) {
    bool at_end = to_end || !timeline_ || timeline_->size() == 0 || player.t >= timeline_->total();
    std::uint32_t keep_chunk = kNone32;
    double frac = 0;
    if (!at_end) {
        const std::uint32_t e = event();
        keep_chunk = timeline_->events()[e].chunk;
        frac = event_fraction(*timeline_, e, player.t);
    }
    timeline_.reset();
    geometry_.clear();
    timeline_.reset(new Timeline(*trace_, FitLengthPacing()));
    geometry_ = build_copper_geometry(*trace_);
    overlay.reset();
    ++timeline_generation_;
    sig_valid_ = false;
    if (at_end || timeline_->size() == 0) {
        player.t = timeline_->total();
    } else {
        const std::uint32_t e = timeline_->event_of_chunk(keep_chunk);
        const Event &ev = timeline_->events()[e];
        player.t = std::min(timeline_->total(), ev.start + frac * ev.duration);
    }
    sync();
}

bool Session::routed_hidden() const {
    if (!timeline_ || !trace_) return false;
    return timeline_->board_holds_routing(event(), scene.effective_board(trace_.get()));
}

std::uint32_t Session::failures_now() const {
    if (!timeline_) return 0;
    std::vector<std::uint32_t> f;
    timeline_->alive_failures(event(), f);
    return static_cast<std::uint32_t>(f.size());
}

void Session::sync() {
    Signature s;
    s.timeline = timeline_generation_;
    if (trace_ && timeline_) {
        const std::uint32_t e = event();
        std::uint32_t board = kNone32, outline = kNone32;
        if (e != kNone32) {
            board = timeline_->events()[e].board;
            if (board == kNone32) outline = timeline_->events()[e].session;
        }
        scene.set_follow(board, outline);
        s.event = e;
        s.visibility = scene.visibility_generation();
        s.board = scene.effective_board(trace_.get());
        s.outline = scene.effective_outline(trace_.get());
        s.hidden = routed_hidden();
        if (e != kNone32 && !s.hidden && scene.class_visible(kClassRouted)) {
            std::vector<std::pair<std::uint32_t, double>> gh;
            timeline_->ghosts(player.t, gh);
            for (const auto &g : gh) s.ghosts.emplace_back(g.first, ghost_level(g.second));
        }
    } else {
        scene.set_follow(kNone32, kNone32);
    }
    if (!sig_valid_ || !(s == sig_)) {
        sig_ = std::move(s);
        sig_valid_ = true;
        ++sig_count_;
    }
}

const Overlay &Session::overlay_now() {
    sync();
    if (built_sig_ != sig_count_) {
        OverlayInput in;
        if (trace_ && timeline_) {
            in.trace = trace_.get();
            in.timeline = timeline_.get();
            in.geometry = &geometry_;
            in.board = &scene.full(trace_.get());
            in.theme = &scene.theme();
            in.routed_visible = scene.class_visible(kClassRouted);
            for (const Run &r : trace_->runs())
                for (const std::string &n : r.layer_name) in.layer_visible.emplace_back(n, scene.layer_visible(n));
            in.time = player.t;
            in.event = event();
            in.suppress_routed = sig_.hidden;
        }
        overlay.build(in);
        built_sig_ = sig_count_;
    }
    return overlay;
}

} // namespace rv

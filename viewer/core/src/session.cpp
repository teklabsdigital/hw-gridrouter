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
    timeline_.reset(new Timeline(*trace_, *make_pacing(pacing_)));
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

void Session::set_pacing(const PacingSettings &p) {
    if (p == pacing_) return;
    pacing_ = p;
    if (!timeline_) return;
    const bool at_end = timeline_->size() == 0 || player.t >= timeline_->total();
    const std::uint32_t e = event();
    const double frac = at_end ? 0.0 : event_fraction(*timeline_, e, player.t);
    timeline_->repace(*make_pacing(pacing_));
    ++timeline_generation_;
    sig_valid_ = false;
    if (at_end) {
        player.t = timeline_->total();
    } else {
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
        s.settled = search_settled(*timeline_, e, player.t);
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

    WaveSignature w;
    w.timeline = timeline_generation_;
    w.visibility = scene.visibility_generation();
    if (trace_ && timeline_) {
        const std::uint32_t e = event();
        if (e != kNone32) {
            w.session = timeline_->events()[e].session;
            w.shown = scene.class_visible(kClassWavefront);
            if (w.shown) {
                w.source = wavefront_source(*trace_, *timeline_, e);
                if (w.source != kNone32) {
                    w.search = timeline_->events()[e].record;
                    w.count = revealed_count(trace_->searches()[w.source].ntrace, event_fraction(*timeline_, e, player.t));
                }
            }
        } else if (!trace_->runs().empty()) {
            w.session = static_cast<std::uint32_t>(trace_->runs().size() - 1);
        }
    }
    if (!wave_sig_valid_ || !(w == wave_sig_)) {
        wave_sig_ = w;
        wave_sig_valid_ = true;
        ++wave_count_;
    }
}

const WavefrontImages &Session::wavefront_now() {
    sync();
    if (built_wave_ == wave_count_ && built_wave_overlay_ == sig_count_) return waves;
    WaveRequest r;
    if (trace_ && timeline_ && wave_sig_.session != kNone32) {
        r.session = wave_sig_.session;
        r.source = wave_sig_.source;
        r.search = wave_sig_.search;
        r.count = wave_sig_.count;
        std::vector<std::pair<std::string, bool>> vis;
        for (const Run &run : trace_->runs())
            for (const std::string &n : run.layer_name) vis.emplace_back(n, scene.layer_visible(n));
        RouterLayer rl[2];
        const int nl = router_layers(*trace_, r.session, &scene.full(trace_.get()), scene.theme(), vis, rl);
        r.layers = nl == 1 ? 1 : 2;
        for (int k = 0; k < 2; ++k) {
            r.visible[k] = rl[k].visible && wave_sig_.shown;
            r.color[k] = rl[k].color;
            r.order[k] = order_key(rl[k].rank, sub::kWavefront);
            r.board_layer[k] = rl[k].index;
        }
        if (nl == 0) r.session = kNone32;
    }
    // Marked built before updating: unreadable arrays are reported once (the
    // images are then consistent and empty), not again on every frame.
    built_wave_ = wave_count_;
    built_wave_overlay_ = sig_count_;
    if (trace_) waves.update(*trace_, r);
    else if (waves.image(0).width != 0) waves.reset();
    return waves;
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
            in.settled = sig_.settled;
        }
        overlay.build(in);
        built_sig_ = sig_count_;
    }
    return overlay;
}

} // namespace rv

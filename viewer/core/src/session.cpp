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
    nets.clear();
    picker.reset();
    select_board_.reset();
    select_board_serial_ = ~0ull;
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
    s.selection = selection_serial_;
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
            if (selection_.active) {
                in.select_active = true;
                in.select_has_net = selection_.has_net;
                in.select_net = selection_.net;
                in.select_board = selected_board_list();
                in.select_build = select_build_;
                const DisplayList &full = scene.full(trace_.get());
                for (const LayerEntry &le : full.layers) {
                    if (in.board_layer_visible.size() <= le.index) in.board_layer_visible.resize(le.index + 1, false);
                    in.board_layer_visible[le.index] = scene.layer_visible(le.name);
                }
                for (std::uint32_t c = 0; c < kClassCount; ++c)
                    if (scene.class_visible(c)) in.classes |= 1u << c;
                // The dim rectangle: everything drawn (the board and the
                // router grid of the position's session), with a margin.
                float r[4] = {1e30f, 1e30f, -1e30f, -1e30f};
                auto unite = [&](float x0, float y0, float x1, float y1) {
                    r[0] = std::min(r[0], x0);
                    r[1] = std::min(r[1], y0);
                    r[2] = std::max(r[2], x1);
                    r[3] = std::max(r[3], y1);
                };
                if (full.has_extent) unite(full.content[0], full.content[1], full.content[2], full.content[3]);
                const std::uint32_t e = event();
                std::uint32_t sess = e != kNone32 ? timeline_->events()[e].session : kNone32;
                if (sess == kNone32 && !trace_->runs().empty()) sess = static_cast<std::uint32_t>(trace_->runs().size() - 1);
                if (sess < trace_->runs().size() && trace_->runs()[sess].valid) {
                    const Run &run = trace_->runs()[sess];
                    const Frame fr(run);
                    unite(static_cast<float>(fr.cx(0) - run.cell), static_cast<float>(fr.cy(0) - run.cell),
                          static_cast<float>(fr.cx(run.nx - 1) + run.cell), static_cast<float>(fr.cy(run.ny - 1) + run.cell));
                }
                if (r[0] <= r[2]) in.dim_rect = {{r[0] - 2.0f, r[1] - 2.0f, r[2] + 2.0f, r[3] + 2.0f}};
            }
        }
        overlay.build(in);
        built_sig_ = sig_count_;
    }
    return overlay;
}

// --- M5: selection ----------------------------------------------------------------

void Session::select(const Selection &sel) {
    Selection s = sel;
    if (!s.active) s = Selection();
    if (s == selection_) return;
    selection_ = s;
    ++selection_serial_;
    if (s.active && s.has_net && pacing_.mode == PacingMode::OneNet && pacing_.net != s.net) {
        PacingSettings p = pacing_;
        p.net = s.net;
        set_pacing(p);
    }
    sync();
}

void Session::select_net(std::int32_t net) {
    Selection s;
    s.active = true;
    s.has_net = true;
    s.net = net;
    if (trace_) {
        auto it = trace_->net_names().find(net);
        if (it != trace_->net_names().end()) s.name = it->second;
    }
    select(s);
}

void Session::select_name(const std::string &name) {
    Selection s;
    if (!name.empty()) {
        s.active = true;
        s.name = name;
        // A router net of that name: the lowest code that has it.
        if (trace_)
            for (const auto &kv : trace_->net_names())
                if (kv.second == name) {
                    s.has_net = true;
                    s.net = kv.first;
                    break;
                }
    }
    select(s);
}

std::vector<std::int32_t> Session::selected_board_nets() {
    std::vector<std::int32_t> out;
    if (!trace_ || !selection_.active || selection_.name.empty()) return out;
    const std::uint32_t b = scene.effective_board(trace_.get());
    if (b == kNone32) return out;
    for (const BoardNet &n : trace_->boards()[b].nets)
        if (n.name == selection_.name) out.push_back(n.code);
    return out;
}

const DisplayList *Session::selected_board_list() {
    if (!trace_) return nullptr;
    const std::uint32_t b = scene.effective_board(trace_.get());
    if (b != select_board_for_ || selection_serial_ != select_board_serial_) {
        select_board_.reset();
        select_board_for_ = b;
        select_board_serial_ = selection_serial_;
        ++select_build_;
        const std::vector<std::int32_t> codes = selected_board_nets();
        if (!codes.empty()) {
            std::shared_ptr<const BoardGeometry> geo = trace_->board_geometry(b);
            select_board_ = std::make_shared<const DisplayList>(
                build_net_list(trace_->boards()[b], *geo, scene.theme(), codes));
        }
    }
    return select_board_.get();
}

std::uint32_t Session::selected_board_items() {
    const std::vector<std::int32_t> codes = selected_board_nets();
    if (codes.empty()) return 0;
    const std::uint32_t b = scene.effective_board(trace_.get());
    std::shared_ptr<const BoardGeometry> geo = trace_->board_geometry(b);
    const DisplayList &full = scene.full(trace_.get());
    std::vector<bool> copper;
    for (const LayerEntry &le : full.layers) {
        if (copper.size() <= le.index) copper.resize(le.index + 1, false);
        copper[le.index] = le.cls.kind == LayerKind::Copper;
    }
    auto has = [&](std::int32_t n) { return std::find(codes.begin(), codes.end(), n) != codes.end(); };
    std::uint32_t n = 0;
    for (const Polyset &p : geo->polysets) {
        const bool cu = p.layer_index >= 0 && static_cast<std::size_t>(p.layer_index) < copper.size() &&
                        copper[static_cast<std::size_t>(p.layer_index)];
        if (((p.role == 0 && cu) || p.role == 3) && has(p.net)) ++n;
    }
    for (const BoardTrack &t : geo->tracks)
        if (has(t.net)) ++n;
    for (const BoardVia &v : geo->vias)
        if (has(v.net)) ++n;
    return n;
}

NetTable &Session::nets_now() {
    if (trace_ && timeline_ && !nets.built()) nets.build(*trace_, *timeline_);
    nets.update(event());
    return nets;
}

std::vector<Hit> Session::pick(double vx, double vy) {
    sync();
    PickInput in;
    if (!trace_ || !timeline_) return {};
    in.trace = trace_.get();
    in.timeline = timeline_.get();
    in.geometry = &geometry_;
    in.event = event();
    const DisplayList &full = scene.full(trace_.get());
    in.board = scene.effective_board(trace_.get());
    in.board_list = in.board != kNone32 ? &full : nullptr;
    for (const LayerEntry &le : full.layers) {
        if (in.layer_visible.size() <= le.index) in.layer_visible.resize(le.index + 1, false);
        in.layer_visible[le.index] = scene.layer_visible(le.name);
    }
    for (std::uint32_t c = 0; c < kClassCount; ++c)
        if (scene.class_visible(c)) in.classes |= 1u << c;
    in.routed_shown = scene.class_visible(kClassRouted) && !sig_.hidden;
    const std::uint32_t e = in.event;
    if (e != kNone32) {
        const std::uint32_t sess = timeline_->events()[e].session;
        std::vector<std::pair<std::string, bool>> vis;
        for (const Run &r : trace_->runs())
            for (const std::string &n : r.layer_name) vis.emplace_back(n, scene.layer_visible(n));
        in.router_layer_count = router_layers(*trace_, sess, &full, scene.theme(), vis, in.rl);
        in.wave_shown = scene.class_visible(kClassWavefront);
        in.wave_source = wavefront_source(*trace_, *timeline_, e);
        if (in.wave_source != kNone32) {
            in.wave_search = timeline_->events()[e].record;
            in.wave_count = revealed_count(trace_->searches()[in.wave_source].ntrace,
                                           event_fraction(*timeline_, e, player.t));
        }
    }
    scene.camera.to_board(vx, vy, in.x, in.y);
    in.tol = kPickPoints / scene.camera.scale();
    return picker.pick(in);
}

} // namespace rv

// session.cpp: timeline rebuilds, position sync and the overlay (session.hpp).

#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    timeline_.reset(new Timeline(*trace_, *make_pacing(pacing_), live_paced()));
    geometry_ = build_copper_geometry(*trace_);
    overlay.reset();
    nets.clear();
    picker.reset();
    select_board_.reset();
    select_board_serial_ = ~0ull;
    ++timeline_generation_;
    c_events_valid = 0;
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

void Session::keep_position(std::uint32_t e, double frac) {
    const Timeline &tl = *timeline_;
    if (e >= tl.size()) {
        player.t = std::min(player.t, tl.total());
        return;
    }
    const Event &ev = tl.events()[e];
    double t = std::min(tl.total(), ev.start + frac * ev.duration);
    if (tl.event_at(t) != e && ev.duration > 0) {
        // At the event's end, which is now the next event's start: just
        // before it, so that the event stays the current one.
        t = std::max(ev.start, std::nextafter(ev.start + ev.duration, ev.start));
    }
    player.t = t;
}

std::uint32_t Session::refresh_trace(const double *now) {
    Trace &t = trace();
    drop_borrowed();
    if (now != nullptr && !have_clock_) {
        have_clock_ = true;
        growth_now_ = *now;
    }
    if (now != nullptr) poll_now_ = *now;
    const std::uint64_t size_before = t.file_size();
    const auto chunks_before = static_cast<std::uint32_t>(t.chunks().size());
    const bool paced_live = timeline_ && timeline_->live();
    std::uint32_t n = 0;
    try {
        n = t.refresh();
    } catch (...) {
        if (now != nullptr && t.file_size() > size_before) growth_now_ = *now;
        throw;
    }
    if (now != nullptr && t.file_size() > size_before) growth_now_ = *now;
    if (!timeline_ || (n == 0 && paced_live == live_paced())) return n;

    // The position: its event and the fraction of it played.
    const std::uint32_t e_cur = event();
    const bool at_end = timeline_->size() == 0 || player.t >= timeline_->total();
    const double frac = e_cur != kNone32 ? event_fraction(*timeline_, e_cur, player.t) : 0.0;

    const Timeline::Change c = timeline_->extend(*make_pacing(pacing_), live_paced());
    append_copper_geometry(t, geometry_);
    nets.extend(t, *timeline_);

    // Outlines of sessions that gained pads, claims, keep-outs or discs.
    const std::vector<Chunk> &ch = t.chunks();
    std::uint32_t last_session = kNone32;
    for (std::uint32_t k = chunks_before; k < ch.size(); ++k) {
        const Chunk &cc = ch[k];
        if ((cc.kind == Kind::Pad || cc.kind == Kind::Rect || cc.kind == Kind::Disc) && cc.session != last_session) {
            scene.invalidate_outline(cc.session);
            last_session = cc.session;
        }
    }
    // A selected router net gets the name NETS gives it.
    if (selection_.active && selection_.has_net) {
        auto it = t.net_names().find(selection_.net);
        const std::string name = it != t.net_names().end() ? it->second : std::string();
        if (name != selection_.name) {
            selection_.name = name;
            ++selection_serial_;
        }
    }
    ++timeline_generation_;
    c_events_valid = std::min(c_events_valid, c.first);
    sig_valid_ = false;

    // The current event never changes because of a refresh: events before
    // c.first kept their times; otherwise the same event at the same
    // fraction.  Following, the position stays where it is (follow_step
    // moves it), unless the trace was refitted.
    if (!live_ && at_end) {
        // Opened without RV_OPEN_LIVE (M5 behaviour): at the end stays at the end.
        player.t = timeline_->total();
    } else if (e_cur == kNone32) {
        // No event before: following plays from the start (jumping ahead if
        // far behind); otherwise the end, as when opened.
        player.t = following_ ? 0.0 : timeline_->total();
    } else if (c.repaced) {
        keep_position(e_cur, frac);
    } else if (e_cur >= c.first) {
        // Prefix pacing: the event was recomputed (it was the last, still
        // growing) but kept its start; the same time is still in it, exactly
        // (recomputing it from the fraction could move it by a rounding).
        if (!(player.t <= timeline_->total() && timeline_->event_at(player.t) == e_cur)) keep_position(e_cur, frac);
    }
    // The axis: the total once complete; while growing it grows by
    // kSpanGrowth when the total reaches it.
    if (!live_paced()) span_ = timeline_->total();
    else if (timeline_->total() > span_) span_ = timeline_->total() * kSpanGrowth;
    // Following ends at the end of a trace that is no longer growing.
    if (!live_paced() && following_ && player.t >= timeline_->total()) {
        following_ = false;
        player.playing = false;
    }
    if (scene.effective_outline(&t) != kNone32) scene.full(&t); // rebuilt now: reported as RV_CHANGED_BOARD
    sync();
    return n;
}

void Session::set_following(bool on) {
    if (!on) {
        following_ = false;
        player.playing = false;
        return;
    }
    if (!timeline_) return;
    if (!live_paced()) {
        following_ = false;
        set_time(total());
        return;
    }
    // To the newest event, then pulled along.
    following_ = true;
    player.playing = true;
    player.direction = 1;
    set_time(total());
}

// Following (rv.h, "Following"): play forward toward the newest event.
//   - Behind by more than kFollowLag router seconds (the newest chunk's time
//     minus the time of the current event's last chunk): jump to the start
//     of the first event within kFollowLag of the newest chunk.
//   - Else move at max(speed, gap / kCatchUp) playback seconds per second,
//     gap being the playback time to the end, so a gap closes in about
//     kCatchUp seconds; but stop at kFollowSettled of the first search with
//     time whose point kFollowSettled is still ahead (the current event
//     included): at most one new search per frame, and none is skipped:
//     each is shown with its wavefront settled.
//   - At the end: stay there, playing, while the trace grows; once it no
//     longer grows, stop following and pause.
void Session::follow_step(double dt) {
    const Timeline &tl = *timeline_;
    const double total_t = tl.total();
    if (tl.size() == 0 || player.t >= total_t) {
        if (!live_paced()) {
            following_ = false;
            player.playing = false;
        }
        if (tl.size() > 0) set_time(total_t);
        return;
    }
    const std::vector<Chunk> &ch = trace_->chunks();
    const double newest = ch.empty() ? 0.0 : ch.back().t;
    const std::uint32_t e = event();
    const double lag = newest - ch[tl.events()[e].last].t;
    if (lag > kFollowLag) {
        std::uint32_t k = tl.size() - 1;
        while (k > e + 1 && newest - ch[tl.events()[k - 1].last].t <= kFollowLag) --k;
        if (k > e) {
            ++skips_;
            set_time(tl.events()[k].start);
            return;
        }
    }
    const double v = std::max(player.speed_value(), (total_t - player.t) / kCatchUp);
    double t1 = std::min(total_t, player.t + v * dt);
    for (std::uint32_t k = e; k < tl.size() && tl.events()[k].start < t1; ++k) {
        const Event &ev = tl.events()[k];
        if (ev.kind != EvKind::Search || ev.duration <= 0) continue;
        const double settled = ev.start + kFollowSettled * ev.duration;
        if (!(settled > player.t)) continue; // the current search, shown settled already
        t1 = std::min(t1, settled);
        break;
    }
    set_time(std::max(player.t, t1));
    if (player.t >= total_t && !live_paced()) {
        following_ = false;
        player.playing = false;
    }
}

Session::LiveInfo Session::live_info() const {
    LiveInfo li;
    if (!trace_) return li;
    const Trace &t = *trace_;
    if (t.replaced()) li.state = RV_LIVE_REPLACED;
    else if (t.failed()) li.state = RV_LIVE_FAILED;
    else if (t.stopped()) li.state = RV_LIVE_STOPPED;
    else if (t.complete()) li.state = RV_LIVE_COMPLETE;
    else if (!t.header_complete()) li.state = RV_LIVE_WAITING;
    else li.state = RV_LIVE_GROWING;
    li.idle = have_clock_ ? std::max(0.0, poll_now_ - growth_now_) : 0.0;
    if (li.state == RV_LIVE_GROWING && li.idle >= kStallSeconds) li.state = RV_LIVE_STALLED;
    if (timeline_ && !t.chunks().empty()) {
        const std::uint32_t e = event();
        if (e != kNone32) li.lag = std::max(0.0, t.chunks().back().t - t.chunks()[timeline_->events()[e].last].t);
    }
    li.skips = skips_;
    return li;
}

namespace {
std::string megabytes(std::uint64_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1e6);
    return buf;
}
std::string grouped_count(std::uint64_t v) {
    std::string d = std::to_string(v), out;
    for (std::size_t k = 0; k < d.size(); ++k) {
        if (k > 0 && (d.size() - k) % 3 == 0) out += "\u2009";
        out += d[k];
    }
    return out;
}
} // namespace

std::string Session::live_text() const {
    if (!trace_) return "";
    const LiveInfo li = live_info();
    const Trace &t = *trace_;
    const std::string size = megabytes(t.file_size());
    const std::string events = grouped_count(timeline_ ? timeline_->size() : 0) + " events";
    char buf[160];
    switch (li.state) {
    case RV_LIVE_WAITING:
        return t.waiting() ? "Waiting for the file" : "Waiting for the recorder to start";
    case RV_LIVE_GROWING: {
        std::string s = "Live: growing, " + size + ", " + events;
        if (following_ && li.lag >= 0.5) {
            std::snprintf(buf, sizeof buf, ", %.1f s behind", li.lag);
            s += buf;
        }
        return s;
    }
    case RV_LIVE_STALLED:
        std::snprintf(buf, sizeof buf, "Live: stalled, no new data for %.0f s", std::floor(li.idle));
        return std::string(buf) + ", " + size + ", " + events;
    case RV_LIVE_COMPLETE:
        return "Complete: " + size + ", " + events;
    case RV_LIVE_STOPPED:
        return "Reading stopped at a corrupt chunk header: " + size + ", " + events;
    case RV_LIVE_REPLACED:
        return "Replaced: the file on disk is another run";
    case RV_LIVE_FAILED:
        return "Unreadable: " + t.failure();
    default: return "";
    }
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
    c_events_valid = 0;
    if (!live_paced()) span_ = timeline_->total();
    else span_ = std::max(span_, timeline_->total() * kSpanGrowth);
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

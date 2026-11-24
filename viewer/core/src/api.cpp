// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// api.cpp: the C boundary of the core.
//
// Each entry point translates between the C types of rv.h and the C++
// objects behind them.  No C++ exception crosses this boundary: every entry
// point catches everything and maps it to an rv_status, recording the
// message on the session when there is one.

#include "rv.h"

#include "errors.hpp"
#include "session.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>

struct rv_session {
    rv::Session impl;
};

namespace {

// Map the exception being handled to a status and record its message.
// Call only from inside a catch block.
rv_status status_from_current_exception(rv::Session *session) noexcept {
    rv_status status = RV_E_INTERNAL;
    const char *message = "unknown internal error";
    try {
        throw;
    } catch (const std::bad_alloc &) {
        status = RV_E_NO_MEMORY;
        message = "out of memory";
    } catch (const std::invalid_argument &e) {
        status = RV_E_INVALID_ARGUMENT;
        message = e.what();
    } catch (const rv::IoError &e) {
        status = RV_E_IO;
        message = e.what();
    } catch (const rv::ReplacedError &e) {
        status = RV_E_REPLACED;
        message = e.what();
    } catch (const rv::FormatError &e) {
        status = RV_E_FORMAT;
        message = e.what();
    } catch (const rv::CorruptChunk &e) {
        status = RV_E_FORMAT;
        message = e.what();
    } catch (const rv::UnsupportedVersion &e) {
        status = RV_E_UNSUPPORTED;
        message = e.what();
    } catch (const std::exception &e) {
        message = e.what();
    } catch (...) {
    }
    if (session != nullptr) session->set_error(message);
    return status;
}

const char kNullSession[] = "invalid argument: session is NULL";

// Run f(session) behind the boundary rules: NULL session rejected, every
// exception mapped to a status, the error cleared on success.
template <class F>
rv_status guarded(rv_session *session, F &&f) noexcept {
    if (session == nullptr) return RV_E_INVALID_ARGUMENT;
    try {
        f(session->impl);
        session->impl.clear_error();
        return RV_OK;
    } catch (...) {
        return status_from_current_exception(&session->impl);
    }
}

// The display list whose extent zoom-to-fit uses.
const rv::DisplayList &fit_list(rv::Session &s) {
    return s.scene.full(s.trace_or_null());
}

template <class T>
void require_out(const T *out, const char *name) {
    if (out == nullptr) throw std::invalid_argument(std::string("invalid argument: ") + name + " is NULL");
}

void require_index(std::size_t index, std::size_t count, const char *what) {
    if (index >= count)
        throw std::invalid_argument(std::string("invalid argument: no ") + what + " with index " +
                                    std::to_string(index) + " (there are " + std::to_string(count) + ")");
}

void require_finite(double v, const char *name) {
    if (!std::isfinite(v)) throw std::invalid_argument(std::string("invalid argument: ") + name + " is not finite");
}

// A call that may move the playback position: reports what changed.
template <class F>
rv_status playback(rv_session *session, uint32_t *changed, F &&f) noexcept {
    if (changed != nullptr) *changed = 0;
    return guarded(session, [&](rv::Session &s) {
        s.drop_borrowed();
        const rv::Session::Mark m = s.mark();
        f(s);
        const uint32_t c = s.changed_since(m);
        if (changed != nullptr) *changed = c;
    });
}

} // namespace

extern "C" {

uint32_t rv_api_version(void) {
    return RV_API_VERSION;
}

rv_status rv_create(rv_session **out) {
    if (out == nullptr) return RV_E_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        *out = new rv_session();
        return RV_OK;
    } catch (...) {
        // No session exists yet, so the status is the only report.
        return status_from_current_exception(nullptr);
    }
}

void rv_destroy(rv_session *session) {
    try {
        delete session;
    } catch (...) {
        // Destructors do not throw; this only keeps the boundary rule.
    }
}

const char *rv_last_error(const rv_session *session) {
    if (session == nullptr) return kNullSession;
    return session->impl.last_error();
}

// --- opening ---------------------------------------------------------------------

rv_status rv_open(rv_session *session, const char *path) {
    return rv_open_ex(session, path, 0);
}

rv_status rv_open_ex(rv_session *session, const char *path, uint32_t flags) {
    return guarded(session, [&](rv::Session &s) {
        require_out(path, "path");
        if ((flags & ~(RV_OPEN_WAIT | RV_OPEN_LIVE)) != 0)
            throw std::invalid_argument("invalid argument: unknown open flags " + std::to_string(flags));
        s.open(path, (flags & RV_OPEN_WAIT) != 0, (flags & RV_OPEN_LIVE) != 0);
    });
}

rv_status rv_reload(rv_session *session) {
    return guarded(session, [&](rv::Session &s) { s.reload(); });
}

rv_status rv_close(rv_session *session) {
    return guarded(session, [&](rv::Session &s) { s.close(); });
}

rv_status rv_refresh(rv_session *session, uint32_t *new_chunks) {
    if (new_chunks != nullptr) *new_chunks = 0;
    return guarded(session, [&](rv::Session &s) {
        const uint32_t n = s.refresh();
        if (new_chunks != nullptr) *new_chunks = n;
    });
}

rv_status rv_poll(rv_session *session, double now, uint32_t *new_chunks, uint32_t *changed) {
    if (new_chunks != nullptr) *new_chunks = 0;
    return playback(session, changed, [&](rv::Session &s) {
        require_finite(now, "now");
        const uint32_t n = s.poll(now);
        if (new_chunks != nullptr) *new_chunks = n;
    });
}

rv_status rv_live_get(rv_session *session, rv_live *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_live l;
        std::memset(&l, 0, sizeof l);
        const rv::Session::LiveInfo li = s.live_info();
        l.state = li.state;
        l.idle_seconds = li.idle;
        l.lag_seconds = li.lag;
        l.skips = li.skips;
        l.following = s.following() ? 1u : 0u;
        l.live = s.live() ? 1u : 0u;
        if (const rv::Trace *t = s.trace_or_null()) {
            l.file_size = t->file_size();
            l.indexed_end = t->indexed_end();
            l.chunks = static_cast<uint32_t>(t->chunks().size());
            l.events = s.timeline() != nullptr ? s.timeline()->size() : 0;
        }
        *out = l;
    });
}

rv_status rv_live_text(rv_session *session, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        s.text_out = s.live_text();
        *text = s.text_out.c_str();
    });
}

rv_status rv_set_following(rv_session *session, uint32_t on, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) { s.set_following(on != 0); });
}

// --- facts -----------------------------------------------------------------------

const char *rv_kind_tag(uint32_t kind) {
    if (kind >= static_cast<uint32_t>(rv::kKindCount)) return rv::kind_tag(rv::Kind::Unknown);
    return rv::kind_tag(static_cast<rv::Kind>(kind));
}

rv_status rv_info(rv_session *session, rv_trace_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace &t = s.trace();
        rv_trace_info i;
        std::memset(&i, 0, sizeof i);
        i.open_time = t.open_time();
        i.last_time = t.chunks().empty() ? 0.0 : t.chunks().back().t;
        i.file_size = t.file_size();
        i.indexed_end = t.indexed_end();
        i.expanded_total = t.expanded_total();
        i.trace_entries = t.trace_entries();
        i.header_complete = t.header_complete() ? 1u : 0u;
        i.version_major = t.major();
        i.version_minor = t.minor();
        i.complete = t.complete() ? 1u : 0u;
        i.stopped = t.stopped() ? 1u : 0u;
        i.failed = t.failed() ? 1u : 0u;
        i.chunk_count = static_cast<uint32_t>(t.chunks().size());
        i.issue_count = static_cast<uint32_t>(t.issues().size());
        i.error_count = t.error_count();
        i.warning_count = t.warning_count();
        i.run_count = static_cast<uint32_t>(t.runs().size());
        i.net_count = static_cast<uint32_t>(t.net_names().size());
        i.search_count = static_cast<uint32_t>(t.searches().size());
        i.route_count = static_cast<uint32_t>(t.routes().size());
        i.copper_count = static_cast<uint32_t>(t.copper().size());
        i.copper_removed = t.copper_removed();
        i.failed_pads = t.failed_pads();
        i.drops_failed = t.drops_failed();
        i.drops_raised = t.drops_raised();
        *out = i;
    });
}

rv_status rv_kind_count(rv_session *session, uint32_t kind, uint32_t *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        require_index(kind, rv::kKindCount, "chunk kind");
        *out = s.trace().kind_count(static_cast<rv::Kind>(kind));
    });
}

rv_status rv_chunk(rv_session *session, uint32_t index, rv_chunk_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace &t = s.trace();
        require_index(index, t.chunks().size(), "chunk");
        const rv::Chunk &c = t.chunks()[index];
        rv_chunk_info i;
        std::memset(&i, 0, sizeof i);
        i.offset = c.offset;
        i.time = c.t;
        i.length = c.len;
        i.kind = static_cast<uint32_t>(c.kind);
        i.corrupt = c.corrupt ? 1u : 0u;
        i.session = c.session;
        i.record = c.record;
        std::memcpy(i.tag, c.tag, 4);
        i.tag[4] = '\0';
        *out = i;
    });
}

rv_status rv_chunk_text(rv_session *session, uint32_t index, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        const rv::Trace &t = s.trace();
        require_index(index, t.chunks().size(), "chunk");
        s.text = t.describe(index);
        *text = s.text.c_str();
    });
}

rv_status rv_issue(rv_session *session, uint32_t index, rv_issue_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace &t = s.trace();
        require_index(index, t.issues().size(), "issue");
        const rv::Issue &is = t.issues()[index];
        out->offset = is.offset;
        out->severity = is.severity == rv::Issue::Error ? RV_ISSUE_ERROR : RV_ISSUE_WARNING;
        out->chunk = is.chunk;
        out->message = is.message.c_str();
    });
}

rv_status rv_run(rv_session *session, uint32_t index, rv_run_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace &t = s.trace();
        require_index(index, t.runs().size(), "run");
        const rv::Run &r = t.runs()[index];
        rv_run_info i;
        std::memset(&i, 0, sizeof i);
        i.chunk = r.chunk;
        i.valid = r.valid ? 1u : 0u;
        i.producer = "";
        i.layer_name[0] = "";
        i.layer_name[1] = "";
        if (r.valid) {
            i.cell = r.cell;
            i.origin_x = r.origin_x;
            i.origin_y = r.origin_y;
            i.track = r.track;
            i.track_wide = r.track_wide;
            i.via_dia = r.via_dia;
            i.via_drill = r.via_drill;
            i.producer = r.producer.c_str();
            i.layer_name[0] = r.layer_name[0].c_str();
            i.layer_name[1] = r.layer_name[1].c_str();
            i.nx = r.nx;
            i.ny = r.ny;
            i.nd = r.nd;
            i.via_cost = r.via_cost;
            i.rip_cost = r.rip_cost;
            i.max_rips = r.max_rips;
            i.flags = r.flags;
        }
        *out = i;
    });
}

rv_status rv_net(rv_session *session, uint32_t index, int32_t *code, const char **name) {
    return guarded(session, [&](rv::Session &s) {
        require_out(code, "code");
        require_out(name, "name");
        const auto &names = s.trace().net_names();
        require_index(index, names.size(), "net");
        auto it = std::next(names.begin(), static_cast<std::ptrdiff_t>(index));
        *code = it->first;
        *name = it->second.c_str();
    });
}

rv_status rv_search(rv_session *session, uint32_t search_id, rv_search_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace &t = s.trace();
        require_index(search_id, t.searches().size(), "search");
        const rv::Search &x = t.searches()[search_id];
        rv_search_info i;
        std::memset(&i, 0, sizeof i);
        i.search_id = search_id;
        i.chunk = x.chunk;
        i.valid = x.valid ? 1u : 0u;
        i.session = x.chunk < t.chunks().size() ? t.chunks()[x.chunk].session : RV_NONE;
        i.route_id = RV_NONE;
        i.repeat_of = RV_NONE;
        i.wavefront_search = RV_NONE;
        if (x.valid) {
            i.time = t.chunks()[x.chunk].t;
            i.duration_s = x.duration_s;
            i.max_cost = x.max_cost;
            i.cost = x.cost;
            i.expanded = x.expanded;
            i.route_id = x.route_id;
            i.ctx = x.ctx;
            i.net = x.net;
            i.flags = x.flags;
            i.nsrc = x.nsrc;
            i.ntgt = x.ntgt;
            i.npath = x.npath;
            i.ntrace = x.ntrace;
            i.ntrace_total = x.ntrace_total;
            i.repeat_of = x.repeat_of;
            i.wavefront_search = x.trace_source;
            if (x.trace_source != rv::kNone32) i.wavefront_length = t.searches()[x.trace_source].ntrace;
        }
        *out = i;
    });
}

rv_status rv_search_cells(rv_session *session, uint32_t search_id, uint32_t which,
                          const uint32_t **cells, uint32_t *count) {
    if (cells != nullptr) *cells = nullptr;
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(cells, "cells");
        require_out(count, "count");
        const rv::Trace &t = s.trace();
        require_index(search_id, t.searches().size(), "search");
        if (which > 2) throw std::invalid_argument("invalid argument: which must be 0, 1 or 2");
        const rv::Search &x = t.searches()[search_id];
        if (!x.valid) return;
        const uint32_t n = which == 0 ? x.nsrc_kept : which == 1 ? x.ntgt_kept : x.npath;
        *cells = n == 0 ? nullptr : which == 0 ? x.src() : which == 1 ? x.tgt() : x.path();
        *count = n;
    });
}

rv_status rv_wavefront(rv_session *session, uint32_t search_id, const uint32_t **cells,
                       const int32_t **costs, uint32_t *count) {
    if (cells != nullptr) *cells = nullptr;
    if (costs != nullptr) *costs = nullptr;
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(cells, "cells");
        require_out(costs, "costs");
        require_out(count, "count");
        rv::Trace &t = s.trace();
        std::shared_ptr<const rv::Wavefront> w = t.wavefront(search_id);
        s.wavefront = w;
        if (!w || w->cells.empty()) return;
        *cells = w->cells.data();
        *costs = w->has_costs ? w->costs.data() : nullptr;
        *count = static_cast<uint32_t>(w->cells.size());
    });
}

// --- M2: boards, layers, classes, display list, camera ----------------------------

rv_status rv_board_count(rv_session *session, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(count, "count");
        const rv::Trace *t = s.trace_or_null();
        *count = t == nullptr ? 0u : static_cast<uint32_t>(t->boards().size());
    });
}

rv_status rv_board(rv_session *session, uint32_t index, rv_board_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Trace *t = s.trace_or_null();
        require_index(index, t == nullptr ? 0 : t->boards().size(), "board");
        const rv::BoardInfo &b = t->boards()[index];
        rv_board_info i;
        std::memset(&i, 0, sizeof i);
        i.thickness_mm = b.thickness_mm;
        i.origin_x_mm = b.origin_x_nm * 1e-6;
        i.origin_y_mm = b.origin_y_nm * 1e-6;
        i.label = b.label.c_str();
        i.chunk = b.chunk;
        i.layer_count = static_cast<uint32_t>(b.layers.size());
        i.net_count = static_cast<uint32_t>(b.nets.size());
        i.footprint_count = static_cast<uint32_t>(b.footprints.size());
        i.record_count = b.nrecords;
        *out = i;
    });
}

rv_status rv_board_select(rv_session *session, uint32_t index) {
    return guarded(session, [&](rv::Session &s) {
        s.drop_borrowed();
        s.scene.select(index);
        s.sync();
    });
}

rv_status rv_board_selected(rv_session *session, uint32_t *index) {
    if (index != nullptr) *index = RV_NONE;
    return guarded(session, [&](rv::Session &s) {
        require_out(index, "index");
        *index = s.scene.effective_board(s.trace_or_null());
    });
}

rv_status rv_layer_count(rv_session *session, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(count, "count");
        *count = static_cast<uint32_t>(s.scene.full(s.trace_or_null()).layers.size());
    });
}

rv_status rv_layer(rv_session *session, uint32_t position, rv_layer_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::DisplayList &dl = s.scene.full(s.trace_or_null());
        require_index(position, dl.layers.size(), "layer");
        const rv::LayerEntry &e = dl.layers[position];
        rv_layer_info i;
        std::memset(&i, 0, sizeof i);
        i.name = e.name.c_str();
        i.index = e.index;
        i.kind = static_cast<uint32_t>(e.cls.kind);
        i.side = static_cast<uint32_t>(e.cls.side);
        i.rgba = e.color;
        i.visible = s.scene.layer_visible(e.name) ? 1u : 0u;
        i.draw_rank = e.rank;
        i.items = e.items;
        i.kicad_id = e.kicad_id;
        i.type = e.type;
        *out = i;
    });
}

rv_status rv_layer_set_visible(rv_session *session, const char *name, uint32_t visible) {
    return guarded(session, [&](rv::Session &s) {
        require_out(name, "name");
        s.drop_borrowed();
        s.scene.set_layer_visible(name, visible != 0);
    });
}

rv_status rv_class_count(rv_session *session, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &) {
        require_out(count, "count");
        *count = rv::kClassCount;
    });
}

rv_status rv_class(rv_session *session, uint32_t cls, const char **name, uint32_t *visible) {
    return guarded(session, [&](rv::Session &s) {
        require_out(name, "name");
        require_out(visible, "visible");
        require_index(cls, rv::kClassCount, "object class");
        *name = rv::class_name(cls);
        *visible = s.scene.class_visible(cls) ? 1u : 0u;
    });
}

rv_status rv_class_set_visible(rv_session *session, uint32_t cls, uint32_t visible) {
    return guarded(session, [&](rv::Session &s) {
        require_index(cls, rv::kClassCount, "object class");
        s.drop_borrowed();
        s.scene.set_class_visible(cls, visible != 0);
    });
}

rv_status rv_get_display_list(rv_session *session, rv_display_list *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv::Scene &sc = s.scene;
        const rv::DisplayList &dl = sc.full(s.trace_or_null());
        const std::vector<rv::Batch> &vis = sc.visible(s.trace_or_null());
        // rv_batch is the public mirror of rv::Batch's leading fields; copy
        // into the session's buffer of C structs.
        std::vector<rv_batch> &cb = s.c_batches;
        if (s.c_batches_generation != sc.generation() || cb.size() != vis.size()) {
            cb.resize(vis.size());
            for (std::size_t k = 0; k < vis.size(); ++k) {
                const rv::Batch &b = vis[k];
                rv_batch &c = cb[k];
                c.prim = static_cast<uint32_t>(b.prim);
                c.cls = b.cls;
                c.layer = b.layer;
                c.order = b.order;
                c.rgba = b.color;
                c.key = b.key;
                c.width = b.width;
                c.first = b.first;
                c.count = b.count;
                for (int i = 0; i < 4; ++i) c.bbox[i] = b.bbox[static_cast<std::size_t>(i)];
            }
            s.c_batches_generation = sc.generation();
        }
        rv_display_list d;
        std::memset(&d, 0, sizeof d);
        d.batches = cb.empty() ? nullptr : cb.data();
        d.xy = dl.xy.empty() ? nullptr : dl.xy.data();
        d.contours = dl.contours.empty() ? nullptr : dl.contours.data();
        d.polys = dl.polys.empty() ? nullptr : dl.polys.data();
        d.segs = dl.segs.empty() ? nullptr : dl.segs.data();
        d.circles = dl.circles.empty() ? nullptr : dl.circles.data();
        d.batch_count = static_cast<uint32_t>(cb.size());
        d.point_count = static_cast<uint32_t>(dl.xy.size() / 2);
        d.contour_count = static_cast<uint32_t>(dl.contours.size() / 2);
        d.poly_count = static_cast<uint32_t>(dl.polys.size() / 2);
        d.seg_count = static_cast<uint32_t>(dl.segs.size() / 4);
        d.circle_count = static_cast<uint32_t>(dl.circles.size() / 3);
        d.background = sc.theme().background;
        d.board = sc.built_board();
        d.has_extent = dl.has_extent ? 1u : 0u;
        d.geometry_generation = sc.geometry_generation();
        d.generation = sc.generation();
        for (int i = 0; i < 4; ++i) d.extent[i] = dl.extent[static_cast<std::size_t>(i)];
        *out = d;
    });
}

rv_status rv_order_key(rv_session *session, uint32_t layer, uint32_t sub, uint32_t *key) {
    if (key != nullptr) *key = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(key, "key");
        if (sub > 0xFF) throw std::invalid_argument("invalid argument: sub must be below 256");
        if (layer == RV_NONE) {
            *key = rv::order_key(rv::kRankHoles, sub);
            return;
        }
        if (layer == RV_OVERLAY_LAYER) {
            *key = rv::order_key(rv::kRankOverlay, sub);
            return;
        }
        const rv::DisplayList &dl = s.scene.full(s.trace_or_null());
        for (const rv::LayerEntry &e : dl.layers)
            if (e.index == layer) {
                *key = rv::order_key(e.rank, sub);
                return;
            }
        throw std::invalid_argument("invalid argument: the board has no layer " + std::to_string(layer));
    });
}

rv_status rv_view_set_size(rv_session *session, double width, double height, double backing) {
    return guarded(session, [&](rv::Session &s) { s.scene.camera.set_size(width, height, backing); });
}

rv_status rv_view_fit(rv_session *session) {
    return guarded(session, [&](rv::Session &s) {
        const rv::DisplayList &dl = fit_list(s);
        s.scene.camera.fit(dl.extent, dl.has_extent);
    });
}

rv_status rv_view_zoom(rv_session *session, double factor, double vx, double vy) {
    return guarded(session, [&](rv::Session &s) { s.scene.camera.zoom(factor, vx, vy); });
}

rv_status rv_view_pan(rv_session *session, double dx, double dy) {
    return guarded(session, [&](rv::Session &s) { s.scene.camera.pan(dx, dy); });
}

rv_status rv_view_get(rv_session *session, rv_view *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Camera &c = s.scene.camera;
        const rv::DisplayList &dl = fit_list(s);
        rv_view v;
        std::memset(&v, 0, sizeof v);
        v.scale = c.scale();
        v.tx = c.tx();
        v.ty = c.ty();
        v.width = c.width();
        v.height = c.height();
        v.backing = c.backing();
        v.fit_scale = c.fit_scale(dl.extent, dl.has_extent);
        v.zoom_percent = 100.0 * c.scale() / v.fit_scale;
        v.min_scale = rv::Camera::kMinScale;
        v.max_scale = rv::Camera::kMaxScale;
        *out = v;
    });
}

rv_status rv_view_to_board(rv_session *session, double vx, double vy, double *bx, double *by) {
    return guarded(session, [&](rv::Session &s) {
        require_out(bx, "bx");
        require_out(by, "by");
        s.scene.camera.to_board(vx, vy, *bx, *by);
    });
}

rv_status rv_board_to_view(rv_session *session, double bx, double by, double *vx, double *vy) {
    return guarded(session, [&](rv::Session &s) {
        require_out(vx, "vx");
        require_out(vy, "vy");
        s.scene.camera.to_view(bx, by, *vx, *vy);
    });
}

rv_status rv_view_status(rv_session *session, double vx, double vy, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        const rv::Camera &c = s.scene.camera;
        const rv::DisplayList &dl = fit_list(s);
        double bx, by;
        c.to_board(vx, vy, bx, by);
        const double zoom = 100.0 * c.scale() / c.fit_scale(dl.extent, dl.has_extent);
        char buf[160];
        std::snprintf(buf, sizeof buf, "X %.4f mm   Y %.4f mm   zoom %.0f%%", bx, by, zoom);
        s.status = buf;
        *text = s.status.c_str();
    });
}

// --- M3: timeline, playback, overlay ------------------------------------------------

const char *rv_event_kind_name(uint32_t kind) {
    return rv::event_kind_name(kind);
}

const char *rv_step_kind_name(uint32_t kind) {
    return rv::step_kind_name(kind);
}

rv_status rv_get_timeline(rv_session *session, rv_timeline *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_timeline tl;
        std::memset(&tl, 0, sizeof tl);
        tl.generation = s.timeline_generation();
        const rv::Timeline *t = s.timeline();
        if (t != nullptr) {
            std::vector<rv_event> &ce = s.c_events;
            tl.unchanged = t->size();
            if (s.c_events_generation != s.timeline_generation()) {
                // M6: only the events that changed since the last call (the
                // others too when the largest search grew: it scales `size`).
                const double lmax = std::log1p(t->max_search_size());
                std::size_t from = lmax == s.c_events_lmax ? std::min<std::size_t>(s.c_events_valid, ce.size()) : 0;
                if (from > t->size()) from = t->size();
                ce.resize(t->size());
                tl.unchanged = static_cast<uint32_t>(from);
                s.c_events_lmax = lmax;
                s.c_events_valid = t->size();
                for (std::size_t k = from; k < ce.size(); ++k) {
                    const rv::Event &e = t->events()[k];
                    rv_event &c = ce[k];
                    std::memset(&c, 0, sizeof c);
                    c.start = e.start;
                    c.duration = e.duration;
                    c.size = e.kind == rv::EvKind::Search && lmax > 0
                                 ? static_cast<float>(std::log1p(e.size) / lmax)
                                 : 0.0f;
                    c.kind = static_cast<uint32_t>(e.kind);
                    c.flags = e.flags;
                    c.chunk = e.chunk;
                    c.last_chunk = e.last;
                    c.record = e.record;
                    c.net = e.net;
                    c.session = e.session;
                    c.route = e.route;
                    c.phase = e.phase;
                    c.board = e.board;
                }
                s.c_events_generation = s.timeline_generation();
            }
            tl.events = ce.empty() ? nullptr : ce.data();
            tl.event_count = static_cast<uint32_t>(ce.size());
            tl.rip_ticks = t->rip_ticks().empty() ? nullptr : t->rip_ticks().data();
            tl.rip_tick_count = static_cast<uint32_t>(t->rip_ticks().size());
            tl.failure_ticks = t->failure_ticks().empty() ? nullptr : t->failure_ticks().data();
            tl.failure_tick_count = static_cast<uint32_t>(t->failure_ticks().size());
            tl.phase_count = static_cast<uint32_t>(t->phases().size());
            tl.total = t->total();
            tl.span = std::max(s.span(), t->total());
            const rv::Trace &tr = s.trace();
            tl.router_seconds = tr.chunks().empty() ? 0.0 : tr.chunks().back().t;
        }
        *out = tl;
    });
}

rv_status rv_phase(rv_session *session, uint32_t index, rv_phase_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::Timeline *t = s.timeline();
        require_index(index, t == nullptr ? 0 : t->phases().size(), "phase");
        const rv::Phase &p = t->phases()[index];
        rv_phase_info i;
        std::memset(&i, 0, sizeof i);
        i.start = p.start;
        i.end = p.end;
        i.label = p.label.c_str();
        i.event = p.event;
        *out = i;
    });
}

rv_status rv_position_get(rv_session *session, rv_position *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_position p;
        std::memset(&p, 0, sizeof p);
        s.sync();
        p.time = s.player.t;
        p.total = s.total();
        p.speed = s.player.speed_value();
        p.direction = s.player.direction;
        p.playing = s.player.playing ? 1u : 0u;
        p.event = RV_NONE;
        p.last_chunk = RV_NONE;
        const rv::Timeline *t = s.timeline();
        if (t != nullptr) {
            p.event_count = t->size();
            p.at_end = s.player.t >= t->total() ? 1u : 0u;
            const uint32_t e = s.event();
            if (e != RV_NONE) {
                const rv::Trace &tr = s.trace();
                p.event = e;
                p.fraction = rv::event_fraction(*t, e, s.player.t);
                p.last_chunk = t->events()[e].last;
                p.router_time = tr.chunks()[p.last_chunk].t;
                p.cells_revealed = rv::cells_revealed(tr, *t, e, s.player.t);
                std::vector<uint32_t> alive;
                t->alive_copper(e, alive);
                p.copper_alive = static_cast<uint32_t>(alive.size());
                std::vector<std::pair<uint32_t, double>> gh;
                t->ghosts(s.player.t, gh);
                p.ghosts = static_cast<uint32_t>(gh.size());
                p.failures = s.failures_now();
                p.routed_hidden = s.routed_hidden() ? 1u : 0u;
                p.settled = rv::search_settled(*t, e, s.player.t) ? 1u : 0u;
                const rv::Event &ev = t->events()[e];
                if ((ev.flags & rv::evf::kHasNet) != 0) {
                    p.net = ev.net;
                    p.has_net = 1;
                } else if (ev.route != RV_NONE && ev.route < tr.routes().size()) {
                    p.net = tr.routes()[ev.route].net;
                    p.has_net = 1;
                }
            }
        }
        *out = p;
    });
}

rv_status rv_advance(rv_session *session, double dt, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_finite(dt, "dt");
        if (dt < 0) throw std::invalid_argument("invalid argument: dt is negative");
        s.advance(dt);
    });
}

rv_status rv_play(rv_session *session, uint32_t playing, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) { s.play(playing != 0); });
}

rv_status rv_set_direction(rv_session *session, int32_t direction) {
    return guarded(session, [&](rv::Session &s) {
        if (direction == 0) throw std::invalid_argument("invalid argument: direction must be +1 or -1");
        s.set_direction(direction);
    });
}

rv_status rv_set_speed(rv_session *session, double speed) {
    return guarded(session, [&](rv::Session &s) {
        require_finite(speed, "speed");
        if (!(speed > 0)) throw std::invalid_argument("invalid argument: speed must be positive");
        int best = 0;
        for (int k = 1; k < rv::kSpeedCount; ++k)
            if (std::fabs(std::log(rv::kSpeeds[k] / speed)) < std::fabs(std::log(rv::kSpeeds[best] / speed))) best = k;
        s.player.speed = best;
    });
}

rv_status rv_speed_step(rv_session *session, int32_t steps) {
    return guarded(session, [&](rv::Session &s) {
        long v = static_cast<long>(s.player.speed) + steps;
        if (v < 0) v = 0;
        if (v >= rv::kSpeedCount) v = rv::kSpeedCount - 1;
        s.player.speed = static_cast<int>(v);
    });
}

rv_status rv_seek_time(rv_session *session, double t, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_finite(t, "t");
        s.seek(t);
    });
}

rv_status rv_seek_fraction(rv_session *session, double fraction, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_finite(fraction, "fraction");
        s.seek(fraction * s.total());
    });
}

rv_status rv_seek_event(rv_session *session, uint32_t event, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        const rv::Timeline *t = s.timeline();
        require_index(event, t == nullptr ? 0 : t->size(), "event");
        s.seek(t->events()[event].start);
    });
}

rv_status rv_jump(rv_session *session, uint32_t to_end, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        // M6: to the end of a growing trace opened live is "Live": follow it.
        if (to_end != 0 && s.live_paced()) s.set_following(true);
        else s.seek(to_end != 0 ? s.total() : 0.0);
    });
}

rv_status rv_step(rv_session *session, uint32_t kind, int32_t direction, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_index(kind, rv::kStepKindCount, "step kind");
        if (direction == 0) throw std::invalid_argument("invalid argument: direction must be +1 or -1");
        s.stop_following();
        s.player.playing = false;
        const rv::Timeline *t = s.timeline();
        if (t == nullptr) return;
        const uint32_t e = rv::step_target(*t, s.player.t, static_cast<rv::StepKind>(kind), direction);
        if (e != RV_NONE) s.set_time(t->events()[e].start);
    });
}

rv_status rv_text(rv_session *session, uint32_t which, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        if (which > RV_TEXT_PACING) throw std::invalid_argument("invalid argument: no text " + std::to_string(which));
        const rv::Timeline *t = s.timeline();
        if (which == RV_TEXT_SPEED) {
            s.text_out = rv::speed_text(s.player.speed_value());
        } else if (which == RV_TEXT_PACING) {
            s.text_out = rv::pacing_text(s.pacing(), s.trace_or_null(), s.total());
        } else if (t == nullptr) {
            s.text_out = "no trace open";
        } else if (which == RV_TEXT_POSITION) {
            s.text_out = rv::position_text(*t, s.event(), s.player.t);
        } else {
            s.text_out = rv::caption(s.trace(), *t, s.event(), s.failures_now());
        }
        *text = s.text_out.c_str();
    });
}

rv_status rv_get_overlay(rv_session *session, rv_display_list *out, const rv_slot **slots, uint32_t *slot_count) {
    if (slots != nullptr) *slots = nullptr;
    if (slot_count != nullptr) *slot_count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        require_out(slots, "slots");
        require_out(slot_count, "slot_count");
        const rv::Overlay &ov = s.overlay_now();
        const rv::DisplayList &dl = ov.list();
        const rv::DisplayList &st = s.scene.full(s.trace_or_null());
        std::vector<rv_batch> &cb = s.c_overlay;
        if (s.c_overlay_generation != ov.generation() || s.c_overlay_epoch != ov.key_epoch() ||
            cb.size() != dl.batches.size()) {
            cb.resize(dl.batches.size());
            for (std::size_t k = 0; k < cb.size(); ++k) {
                const rv::Batch &b = dl.batches[k];
                rv_batch &c = cb[k];
                c.prim = static_cast<uint32_t>(b.prim);
                c.cls = b.cls;
                c.layer = b.layer;
                c.order = b.order;
                c.rgba = b.color;
                c.key = b.key;
                c.width = b.width;
                c.first = b.first;
                c.count = b.count;
                for (int i = 0; i < 4; ++i) c.bbox[i] = b.bbox[static_cast<std::size_t>(i)];
            }
            s.c_slots.resize(ov.slots().size());
            for (std::size_t k = 0; k < s.c_slots.size(); ++k) {
                const rv::Slot &x = ov.slots()[k];
                rv_slot &c = s.c_slots[k];
                std::memset(&c, 0, sizeof c);
                c.kind = static_cast<uint32_t>(x.kind);
                c.order_begin = x.order_begin;
                c.order_end = x.order_end;
                c.layer = x.layer;
                c.first_batch = x.first_batch;
                c.batch_count = x.batch_count;
                c.generation = x.generation;
                for (int i = 0; i < 4; ++i) c.bbox[i] = x.bbox[static_cast<std::size_t>(i)];
            }
            s.c_overlay_generation = ov.generation();
            s.c_overlay_epoch = ov.key_epoch();
        }
        rv_display_list d;
        std::memset(&d, 0, sizeof d);
        d.batches = cb.empty() ? nullptr : cb.data();
        d.xy = dl.xy.empty() ? nullptr : dl.xy.data();
        d.contours = dl.contours.empty() ? nullptr : dl.contours.data();
        d.polys = dl.polys.empty() ? nullptr : dl.polys.data();
        d.segs = dl.segs.empty() ? nullptr : dl.segs.data();
        d.circles = dl.circles.empty() ? nullptr : dl.circles.data();
        d.batch_count = static_cast<uint32_t>(cb.size());
        d.point_count = static_cast<uint32_t>(dl.xy.size() / 2);
        d.contour_count = static_cast<uint32_t>(dl.contours.size() / 2);
        d.poly_count = static_cast<uint32_t>(dl.polys.size() / 2);
        d.seg_count = static_cast<uint32_t>(dl.segs.size() / 4);
        d.circle_count = static_cast<uint32_t>(dl.circles.size() / 3);
        d.background = s.scene.theme().background;
        d.board = s.scene.built_board();
        d.has_extent = st.has_extent ? 1u : 0u;
        d.geometry_generation = ov.key_epoch();
        d.generation = ov.generation();
        for (int i = 0; i < 4; ++i) d.extent[i] = st.extent[static_cast<std::size_t>(i)];
        *out = d;
        *slots = s.c_slots.empty() ? nullptr : s.c_slots.data();
        *slot_count = static_cast<uint32_t>(s.c_slots.size());
    });
}

// --- M4: wavefront and pacing ------------------------------------------------------

rv_status rv_wavefront_image_count(rv_session *session, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(count, "count");
        *count = static_cast<uint32_t>(s.wavefront_now().count());
    });
}

rv_status rv_get_wavefront_image(rv_session *session, uint32_t index, rv_wavefront_image *out) {
    if (out != nullptr) std::memset(out, 0, sizeof *out);
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        if (index > 1) throw std::invalid_argument("invalid argument: no wavefront image " + std::to_string(index));
        const rv::WavefrontImages *w = nullptr;
        try {
            w = &s.wavefront_now();
        } catch (...) {
            // The images are consistent (empty for the unreadable search):
            // describe them, then report the error.
            const rv::WaveImage &im = s.waves.image(static_cast<int>(index));
            rv_wavefront_image o;
            std::memset(&o, 0, sizeof o);
            o.width = im.width;
            o.height = im.height;
            o.generation = im.generation;
            o.search = RV_NONE;
            o.layer = RV_NONE;
            *out = o;
            throw;
        }
        if (index >= static_cast<uint32_t>(w->count()) && !(index == 0 && w->count() == 0))
            throw std::invalid_argument("invalid argument: no wavefront image " + std::to_string(index) + " (there are " +
                                        std::to_string(w->count()) + ")");
        const rv::WaveImage &im = w->image(static_cast<int>(index));
        rv_wavefront_image o;
        std::memset(&o, 0, sizeof o);
        o.pixels = im.pixels.empty() ? nullptr : im.pixels.data();
        o.width = im.width;
        o.height = im.height;
        o.stride = im.width * 4;
        o.order = im.order;
        o.layer = im.board_layer;
        o.router_layer = index;
        o.generation = im.generation;
        o.dirty_from = im.dirty_from;
        for (int k = 0; k < 4; ++k) {
            o.dirty[k] = im.dirty[static_cast<std::size_t>(k)];
            o.bounds[k] = im.bounds[static_cast<std::size_t>(k)];
            o.rect[k] = im.rect[static_cast<std::size_t>(k)];
        }
        o.cells = im.cells;
        o.search = w->search();
        *out = o;
    });
}

const char *rv_pacing_mode_name(uint32_t mode) {
    return rv::pacing_mode_name(mode);
}

rv_status rv_get_pacing(rv_session *session, rv_pacing *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_pacing p;
        std::memset(&p, 0, sizeof p);
        p.mode = static_cast<uint32_t>(s.pacing().mode);
        p.net = s.pacing().net;
        p.fit_seconds = s.pacing().fit_seconds;
        p.slowdown = s.pacing().slowdown;
        *out = p;
    });
}

rv_status rv_set_pacing(rv_session *session, const rv_pacing *pacing, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_out(pacing, "pacing");
        require_index(pacing->mode, rv::kPacingModeCount, "pacing mode");
        require_finite(pacing->fit_seconds, "fit_seconds");
        require_finite(pacing->slowdown, "slowdown");
        if (pacing->fit_seconds < rv::kFitSecondsMin || pacing->fit_seconds > rv::kFitSecondsMax)
            throw std::invalid_argument("invalid argument: fit_seconds must be 1 to 86400");
        if (pacing->slowdown < rv::kSlowdownMin || pacing->slowdown > rv::kSlowdownMax)
            throw std::invalid_argument("invalid argument: slowdown must be 0.01 to 100000");
        rv::PacingSettings p;
        p.mode = static_cast<rv::PacingMode>(pacing->mode);
        p.net = pacing->net;
        p.fit_seconds = pacing->fit_seconds;
        p.slowdown = pacing->slowdown;
        s.set_pacing(p);
    });
}

// --- M5: nets table, selection, hit testing, history ---------------------------------

const char *rv_net_state_name(uint32_t state) {
    return rv::net_state_name(state);
}

const char *rv_net_column_title(uint32_t column) {
    return rv::net_column_title(column);
}

uint32_t rv_net_column_align(uint32_t column) {
    return column < rv::netcol::Count ? rv::net_column_align(column) : 0u;
}

rv_status rv_nets_get(rv_session *session, rv_nets_info *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_nets_info i;
        std::memset(&i, 0, sizeof i);
        i.selected_position = RV_NONE;
        i.sort_column = rv::netcol::Name;
        if (s.has_trace()) {
            const rv::NetTable &t = s.nets_now();
            i.row_count = static_cast<uint32_t>(t.rows().size());
            i.view_count = static_cast<uint32_t>(t.view().size());
            i.generation = t.generation();
            i.sort_column = t.sort_column();
            i.descending = t.descending() ? 1u : 0u;
            const rv::Session::Selection &sel = s.selection();
            if (sel.active && sel.has_net) {
                const uint32_t e = s.event();
                const uint32_t sess = e != RV_NONE ? s.timeline()->events()[e].session : RV_NONE;
                uint32_t best = RV_NONE;
                for (uint32_t k = 0; k < t.view().size(); ++k) {
                    const rv::NetRow &r = t.rows()[t.view()[k]];
                    if (r.net != sel.net) continue;
                    if (r.session == sess) {
                        best = k;
                        break;
                    }
                    if (best == RV_NONE || r.session != RV_NONE) best = k; // the latest session's row
                }
                i.selected_position = best;
            }
        }
        *out = i;
    });
}

rv_status rv_nets_set_view(rv_session *session, uint32_t column, uint32_t descending, const char *filter) {
    return guarded(session, [&](rv::Session &s) {
        require_index(column, rv::netcol::Count, "nets column");
        s.nets.set_view(column, descending != 0, filter != nullptr ? filter : "");
    });
}

rv_status rv_nets_row(rv_session *session, uint32_t position, rv_net_row *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        const rv::NetTable &t = s.nets;
        require_index(position, t.view().size(), "nets row");
        const uint32_t r = t.view()[position];
        const rv::NetRow &x = t.rows()[r];
        const rv::NetNow n = r < t.now().size() ? t.now()[r] : rv::NetNow();
        rv_net_row o;
        std::memset(&o, 0, sizeof o);
        o.seconds = x.seconds;
        o.expanded = x.expanded;
        o.name = x.name.c_str();
        o.row = r;
        o.net = x.net;
        o.session = x.session;
        o.named = x.named ? 1u : 0u;
        o.state = static_cast<uint32_t>(n.state);
        o.routes = x.routes;
        o.searches = x.searches;
        o.found = x.found;
        o.not_found = x.not_found;
        o.commits = x.commits;
        o.vias = x.vias;
        o.rips = x.rips;
        o.failed_pads = x.failed_pads;
        o.first_event = x.first_event;
        o.last_event = x.last_event;
        o.copper = n.copper;
        o.failures = n.failures;
        *out = o;
    });
}

rv_status rv_nets_cell(rv_session *session, uint32_t position, uint32_t column, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        const rv::NetTable &t = s.nets;
        require_index(position, t.view().size(), "nets row");
        require_index(column, rv::netcol::Count, "nets column");
        s.cell_text = t.cell(t.view()[position], column);
        *text = s.cell_text.c_str();
    });
}

rv_status rv_select_net(rv_session *session, int32_t net, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) { s.select_net(net); });
}

rv_status rv_select_name(rv_session *session, const char *name, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) { s.select_name(name != nullptr ? name : ""); });
}

rv_status rv_select_clear(rv_session *session, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) { s.select(rv::Session::Selection()); });
}

rv_status rv_selection_get(rv_session *session, rv_selection *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        rv_selection o;
        std::memset(&o, 0, sizeof o);
        const rv::Session::Selection &sel = s.selection();
        o.name = sel.name.c_str();
        o.active = sel.active ? 1u : 0u;
        o.has_net = sel.has_net ? 1u : 0u;
        o.net = sel.net;
        if (s.has_trace()) {
            o.board_nets = static_cast<uint32_t>(s.selected_board_nets().size());
            o.board_items = s.selected_board_items();
        }
        *out = o;
    });
}

rv_status rv_hit_test(rv_session *session, double vx, double vy, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(count, "count");
        require_finite(vx, "vx");
        require_finite(vy, "vy");
        s.hits.clear();
        s.hits = s.pick(vx, vy);
        *count = static_cast<uint32_t>(s.hits.size());
    });
}

rv_status rv_hit_get(rv_session *session, uint32_t index, rv_hit *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        require_index(index, s.hits.size(), "hit");
        const rv::Hit &h = s.hits[index];
        rv_hit o;
        std::memset(&o, 0, sizeof o);
        o.distance = h.dist;
        o.cost = h.cost;
        o.net_name = h.net_name.c_str();
        o.footprint = h.footprint.c_str();
        o.pad = h.pad.c_str();
        o.text = h.text.c_str();
        o.kind = h.kind;
        o.order = h.order;
        o.layer = h.layer;
        o.router_layer = h.router_layer;
        o.net = h.net;
        o.has_net = h.has_net ? 1u : 0u;
        o.board_net = h.board_net;
        o.copper = h.copper;
        o.event = h.event;
        o.record = h.record;
        o.cell = h.cell;
        o.order_index = h.order_index;
        *out = o;
    });
}

rv_status rv_hover_status(rv_session *session, double vx, double vy, const char **text) {
    if (text != nullptr) *text = nullptr;
    return guarded(session, [&](rv::Session &s) {
        require_out(text, "text");
        require_finite(vx, "vx");
        require_finite(vy, "vy");
        const rv::Camera &c = s.scene.camera;
        const rv::DisplayList &dl = fit_list(s);
        double bx, by;
        c.to_board(vx, vy, bx, by);
        const double zoom = 100.0 * c.scale() / c.fit_scale(dl.extent, dl.has_extent);
        char buf[160];
        std::snprintf(buf, sizeof buf, "X %.4f mm   Y %.4f mm   zoom %.0f%%", bx, by, zoom);
        s.hover = buf;
        const std::vector<rv::Hit> hits = s.pick(vx, vy);
        if (!hits.empty()) s.hover += "   ·   " + hits.front().text;
        *text = s.hover.c_str();
    });
}

rv_status rv_click(rv_session *session, double vx, double vy, uint32_t *changed) {
    return playback(session, changed, [&](rv::Session &s) {
        require_finite(vx, "vx");
        require_finite(vy, "vy");
        const std::vector<rv::Hit> hits = s.pick(vx, vy);
        for (const rv::Hit &h : hits) {
            if (h.has_net) {
                s.select_net(h.net);
                return;
            }
            if (h.board_net >= 0 && !h.net_name.empty()) {
                s.select_name(h.net_name);
                return;
            }
        }
        s.select(rv::Session::Selection());
    });
}

rv_status rv_net_history(rv_session *session, int32_t net, uint32_t *count) {
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(count, "count");
        s.history.clear();
        const rv::Timeline *t = s.timeline();
        if (t != nullptr) s.history = rv::net_history(s.trace(), *t, net);
        *count = static_cast<uint32_t>(s.history.size());
    });
}

rv_status rv_history_get(rv_session *session, uint32_t index, rv_history_entry *out) {
    return guarded(session, [&](rv::Session &s) {
        require_out(out, "out");
        require_index(index, s.history.size(), "history entry");
        const rv::HistoryEntry &h = s.history[index];
        rv_history_entry o;
        std::memset(&o, 0, sizeof o);
        o.text = h.text.c_str();
        o.depth = h.depth;
        o.kind = h.kind;
        o.event = h.event;
        o.chunk = h.chunk;
        o.session = h.session;
        o.flags = h.flags;
        *out = o;
    });
}

rv_status rv_net_events(rv_session *session, int32_t net, const uint32_t **events, uint32_t *count) {
    if (events != nullptr) *events = nullptr;
    if (count != nullptr) *count = 0;
    return guarded(session, [&](rv::Session &s) {
        require_out(events, "events");
        require_out(count, "count");
        s.net_event_list.clear();
        const rv::Timeline *t = s.timeline();
        if (t != nullptr) s.net_event_list = rv::net_events(s.trace(), *t, net);
        *events = s.net_event_list.empty() ? nullptr : s.net_event_list.data();
        *count = static_cast<uint32_t>(s.net_event_list.size());
    });
}

} // extern "C"

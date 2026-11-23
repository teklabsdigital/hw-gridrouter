// api.cpp: the C boundary of the core.
//
// Each entry point translates between the C types of rv.h and the C++
// objects behind them.  No C++ exception crosses this boundary: every entry
// point catches everything and maps it to an rv_status, recording the
// message on the session when there is one.

#include "rv.h"

#include "errors.hpp"
#include "session.hpp"

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

template <class T>
void require_out(const T *out, const char *name) {
    if (out == nullptr) throw std::invalid_argument(std::string("invalid argument: ") + name + " is NULL");
}

void require_index(std::size_t index, std::size_t count, const char *what) {
    if (index >= count)
        throw std::invalid_argument(std::string("invalid argument: no ") + what + " with index " +
                                    std::to_string(index) + " (there are " + std::to_string(count) + ")");
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
    return guarded(session, [&](rv::Session &s) {
        require_out(path, "path");
        s.open(path);
    });
}

rv_status rv_close(rv_session *session) {
    return guarded(session, [&](rv::Session &s) { s.close(); });
}

rv_status rv_refresh(rv_session *session, uint32_t *new_chunks) {
    if (new_chunks != nullptr) *new_chunks = 0;
    return guarded(session, [&](rv::Session &s) {
        rv::Trace &t = s.trace();
        s.drop_borrowed();
        const uint32_t n = t.refresh();
        if (new_chunks != nullptr) *new_chunks = n;
    });
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
        const uint32_t n = which == 0 ? x.nsrc : which == 1 ? x.ntgt : x.npath;
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

} // extern "C"

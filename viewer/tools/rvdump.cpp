// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// rvdump: print what the core reads from a .grtrace file.
//
//   rvdump FILE              summary
//   rvdump --chunks FILE     one line per chunk
//   rvdump --search N FILE   one search in detail, with its first trace entries
//   rvdump --board N FILE    board N (or "latest"): layers and display list
//   rvdump --timeline [--bench SEEKS] [--pacing PACING] FILE
//                            the timeline, the state at a few positions, and
//                            optionally the time of SEEKS random seeks
//                            (with the wavefront images) and of the worst
//                            wavefront seek; PACING as in rvrender
//   rvdump --nets [--at AT] [--sort COLUMN[:desc]] [--filter TEXT] FILE
//                            the nets table at a position (default the end)
//   rvdump --net NAME [--at AT] FILE
//                            select a net by name: the highlight, its table
//                            row, its attempt history and its events
//   rvdump --hit X,Y [--at AT] [--bench N] FILE
//                            what is under board point (X, Y) mm, in a
//                            1600 x 1000 view fitted to the board, and the
//                            time of N hit tests at random view points
//   rvdump --follow [--interval MS] [--stall-exit S] [--quiet] FILE
//                            M6: follow a file being written (it may not
//                            exist yet): poll it every MS milliseconds
//                            (default 250), print each new event, and stop
//                            at END; reload when it is replaced; give up
//                            after S seconds without growth (default never).
//                            Ends with the refresh times.
//   rvdump --help            the usage
//
// Exit status: 0 for a readable trace with no errors (complete, or a valid
// file that is still being written: "incomplete") and for --help; 1 if the
// file cannot be opened or read, is not a trace, or has corrupt parts; 2 for
// bad usage (also a FILE that starts with '-': an unknown option).
//
// It uses only the C interface rv.h, like every client, so it also shows
// that the interface is enough to inspect a file.

#include "rv.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

// Bad usage: on stderr, exit 2; --help: on stdout, exit 0.
int usage(bool asked = false) {
    std::fprintf(asked ? stdout : stderr,
                 "usage: rvdump FILE\n"
                 "       rvdump --chunks FILE\n"
                 "       rvdump --search N FILE\n"
                 "       rvdump --board N|latest FILE\n"
                 "       rvdump --timeline [--bench SEEKS] [--pacing PACING] FILE\n"
                 "       rvdump --nets [--at AT] [--sort COLUMN[:desc]] [--filter TEXT] FILE\n"
                 "       rvdump --net NAME [--at AT] FILE\n"
                 "       rvdump --hit X,Y [--at AT] [--bench N] FILE\n"
                 "       rvdump --follow [--interval MS] [--stall-exit S] [--quiet] FILE\n"
                 "       rvdump --help\n"
                 "PACING: fit[:SECONDS], nets, net:NET, skip-failed, real[:SLOWDOWN]\n"
                 "AT: a fraction of the timeline 0..1, or end (the default)\n"
                 "COLUMN: a column title in lower case, spaces as '-' (name, state, failed-pads, ...)\n");
    return asked ? 0 : 2;
}

// "fit", "fit:60", "nets", "net:4", "skip-failed", "real", "real:10".
bool parse_pacing(const char *spec, rv_pacing *p) {
    const char *colon = std::strchr(spec, ':');
    const std::string name = colon ? std::string(spec, colon) : std::string(spec);
    double value = 0;
    if (colon) {
        char *end = nullptr;
        value = std::strtod(colon + 1, &end);
        if (end == colon + 1 || *end != '\0') return false;
    }
    if (name == "fit") {
        p->mode = RV_PACING_FIT;
        if (colon) p->fit_seconds = value;
    } else if (name == "nets" && !colon) {
        p->mode = RV_PACING_NETS_ONLY;
    } else if (name == "net" && colon && value == static_cast<double>(static_cast<int32_t>(value))) {
        p->mode = RV_PACING_ONE_NET;
        p->net = static_cast<int32_t>(value);
    } else if (name == "skip-failed" && !colon) {
        p->mode = RV_PACING_SKIP_FAILED;
    } else if (name == "real") {
        p->mode = RV_PACING_REAL_TIME;
        if (colon) p->slowdown = value;
    } else {
        return false;
    }
    return true;
}

struct Grid {
    int32_t nx = 0, ny = 0;
};

std::string cell_text(uint32_t cell, const Grid &g) {
    char b[64];
    if (g.nx <= 0 || g.ny <= 0) {
        std::snprintf(b, sizeof b, "%u", cell);
        return b;
    }
    const uint64_t nc = static_cast<uint64_t>(g.nx) * static_cast<uint64_t>(g.ny);
    const uint64_t layer = cell / nc, idx = cell % nc;
    std::snprintf(b, sizeof b, "%u=L%" PRIu64 "(%" PRIu64 ",%" PRIu64 ")", cell, layer,
                  idx % static_cast<uint64_t>(g.nx), idx / static_cast<uint64_t>(g.nx));
    return b;
}

Grid grid_of(rv_session *s, uint32_t session) {
    Grid g;
    rv_run_info r;
    if (session != RV_NONE && rv_run(s, session, &r) == RV_OK && r.valid) {
        g.nx = r.nx;
        g.ny = r.ny;
    }
    return g;
}

std::string id_text(uint32_t v) {
    return v == RV_NONE ? std::string("none") : std::to_string(v);
}

void print_issues(rv_session *s, const rv_trace_info &info, uint32_t limit) {
    std::printf("issues:    %u errors, %u warnings\n", info.error_count, info.warning_count);
    for (uint32_t k = 0; k < info.issue_count && k < limit; ++k) {
        rv_issue_info is;
        if (rv_issue(s, k, &is) != RV_OK) continue;
        std::printf("  %-7s chunk %-6s @%-10" PRIu64 " %s\n", is.severity == RV_ISSUE_ERROR ? "error" : "warning",
                    id_text(is.chunk).c_str(), is.offset, is.message);
    }
    if (info.issue_count > limit) std::printf("  ... %u more\n", info.issue_count - limit);
}

void print_state(const char *path, const rv_trace_info &info) {
    std::printf("file:      %s\n", path);
    if (!info.header_complete) {
        std::printf("state:     incomplete (the 24-byte header is not written yet)\n");
        std::printf("size:      %" PRIu64 " bytes\n", info.file_size);
        return;
    }
    std::printf("format:    %u.%u\n", info.version_major, info.version_minor);
    std::printf("opened:    %.3f (Unix time)\n", info.open_time);
    if (info.complete)
        std::printf("state:     complete\n");
    else
        std::printf("state:     incomplete (no END chunk: still being written, or the writer stopped)\n");
    if (info.stopped) std::printf("           reading stopped at a corrupt chunk header\n");
    if (info.failed) std::printf("           the file shrank or was rewritten while open\n");
    std::printf("size:      %" PRIu64 " bytes, %" PRIu64 " read", info.file_size, info.indexed_end);
    if (info.file_size > info.indexed_end)
        std::printf(", %" PRIu64 " after the last complete chunk", info.file_size - info.indexed_end);
    std::printf("\n");
    std::printf("time:      last chunk at %.3f s\n", info.last_time);
}

int summary(rv_session *s, const char *path, const rv_trace_info &info) {
    print_state(path, info);
    if (!info.header_complete) return 0;

    std::printf("chunks:    %u\n", info.chunk_count);
    std::map<std::string, uint32_t> unknown;
    for (uint32_t k = 0; k < info.chunk_count; ++k) {
        rv_chunk_info c;
        if (rv_chunk(s, k, &c) == RV_OK && c.kind == RV_CHUNK_UNKNOWN) ++unknown[c.tag];
    }
    for (uint32_t kind = 1; kind < RV_CHUNK_KIND_COUNT; ++kind) {
        uint32_t n = 0;
        if (rv_kind_count(s, kind, &n) == RV_OK && n > 0) std::printf("  \"%s\"  %u\n", rv_kind_tag(kind), n);
    }
    for (const auto &u : unknown) std::printf("  \"%s\"  %u  (unknown tag, skipped)\n", u.first.c_str(), u.second);
    print_issues(s, info, 20);

    std::printf("runs:      %u\n", info.run_count);
    for (uint32_t k = 0; k < info.run_count; ++k) {
        rv_run_info r;
        if (rv_run(s, k, &r) != RV_OK) continue;
        if (!r.valid) {
            std::printf("  %u: corrupt RUN chunk %u\n", k, r.chunk);
            continue;
        }
        std::printf("  %u: grid %d x %d, %d directions, cell %g mm, origin (%g, %g) mm, layers %s / %s, "
                    "via_cost %d, rip_cost %d, max_rips %d, flags 0x%x\n",
                    k, r.nx, r.ny, r.nd, r.cell, r.origin_x, r.origin_y, r.layer_name[0], r.layer_name[1],
                    r.via_cost, r.rip_cost, r.max_rips, r.flags);
        std::printf("     producer: %s\n", r.producer);
    }

    std::printf("nets:      %u\n", info.net_count);
    for (uint32_t k = 0; k < info.net_count && k < 40; ++k) {
        int32_t code = 0;
        const char *name = nullptr;
        if (rv_net(s, k, &code, &name) == RV_OK) std::printf("  %6d  %s\n", code, name);
    }
    if (info.net_count > 40) std::printf("  ... %u more\n", info.net_count - 40);

    uint32_t found = 0, raised = 0, repeats = 0, waves = 0, corrupt = 0;
    for (uint32_t k = 0; k < info.search_count; ++k) {
        rv_search_info si;
        if (rv_search(s, k, &si) != RV_OK) continue;
        if (!si.valid) {
            ++corrupt;
            continue;
        }
        if (si.flags & RV_SRCH_ERROR) ++raised;
        else if (si.flags & RV_SRCH_FOUND) ++found;
        if (si.repeat_of != RV_NONE) ++repeats;
        if (si.wavefront_search != RV_NONE) ++waves;
    }
    std::printf("searches:  %u (found %u, not found %u, raised %u, corrupt %u; repeats %u; with a wavefront %u)\n",
                info.search_count, found, info.search_count - found - raised - corrupt, raised, corrupt, repeats,
                waves);
    std::printf("  expanded total:  %" PRId64 "\n", info.expanded_total);
    std::printf("  trace entries:   %" PRId64 "\n", info.trace_entries);
    uint32_t commits = 0, vias = 0, rips = 0, rebuilds = 0, routes = 0;
    rv_kind_count(s, RV_CHUNK_CMIT, &commits);
    rv_kind_count(s, RV_CHUNK_VIA, &vias);
    rv_kind_count(s, RV_CHUNK_RIP, &rips);
    rv_kind_count(s, RV_CHUNK_RBLD, &rebuilds);
    rv_kind_count(s, RV_CHUNK_RNB, &routes);
    std::printf("route_net: %u calls\n", routes);
    std::printf("commits:   %u (standalone vias %u; copper ids %u)\n", commits, vias, info.copper_count);
    std::printf("rip-ups:   %u rip calls, %u rebuilds, %u copper ids removed\n", rips, rebuilds,
                info.copper_removed);
    std::printf("failures:  %u failed pads (RNE), drop_to_plane failed %u, raised %u\n", info.failed_pads,
                info.drops_failed, info.drops_raised);
    return info.error_count == 0 && !info.failed ? 0 : 1;
}

int chunks(rv_session *s, const rv_trace_info &info) {
    std::printf("%-6s %-10s %-4s %-10s %-12s %s\n", "index", "offset", "tag", "length", "t (s)", "content");
    for (uint32_t k = 0; k < info.chunk_count; ++k) {
        rv_chunk_info c;
        const char *text = nullptr;
        if (rv_chunk(s, k, &c) != RV_OK || rv_chunk_text(s, k, &text) != RV_OK) {
            std::fprintf(stderr, "rvdump: chunk %u: %s\n", k, rv_last_error(s));
            return 1;
        }
        std::printf("%-6u %-10" PRIu64 " %-4s %-10u %-12.6f %s\n", k, c.offset, c.tag, c.length, c.time, text);
    }
    if (info.issue_count > 0) print_issues(s, info, info.issue_count);
    if (!info.complete) std::printf("incomplete: no END chunk\n");
    return info.error_count == 0 && !info.failed ? 0 : 1;
}

void print_cells(const char *label, const uint32_t *cells, uint32_t n, const Grid &g) {
    std::printf("  %-8s %u:", label, n);
    for (uint32_t k = 0; k < n && k < 8; ++k) std::printf(" %s", cell_text(cells[k], g).c_str());
    if (n > 8) std::printf(" ...");
    std::printf("\n");
}

int search(rv_session *s, uint32_t id) {
    rv_search_info si;
    if (rv_search(s, id, &si) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("search %u: chunk %u, session %s\n", id, si.chunk, id_text(si.session).c_str());
    if (!si.valid) {
        std::printf("  corrupt SRCH chunk\n");
        return 1;
    }
    const Grid g = grid_of(s, si.session);
    static const char *const names[10] = {"wide", "has_goal", "has_max", "via_goal", "relax",
                                          "found", "new_via", "has_costs", "error", "has_trace"};
    std::printf("  net %d, ctx %u (%s), route %s\n", si.net, si.ctx,
                si.ctx == 0 ? "direct" : si.ctx == 1 ? "route_net" : si.ctx == 2 ? "drop_to_plane" : "?",
                id_text(si.route_id).c_str());
    std::printf("  flags 0x%x:", si.flags);
    for (int b = 0; b < 10; ++b)
        if (si.flags & (1u << b)) std::printf(" %s", names[b]);
    std::printf("\n");
    std::printf("  result: %s, cost %" PRId64 ", expanded %" PRId64 ", max_cost %" PRId64 "\n",
                (si.flags & RV_SRCH_ERROR) ? "raised" : (si.flags & RV_SRCH_FOUND) ? "found" : "not found", si.cost,
                si.expanded, si.max_cost);
    std::printf("  time: ended at %.6f s, took %.6f s\n", si.time, si.duration_s);
    const char *labels[3] = {"sources", "targets", "path"};
    for (uint32_t w = 0; w < 3; ++w) {
        const uint32_t *cells = nullptr;
        uint32_t n = 0;
        if (rv_search_cells(s, id, w, &cells, &n) == RV_OK) print_cells(labels[w], cells, n, g);
    }
    std::printf("  trace: %u stored, %u counted by the core", si.ntrace, si.ntrace_total);
    if (si.repeat_of != RV_NONE) std::printf(", repeat of search %u", si.repeat_of);
    std::printf("\n");
    const uint32_t *cells = nullptr;
    const int32_t *costs = nullptr;
    uint32_t n = 0;
    if (rv_wavefront(s, id, &cells, &costs, &n) != RV_OK) {
        std::printf("  wavefront: cannot be read: %s\n", rv_last_error(s));
        return 1;
    }
    if (si.wavefront_search == RV_NONE) {
        std::printf("  wavefront: none\n");
        return 0;
    }
    std::printf("  wavefront: %u entries (arrays of search %u)%s\n", n, si.wavefront_search,
                costs ? "" : ", no costs");
    for (uint32_t k = 0; k < n && k < 12; ++k) {
        if (costs)
            std::printf("    %4u  %-22s d %d\n", k, cell_text(cells[k], g).c_str(), costs[k]);
        else
            std::printf("    %4u  %s\n", k, cell_text(cells[k], g).c_str());
    }
    if (n > 12) std::printf("    ... %u more\n", n - 12);
    return 0;
}

const char *prim_name(uint32_t p) {
    switch (p) {
    case RV_FILL: return "fill";
    case RV_STROKE: return "stroke";
    case RV_SEGS: return "segs";
    case RV_DISC: return "disc";
    case RV_RING: return "ring";
    default: return "?";
    }
}

// Points (for fills and strokes) or items behind one batch.
uint64_t batch_points(const rv_display_list &d, const rv_batch &b) {
    uint64_t n = 0;
    if (b.prim == RV_FILL) {
        for (uint32_t p = b.first; p < b.first + b.count; ++p)
            for (uint32_t c = d.polys[2 * p]; c < d.polys[2 * p] + d.polys[2 * p + 1]; ++c) n += d.contours[2 * c + 1];
    } else if (b.prim == RV_STROKE) {
        for (uint32_t c = b.first; c < b.first + b.count; ++c) n += d.contours[2 * c + 1];
    }
    return n;
}

void print_list_totals(const char *label, const rv_display_list &d) {
    uint64_t items[6] = {0, 0, 0, 0, 0, 0}, points = 0, contours = 0;
    for (uint32_t k = 0; k < d.batch_count; ++k) {
        const rv_batch &b = d.batches[k];
        items[b.prim < 6 ? b.prim : 0] += b.count;
        points += batch_points(d, b);
        if (b.prim == RV_FILL)
            for (uint32_t p = b.first; p < b.first + b.count; ++p) contours += d.polys[2 * p + 1];
        if (b.prim == RV_STROKE) contours += b.count;
    }
    std::printf("%s: %u batches; %" PRIu64 " polygons, %" PRIu64 " polylines, %" PRIu64 " segments, %" PRIu64
                " discs, %" PRIu64 " rings; %" PRIu64 " contours, %" PRIu64 " points\n",
                label, d.batch_count, items[RV_FILL], items[RV_STROKE], items[RV_SEGS], items[RV_DISC], items[RV_RING],
                contours, points);
}

int board(rv_session *s, uint32_t which) {
    uint32_t n = 0;
    rv_board_count(s, &n);
    std::printf("boards:    %u\n", n);
    for (uint32_t k = 0; k < n; ++k) {
        rv_board_info b;
        if (rv_board(s, k, &b) != RV_OK) continue;
        std::printf("  %u: \"%s\" chunk %u, %u layers, %u nets, %u footprints, %u records, thickness %g mm, "
                    "origin (%g, %g) mm\n",
                    k, b.label, b.chunk, b.layer_count, b.net_count, b.footprint_count, b.record_count, b.thickness_mm,
                    b.origin_x_mm, b.origin_y_mm);
    }
    if (rv_board_select(s, which) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    uint32_t sel = RV_NONE;
    rv_board_selected(s, &sel);
    if (sel == RV_NONE) {
        std::printf("no board to draw\n");
        return n == 0 ? 0 : 1;
    }
    uint32_t nl = 0;
    if (rv_layer_count(s, &nl) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("drawing board %u; layers in Appearance order:\n", sel);
    std::vector<std::string> names;
    for (uint32_t k = 0; k < nl; ++k) {
        rv_layer_info l;
        if (rv_layer(s, k, &l) != RV_OK) continue;
        names.push_back(l.name);
        std::printf("  %-16s index %-3u kind %-2u side %u rank %-4u #%08x %s, %u items\n", l.name, l.index, l.kind,
                    l.side, l.draw_rank, l.rgba, l.visible ? "shown " : "hidden", l.items);
    }
    rv_display_list d;
    if (rv_get_display_list(s, &d) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("extent: (%.4f, %.4f) - (%.4f, %.4f) mm; background #%08x\n", d.extent[0], d.extent[1], d.extent[2],
                d.extent[3], d.background);
    std::printf("%-5s %-6s %-8s %-16s %-6s %-9s %-7s %-6s %-8s %s\n", "key", "prim", "order", "layer", "class",
                "rgba", "width", "items", "points", "bbox");
    for (uint32_t k = 0; k < d.batch_count; ++k) {
        const rv_batch &b = d.batches[k];
        std::string layer = "(holes)";
        for (uint32_t j = 0; j < nl; ++j) {
            rv_layer_info l;
            if (rv_layer(s, j, &l) == RV_OK && l.index == b.layer) layer = l.name;
        }
        std::printf("%-5u %-6s 0x%-6x %-16s %-6u %08x  %-7.3f %-6u %-8" PRIu64 " (%.2f, %.2f)-(%.2f, %.2f)\n", b.key,
                    prim_name(b.prim), b.order, layer.c_str(), b.cls, b.rgba, b.width, b.count, batch_points(d, b),
                    b.bbox[0], b.bbox[1], b.bbox[2], b.bbox[3]);
    }
    print_list_totals("visible (default layers)", d);
    for (const std::string &name : names) rv_layer_set_visible(s, name.c_str(), 1);
    if (rv_get_display_list(s, &d) == RV_OK) print_list_totals("everything shown", d);
    return 0;
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The timeline: events by kind, phases, and the state at a few positions.
// With seeks > 0, also times that many seeks to pseudo-random positions
// (position, overlay and static list fetched, as a client does per frame).
int timeline(rv_session *s, double open_ms, uint32_t seeks) {
    rv_timeline tl;
    if (rv_get_timeline(s, &tl) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("open:      %.1f ms\n", open_ms);
    std::printf("events:    %u over %.1f s of playback at 1x (router %.2f s)\n", tl.event_count, tl.total,
                tl.router_seconds);
    uint32_t by_kind[RV_EV_KIND_COUNT] = {};
    for (uint32_t k = 0; k < tl.event_count; ++k)
        if (tl.events[k].kind < RV_EV_KIND_COUNT) ++by_kind[tl.events[k].kind];
    for (uint32_t k = 0; k < RV_EV_KIND_COUNT; ++k)
        if (by_kind[k] != 0) std::printf("  %-18s %u\n", rv_event_kind_name(k), by_kind[k]);
    std::printf("ticks:     %u rip-ups, %u failures\n", tl.rip_tick_count, tl.failure_tick_count);
    std::printf("phases:    %u\n", tl.phase_count);
    for (uint32_t k = 0; k < tl.phase_count; ++k) {
        rv_phase_info p;
        if (rv_phase(s, k, &p) == RV_OK)
            std::printf("  %7.2f - %7.2f s  event %-6u %s\n", p.start, p.end, p.event, p.label);
    }
    const double at[] = {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0};
    for (double f : at) {
        rv_seek_fraction(s, f, nullptr);
        rv_position p;
        const char *cap = "";
        const char *pos = "";
        rv_position_get(s, &p);
        rv_text(s, RV_TEXT_CAPTION, &cap);
        std::string caption = cap;
        rv_text(s, RV_TEXT_POSITION, &pos);
        rv_display_list ov;
        const rv_slot *slots = nullptr;
        uint32_t nslots = 0;
        rv_get_overlay(s, &ov, &slots, &nslots);
        std::printf("at %.2f: %s; copper %u, ghosts %u, failures %u%s; overlay %u batches in %u slots\n  %s\n", f, pos,
                    p.copper_alive, p.ghosts, p.failures, p.routed_hidden ? " (routing held by the board)" : "",
                    ov.batch_count, nslots, caption.c_str());
    }
    const char *pacing = "";
    rv_text(s, RV_TEXT_PACING, &pacing);
    std::printf("pacing:    %s\n", pacing);
    if (seeks == 0) return 0;
    // One seek as a client does it per frame: position, overlay, static
    // list and the wavefront images.  Returns ms; *cells gets the cells drawn.
    auto seek = [&](double t, uint32_t *cells, uint32_t *batches) {
        const double t0 = now_ms();
        rv_seek_time(s, t, nullptr);
        rv_position p;
        rv_position_get(s, &p);
        rv_display_list ov, st;
        const rv_slot *slots = nullptr;
        uint32_t nslots = 0;
        rv_get_overlay(s, &ov, &slots, &nslots);
        rv_get_display_list(s, &st);
        uint32_t n = 0, drawn = 0;
        rv_wavefront_image_count(s, &n);
        for (uint32_t k = 0; k < n; ++k) {
            rv_wavefront_image im;
            if (rv_get_wavefront_image(s, k, &im) == RV_OK) drawn += im.cells;
        }
        const double ms = now_ms() - t0;
        if (cells) *cells = drawn;
        if (batches) *batches = ov.batch_count;
        return ms;
    };
    // The worst case: the middle of the reveal of the search with the most
    // stored entries, from another position: first with its arrays read
    // from the file (before anything else loads them), then from the cache.
    uint32_t big = RV_NONE, big_n = 0, big_cells = 0;
    double cold = 0;
    for (uint32_t k = 0; k < tl.event_count; ++k) {
        if (tl.events[k].kind != RV_EV_SEARCH || !(tl.events[k].duration > 0)) continue;
        rv_search_info si;
        if (rv_search(s, tl.events[k].record, &si) == RV_OK && si.wavefront_length > big_n) {
            big_n = si.wavefront_length;
            big = k;
        }
    }
    const double mid = big != RV_NONE ? tl.events[big].start + 0.4 * tl.events[big].duration : 0; // half the entries
    if (big != RV_NONE) {
        seek(0, nullptr, nullptr);
        cold = seek(mid, &big_cells, nullptr);
    }
    std::vector<double> times, wave_times;
    uint64_t x = 88172645463325252ull;
    uint64_t batches = 0, cells = 0;
    for (uint32_t k = 0; k < seeks; ++k) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const double f = static_cast<double>(x % 1000001) / 1000000.0;
        uint32_t c = 0, b = 0;
        const double ms = seek(f * tl.total, &c, &b);
        times.push_back(ms);
        if (c > 0) wave_times.push_back(ms);
        batches += b;
        cells += c;
    }
    std::sort(times.begin(), times.end());
    std::sort(wave_times.begin(), wave_times.end());
    std::printf("seek:      %u random seeks: median %.3f ms, 90%% %.3f ms, max %.3f ms (%.0f overlay batches on average)\n",
                seeks, times[times.size() / 2], times[times.size() * 9 / 10], times.back(),
                static_cast<double>(batches) / seeks);
    if (!wave_times.empty())
        std::printf("           %zu of them into a wavefront: median %.3f ms, 90%% %.3f ms, max %.3f ms "
                    "(%.0f cells on average)\n",
                    wave_times.size(), wave_times[wave_times.size() / 2], wave_times[wave_times.size() * 9 / 10],
                    wave_times.back(), static_cast<double>(cells) / static_cast<double>(wave_times.size()));
    if (big != RV_NONE) {
        const rv_event &ev = tl.events[big];
        std::vector<double> warm;
        const uint32_t c = big_cells;
        for (int k = 0; k < 20; ++k) {
            seek(k % 2 ? 0 : tl.total, nullptr, nullptr);
            warm.push_back(seek(mid, nullptr, nullptr));
        }
        std::sort(warm.begin(), warm.end());
        std::printf("           largest wavefront (event %u, search %u, %u entries): seek to its middle "
                    "(%u cells drawn) %.3f ms the first time, then median %.3f ms, max %.3f ms\n",
                    big, ev.record, big_n, c, cold, warm[warm.size() / 2], warm.back());
    }
    return 0;
}

// --- M5: the nets table, a net's history, hit tests ---------------------------------

// Display width of UTF-8 text: code points.
std::size_t text_width(const char *t) {
    std::size_t n = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(t); *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

void print_padded(const std::string &t, std::size_t width, bool right) {
    const std::size_t w = text_width(t.c_str());
    const std::string pad(width > w ? width - w : 0, ' ');
    std::printf("%s", right ? (pad + t).c_str() : (t + pad).c_str());
}

// Seek to a fraction of the timeline (1.0: the end) and say where.
void seek_to(rv_session *s, double at) {
    rv_seek_fraction(s, at, nullptr);
    const char *pos = "";
    rv_text(s, RV_TEXT_POSITION, &pos);
    std::printf("position:  %s\n", pos);
}

struct Opts {
    double at = 1.0;
    uint32_t sort = RV_NETCOL_NAME, desc = 0, bench = 0;
    std::string filter;
};

// The nets table as a client shows it: every column, as text from the core.
int nets(rv_session *s, const Opts &o) {
    seek_to(s, o.at);
    if (rv_nets_set_view(s, o.sort, o.desc, o.filter.c_str()) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 2;
    }
    rv_nets_info info;
    if (rv_nets_get(s, &info) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("nets:      %u rows, %u shown, sorted by %s%s\n", info.row_count, info.view_count,
                rv_net_column_title(info.sort_column), info.descending ? " (descending)" : "");
    std::vector<std::vector<std::string>> cells(info.view_count, std::vector<std::string>(RV_NETCOL_COUNT));
    std::vector<std::size_t> width(RV_NETCOL_COUNT);
    for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) width[c] = text_width(rv_net_column_title(c));
    for (uint32_t p = 0; p < info.view_count; ++p)
        for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
            const char *t = "";
            if (rv_nets_cell(s, p, c, &t) == RV_OK) cells[p][c] = t;
            width[c] = std::max(width[c], text_width(cells[p][c].c_str()));
        }
    for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
        if (c) std::printf("  ");
        print_padded(rv_net_column_title(c), width[c], rv_net_column_align(c) == 1);
    }
    std::printf("\n");
    for (uint32_t p = 0; p < info.view_count; ++p) {
        for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
            if (c) std::printf("  ");
            print_padded(cells[p][c], width[c], rv_net_column_align(c) == 1);
        }
        std::printf("\n");
    }
    return 0;
}

// One net by name: the selection, its history and its events.
int net(rv_session *s, const char *name, const Opts &o) {
    seek_to(s, o.at);
    rv_display_list ov;
    const rv_slot *slots = nullptr;
    uint32_t nslots = 0;
    rv_get_overlay(s, &ov, &slots, &nslots);
    const double t0 = now_ms();
    uint32_t changed = 0;
    rv_select_name(s, name, &changed);
    rv_get_overlay(s, &ov, &slots, &nslots);
    const double select_ms = now_ms() - t0;
    rv_selection sel;
    rv_selection_get(s, &sel);
    uint32_t dim = 0, hl = 0;
    for (uint32_t k = 0; k < nslots; ++k) {
        if (slots[k].kind == RV_SLOT_DIM) dim = slots[k].batch_count;
        if (slots[k].kind == RV_SLOT_HIGHLIGHT) hl = slots[k].batch_count;
    }
    std::printf("selection: \"%s\"%s; %u board nets of that name, %u board items; overlay: dim %u, highlight %u "
                "batches; select %.3f ms (with the overlay)\n",
                sel.name, sel.has_net ? (" = router net " + std::to_string(sel.net)).c_str() : " (no router net)",
                sel.board_nets, sel.board_items, dim, hl, select_ms);
    if (!sel.has_net) return 0;
    rv_nets_info info;
    rv_nets_get(s, &info);
    if (info.selected_position != RV_NONE) {
        std::printf("row:      ");
        for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
            const char *t = "";
            rv_nets_cell(s, info.selected_position, c, &t);
            std::printf(" %s %s%s", rv_net_column_title(c), t, c + 1 < RV_NETCOL_COUNT ? "," : "\n");
        }
    }
    uint32_t n = 0;
    if (rv_net_history(s, sel.net, &n) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("history:   %u entries (event numbers 1-based, as shown)\n", n);
    for (uint32_t k = 0; k < n; ++k) {
        rv_history_entry e;
        if (rv_history_get(s, k, &e) != RV_OK) continue;
        char ev[24] = "";
        if (e.event != RV_NONE) std::snprintf(ev, sizeof ev, "%u", e.event + 1);
        std::printf("  %7s  %s%s\n", ev, std::string(e.depth * 2, ' ').c_str(), e.text);
    }
    const uint32_t *events = nullptr;
    uint32_t ne = 0;
    rv_net_events(s, sel.net, &events, &ne);
    std::printf("events:    %u (timeline marks)\n", ne);
    return 0;
}

// Hits at a board point (mm), with an 1600 x 1000 point view fitted to the
// board; with --bench N also the time of N hit tests at pseudo-random view
// points.
int hit(rv_session *s, double x, double y, const Opts &o) {
    rv_view_set_size(s, 1600, 1000, 2);
    rv_view_fit(s);
    seek_to(s, o.at);
    double vx = 0, vy = 0;
    rv_board_to_view(s, x, y, &vx, &vy);
    uint32_t n = 0;
    if (rv_hit_test(s, vx, vy, &n) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        return 1;
    }
    std::printf("hits at (%.3f, %.3f) mm: %u (view %.1f, %.1f; tolerance %.0f points = %.4f mm)\n", x, y, n, vx, vy,
                RV_PICK_POINTS, RV_PICK_POINTS / [&] {
                    rv_view v;
                    rv_view_get(s, &v);
                    return v.scale;
                }());
    for (uint32_t k = 0; k < n; ++k) {
        rv_hit h;
        if (rv_hit_get(s, k, &h) == RV_OK) std::printf("  %u. %s (%.4f mm)\n", k + 1, h.text, h.distance);
    }
    const char *status = "";
    rv_hover_status(s, vx, vy, &status);
    std::printf("hover:     %s\n", status);
    if (o.bench == 0) return 0;
    std::vector<double> times;
    uint64_t r = 88172645463325252ull, total = 0;
    for (uint32_t k = 0; k < o.bench; ++k) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        const double px = static_cast<double>(r % 160001) / 100.0, py = static_cast<double>((r >> 20) % 100001) / 100.0;
        const double t0 = now_ms();
        uint32_t c = 0;
        rv_hit_test(s, px, py, &c);
        times.push_back(now_ms() - t0);
        total += c;
    }
    std::sort(times.begin(), times.end());
    std::printf("bench:     %u hit tests: median %.4f ms, 99%% %.4f ms, max %.4f ms (%.2f hits on average)\n", o.bench,
                times[times.size() / 2], times[times.size() * 99 / 100], times.back(),
                static_cast<double>(total) / o.bench);
    return 0;
}

bool parse_column(const char *spec, Opts *o) {
    const char *colon = std::strchr(spec, ':');
    const std::string name = colon ? std::string(spec, colon) : std::string(spec);
    if (colon && std::strcmp(colon + 1, "desc") != 0 && std::strcmp(colon + 1, "asc") != 0) return false;
    for (uint32_t c = 0; c < RV_NETCOL_COUNT; ++c) {
        std::string t = rv_net_column_title(c);
        for (char &ch : t) ch = ch == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (t == name) {
            o->sort = c;
            o->desc = colon && std::strcmp(colon + 1, "desc") == 0 ? 1 : 0;
            return true;
        }
    }
    return false;
}

} // namespace

// --- M6: following a file being written ------------------------------------------

struct FollowOpts {
    double interval_ms = 250;
    double stall_exit = 0;
    bool quiet = false;
};

int follow(const char *path, const FollowOpts &o) {
    rv_session *s = nullptr;
    if (rv_create(&s) != RV_OK) return 1;
    if (rv_open_ex(s, path, RV_OPEN_WAIT | RV_OPEN_LIVE) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        rv_destroy(s);
        return 1;
    }
    const double t0 = now_ms();
    uint32_t printed = 0, polls = 0, reloads = 0;
    struct Poll {
        double ms;
        uint32_t chunks;
        uint64_t bytes;
    };
    std::vector<Poll> work; // polls that read something
    uint64_t size = 0;
    int rc = 1;
    std::string last_text;
    for (;;) {
        const double a = now_ms();
        uint32_t added = 0, changed = 0;
        const rv_status st = rv_poll(s, (a - t0) / 1000.0, &added, &changed);
        const double ms = now_ms() - a;
        ++polls;
        if (st == RV_E_REPLACED) {
            std::printf("rvdump: %s; reloading\n", rv_last_error(s));
            if (rv_reload(s) != RV_OK) {
                std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
                break;
            }
            ++reloads;
            printed = 0;
            size = 0;
            continue;
        }
        if (st != RV_OK) {
            std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
            break;
        }
        rv_live live;
        rv_live_get(s, &live);
        if (live.file_size != size) {
            work.push_back({ms, added, live.file_size - size});
            size = live.file_size;
        }
        rv_timeline tl;
        rv_get_timeline(s, &tl);
        for (; printed < tl.event_count; ++printed) {
            if (o.quiet) continue;
            const rv_event &e = tl.events[printed];
            rv_chunk_info ci;
            rv_chunk(s, e.chunk, &ci);
            std::printf("event %u  %-16s", printed + 1, rv_event_kind_name(e.kind));
            if ((e.flags & RV_EVF_HAS_NET) != 0) std::printf("  net %d", e.net);
            std::printf("  chunk %u  t %.3f s\n", e.chunk, ci.time);
        }
        const char *text = nullptr;
        rv_live_text(s, &text);
        if (text != nullptr && last_text != text && (live.state != RV_LIVE_GROWING || last_text.empty())) {
            std::printf("rvdump: %s\n", text);
        }
        if (text != nullptr) last_text = text;
        if (live.state == RV_LIVE_COMPLETE) {
            rc = 0;
            break;
        }
        if (live.state == RV_LIVE_STOPPED || live.state == RV_LIVE_FAILED) break;
        if (o.stall_exit > 0 && live.idle_seconds >= o.stall_exit) {
            std::printf("rvdump: gave up after %.1f s without growth\n", live.idle_seconds);
            break;
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(o.interval_ms));
    }
    rv_live live;
    rv_live_get(s, &live);
    std::vector<double> ms;
    double total = 0;
    std::size_t worst = 0;
    for (std::size_t k = 0; k < work.size(); ++k) {
        ms.push_back(work[k].ms);
        total += work[k].ms;
        if (work[k].ms > work[worst].ms) worst = k;
    }
    std::sort(ms.begin(), ms.end());
    auto q = [&](double f) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, static_cast<std::size_t>(f * ms.size()))]; };
    std::printf("rvdump: %u chunks, %u events, %llu bytes; %u polls, %zu that read data, %u reloads, %u follow jumps\n",
                live.chunks, live.events, static_cast<unsigned long long>(live.file_size), polls, work.size(), reloads,
                live.skips);
    if (!work.empty())
        std::printf("rvdump: refresh ms: median %.3f, p90 %.3f, p99 %.3f, max %.3f (%u chunks, %llu bytes); total %.1f\n",
                    q(0.5), q(0.9), q(0.99), ms.back(), work[worst].chunks,
                    static_cast<unsigned long long>(work[worst].bytes), total);
    rv_destroy(s);
    return rc;
}

int main(int argc, char **argv) {
    for (int a = 1; a < argc; ++a)
        if (std::strcmp(argv[a], "--help") == 0 || std::strcmp(argv[a], "-h") == 0) return usage(true);
    // FILE never starts with '-': such an argument is an unknown option.
    if (argc >= 2 && argv[argc - 1][0] == '-') return usage();
    if (argc >= 3 && std::strcmp(argv[1], "--follow") == 0) {
        FollowOpts o;
        int a = 2;
        for (; a < argc - 1; ++a) {
            char *end = nullptr;
            if (std::strcmp(argv[a], "--quiet") == 0) {
                o.quiet = true;
            } else if (std::strcmp(argv[a], "--interval") == 0 && a + 1 < argc - 1) {
                o.interval_ms = std::strtod(argv[++a], &end);
                if (end == argv[a] || *end != '\0' || !(o.interval_ms >= 0)) return usage();
            } else if (std::strcmp(argv[a], "--stall-exit") == 0 && a + 1 < argc - 1) {
                o.stall_exit = std::strtod(argv[++a], &end);
                if (end == argv[a] || *end != '\0' || !(o.stall_exit >= 0)) return usage();
            } else {
                return usage();
            }
        }
        return follow(argv[argc - 1], o);
    }
    enum { Summary, Chunks, Search, Board, Timeline, Nets, Net, Hit } mode = Summary;
    uint32_t search_id = 0;
    rv_pacing pacing = {RV_PACING_FIT, 0, 180.0, 10.0};
    const char *path = nullptr;
    Opts opts;
    const char *net_name = nullptr;
    double hx = 0, hy = 0;
    if (argc >= 3 && (std::strcmp(argv[1], "--nets") == 0 || std::strcmp(argv[1], "--net") == 0 ||
                      std::strcmp(argv[1], "--hit") == 0)) {
        int a = 2;
        if (std::strcmp(argv[1], "--nets") == 0) {
            mode = Nets;
        } else if (std::strcmp(argv[1], "--net") == 0) {
            mode = Net;
            net_name = argv[a++];
        } else {
            mode = Hit;
            char *end = nullptr;
            hx = std::strtod(argv[a], &end);
            if (end == argv[a] || *end != ',') return usage();
            const char *ys = end + 1;
            hy = std::strtod(ys, &end);
            if (end == ys || *end != '\0') return usage();
            ++a;
        }
        for (; a + 1 < argc; a += 2) {
            const char *v = argv[a + 1];
            if (std::strcmp(argv[a], "--at") == 0) {
                char *end = nullptr;
                opts.at = std::strcmp(v, "end") == 0 ? 1.0 : std::strtod(v, &end);
                if (std::strcmp(v, "end") != 0 && (end == v || *end != '\0' || !(opts.at >= 0 && opts.at <= 1)))
                    return usage();
            } else if (std::strcmp(argv[a], "--sort") == 0 && mode == Nets) {
                if (!parse_column(v, &opts)) return usage();
            } else if (std::strcmp(argv[a], "--filter") == 0 && mode == Nets) {
                opts.filter = v;
            } else if (std::strcmp(argv[a], "--bench") == 0 && mode == Hit) {
                char *end = nullptr;
                const unsigned long n = std::strtoul(v, &end, 10);
                if (end == v || *end != '\0' || n > 10000000ul) return usage();
                opts.bench = static_cast<uint32_t>(n);
            } else {
                return usage();
            }
        }
        if (a != argc - 1) return usage();
        path = argv[a];
    } else if (argc == 2 && argv[1][0] != '-') {
        path = argv[1];
    } else if (argc == 3 && std::strcmp(argv[1], "--chunks") == 0) {
        mode = Chunks;
        path = argv[2];
    } else if (argc == 4 && std::strcmp(argv[1], "--search") == 0) {
        mode = Search;
        char *end = nullptr;
        const unsigned long v = std::strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || v > 0xFFFFFFFEul) return usage();
        search_id = static_cast<uint32_t>(v);
        path = argv[3];
    } else if (argc == 4 && std::strcmp(argv[1], "--board") == 0) {
        mode = Board;
        search_id = RV_NONE;
        if (std::strcmp(argv[2], "latest") != 0) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || v > 0xFFFFFFFEul) return usage();
            search_id = static_cast<uint32_t>(v);
        }
        path = argv[3];
    } else if (argc >= 3 && std::strcmp(argv[1], "--timeline") == 0) {
        mode = Timeline;
        search_id = 0;
        for (int a = 2; a + 1 < argc; a += 2) {
            if (std::strcmp(argv[a], "--bench") == 0) {
                char *end = nullptr;
                const unsigned long v = std::strtoul(argv[a + 1], &end, 10);
                if (end == argv[a + 1] || *end != '\0' || v > 1000000ul) return usage();
                search_id = static_cast<uint32_t>(v);
            } else if (std::strcmp(argv[a], "--pacing") == 0) {
                if (!parse_pacing(argv[a + 1], &pacing)) return usage();
            } else {
                return usage();
            }
        }
        if (argc % 2 != 1) return usage();
        path = argv[argc - 1];
    } else {
        return usage();
    }

    rv_session *s = nullptr;
    if (rv_create(&s) != RV_OK) {
        std::fprintf(stderr, "rvdump: cannot create a session\n");
        return 1;
    }
    if (rv_set_pacing(s, &pacing, nullptr) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        rv_destroy(s);
        return 2;
    }
    int rc = 1;
    const double t_open = now_ms();
    const rv_status opened = rv_open(s, path);
    const double open_ms = now_ms() - t_open;
    if (opened != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
    } else {
        rv_trace_info info;
        if (rv_info(s, &info) != RV_OK) {
            std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        } else if (mode == Summary) {
            rc = summary(s, path, info);
        } else if (mode == Chunks) {
            rc = chunks(s, info);
        } else if (mode == Board) {
            rc = board(s, search_id);
        } else if (mode == Timeline) {
            rc = timeline(s, open_ms, search_id);
        } else if (mode == Nets) {
            rc = nets(s, opts);
        } else if (mode == Net) {
            rc = net(s, net_name, opts);
        } else if (mode == Hit) {
            rc = hit(s, hx, hy, opts);
        } else {
            rc = search(s, search_id);
        }
    }
    rv_destroy(s);
    return rc;
}

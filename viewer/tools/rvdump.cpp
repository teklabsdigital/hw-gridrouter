// rvdump: print what the core reads from a .grtrace file.
//
//   rvdump FILE              summary
//   rvdump --chunks FILE     one line per chunk
//   rvdump --search N FILE   one search in detail, with its first trace entries
//   rvdump --board N FILE    board N (or "latest"): layers and display list
//   rvdump --timeline [--bench SEEKS] FILE
//                            the timeline, the state at a few positions, and
//                            optionally the time of SEEKS random seeks
//
// Exit status: 0 for a readable trace with no errors (complete, or a valid
// file that is still being written: "incomplete"); 1 if the file cannot be
// opened or read, is not a trace, or has corrupt parts; 2 for bad usage.
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
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: rvdump FILE\n"
                 "       rvdump --chunks FILE\n"
                 "       rvdump --search N FILE\n"
                 "       rvdump --board N|latest FILE\n"
                 "       rvdump --timeline [--bench SEEKS] FILE\n");
    return 2;
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
    if (seeks == 0) return 0;
    std::vector<double> times;
    uint64_t x = 88172645463325252ull;
    uint32_t batches = 0;
    for (uint32_t k = 0; k < seeks; ++k) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const double f = static_cast<double>(x % 1000001) / 1000000.0;
        const double t0 = now_ms();
        rv_seek_fraction(s, f, nullptr);
        rv_position p;
        rv_position_get(s, &p);
        rv_display_list ov, st;
        const rv_slot *slots = nullptr;
        uint32_t nslots = 0;
        rv_get_overlay(s, &ov, &slots, &nslots);
        rv_get_display_list(s, &st);
        times.push_back(now_ms() - t0);
        batches += ov.batch_count;
    }
    std::sort(times.begin(), times.end());
    std::printf("seek:      %u random seeks: median %.3f ms, 90%% %.3f ms, max %.3f ms (%.0f overlay batches on average)\n",
                seeks, times[times.size() / 2], times[times.size() * 9 / 10], times.back(),
                static_cast<double>(batches) / seeks);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    enum { Summary, Chunks, Search, Board, Timeline } mode = Summary;
    uint32_t search_id = 0;
    const char *path = nullptr;
    if (argc == 2 && argv[1][0] != '-') {
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
    } else if ((argc == 3 || argc == 5) && std::strcmp(argv[1], "--timeline") == 0) {
        mode = Timeline;
        search_id = 0;
        if (argc == 5) {
            if (std::strcmp(argv[2], "--bench") != 0) return usage();
            char *end = nullptr;
            const unsigned long v = std::strtoul(argv[3], &end, 10);
            if (end == argv[3] || *end != '\0' || v > 1000000ul) return usage();
            search_id = static_cast<uint32_t>(v);
        }
        path = argv[argc - 1];
    } else {
        return usage();
    }

    rv_session *s = nullptr;
    if (rv_create(&s) != RV_OK) {
        std::fprintf(stderr, "rvdump: cannot create a session\n");
        return 1;
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
        } else {
            rc = search(s, search_id);
        }
    }
    rv_destroy(s);
    return rc;
}

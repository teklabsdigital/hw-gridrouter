// rvdump: print what the core reads from a .grtrace file.
//
//   rvdump FILE              summary
//   rvdump --chunks FILE     one line per chunk
//   rvdump --search N FILE   one search in detail, with its first trace entries
//
// Exit status: 0 for a readable trace with no errors (complete, or a valid
// file that is still being written: "incomplete"); 1 if the file cannot be
// opened or read, is not a trace, or has corrupt parts; 2 for bad usage.
//
// It uses only the C interface rv.h, like every client, so it also shows
// that the interface is enough to inspect a file.

#include "rv.h"

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
                 "       rvdump --search N FILE\n");
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

} // namespace

int main(int argc, char **argv) {
    enum { Summary, Chunks, Search } mode = Summary;
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
    } else {
        return usage();
    }

    rv_session *s = nullptr;
    if (rv_create(&s) != RV_OK) {
        std::fprintf(stderr, "rvdump: cannot create a session\n");
        return 1;
    }
    int rc = 1;
    if (rv_open(s, path) != RV_OK) {
        std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
    } else {
        rv_trace_info info;
        if (rv_info(s, &info) != RV_OK) {
            std::fprintf(stderr, "rvdump: %s\n", rv_last_error(s));
        } else if (mode == Summary) {
            rc = summary(s, path, info);
        } else if (mode == Chunks) {
            rc = chunks(s, info);
        } else {
            rc = search(s, search_id);
        }
    }
    rv_destroy(s);
    return rc;
}

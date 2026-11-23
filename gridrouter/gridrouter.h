/*
 * gridrouter.h - two-layer PCB maze router core.
 *
 * The router owns six cell grids of nx * ny cells:
 *   value grids  T0 T1 W0 W1  (int32: 0 free, a net number, or RT_BLOCK)
 *   novia                     (uint8: 1 = a via centre may not go here)
 * Grid number g = level * 2 + layer, level 0 = T (thin track), 1 = W (wide
 * track or via), layer 0 = F.Cu, 1 = B.Cu.  g = 4 is novia in the per-net
 * ownership lists.
 *
 * Claims made before rt_snapshot() and every rectangle claim are permanent.
 * After rt_snapshot(), disc / path / via claims by a net 0 < net < RT_BLOCK
 * are also recorded in a per-cell list of owning nets, so rt_rip(net) can
 * remove exactly that net's claims without copying or replaying anything.
 *
 * A cell on a path or in a source / target set is encoded as
 * layer * nc + idx, idx = j * nx + i.
 *
 * Functions returning int return a negative RT_E* code on error.
 */
#ifndef GRIDROUTER_H
#define GRIDROUTER_H

#include <stdint.h>

/* Windows DLLs export only what is marked; elsewhere every function is visible. */
#if defined(_WIN32)
#define RT_API __declspec(dllexport)
#else
#define RT_API
#endif

#define RT_BLOCK (1 << 30)
#define RT_MAXDIRS 16   /* search directions (the reference uses 4 or 8) */
#define RT_MAXSIDE 8192 /* cells per grid side (819 mm at 0.1 mm) */
#define RT_MAXR 255     /* disc radius in cells */

#define RT_EARG (-1)    /* bad argument */
#define RT_ENOMEM (-2)  /* out of memory */
#define RT_ERANGE (-3)  /* a path cost overflowed 31 bits */
#define RT_ESTATE (-4)  /* operation not allowed in this state (e.g. relax before snapshot) */

typedef struct rt_router rt_router;

/* dx, dy: nd direction vectors.  step: 2 * nd costs (layer 0 row, layer 1 row).
 * turn: (nd + 1) * nd extra costs, row = incoming direction (row nd = start of
 * path / after a via), column = outgoing direction, -1 = forbidden.
 * Heuristic towards a goal cell: max(0, h_max * max(dx,dy) + h_min * min(dx,dy) - h_off). */
RT_API rt_router *rt_new(int32_t nx, int32_t ny, int32_t nd, const int32_t *dx, const int32_t *dy,
                         const int32_t *step, const int32_t *turn, int32_t via_cost, int32_t rip_cost,
                         int32_t h_max, int32_t h_min, int32_t h_off);
RT_API void rt_free(rt_router *r);

RT_API int32_t *rt_grid(rt_router *r, int32_t level, int32_t layer);   /* live value grid */
RT_API int32_t *rt_hard(rt_router *r, int32_t level, int32_t layer);   /* permanent (snapshot) grid */
RT_API uint8_t *rt_novia(rt_router *r);
RT_API uint8_t *rt_hard_novia(rt_router *r);

/* Permanent claims. */
RT_API int rt_claim_rect(rt_router *r, int32_t net, int32_t level, int32_t layer,
                         int32_t i0, int32_t j0, int32_t i1, int32_t j1);
RT_API int rt_novia_rect(rt_router *r, int32_t i0, int32_t j0, int32_t i1, int32_t j1);

/* Disc / path / via claims (owned by net after rt_snapshot). */
RT_API int rt_claim_disc(rt_router *r, int32_t net, int32_t layer, int32_t i, int32_t j,
                         int32_t r_t, int32_t r_w);
RT_API int rt_claim_path(rt_router *r, int32_t net, const int32_t *cells, int32_t n,
                         int32_t r_t, int32_t r_w);
RT_API int rt_add_via(rt_router *r, int32_t net, int32_t i, int32_t j, int32_t r_t, int32_t r_w,
                      int32_t r_space);

/* All current claims become permanent; ownership lists are cleared. */
RT_API int rt_snapshot(rt_router *r);
RT_API int rt_snapped(rt_router *r);
/* Remove every claim owned by net since the snapshot. */
RT_API int rt_rip(rt_router *r, int32_t net);
/* Number of (grid, cell) pairs owned by net. */
RT_API int rt_owned_count(rt_router *r, int32_t net);

RT_API int rt_via_ok(rt_router *r, int32_t idx, int32_t net);
/* Distinct nets owning (via disc claims) any of the cells on grid level.
 * Writes up to cap nets to out, returns the total count. */
RT_API int rt_owners(rt_router *r, int32_t level, const int32_t *cells, int32_t n, int32_t *out,
                     int32_t cap);
/* 1 if every cell on level is 0 or net. */
RT_API int rt_path_free(rt_router *r, int32_t level, const int32_t *cells, int32_t n, int32_t net);

/* A* search.  Returns the path length (>= 1), 0 if no path, or < 0 on error.
 * The path is fetched with rt_get_path. */
RT_API int rt_search(rt_router *r, int32_t net, const int32_t *src, int32_t nsrc,
                     const int32_t *tgt, int32_t ntgt, int32_t wide, int32_t has_goal,
                     int32_t gi, int32_t gj, int32_t has_max, int64_t max_cost, int32_t via_goal,
                     int32_t relax, int64_t *out_cost, int32_t *out_new_via, int64_t *out_expanded);
RT_API int rt_get_path(rt_router *r, int32_t *out, int32_t cap);
/* Heap pushes, pops and peak heap size of the last search. */
RT_API void rt_stats(rt_router *r, int64_t *out3);
#ifdef RT_TRACE

/* Search trace, compiled in only with -DRT_TRACE.  While cells != NULL, each
 * rt_search writes layer * nc + idx of every (layer, cell) it expands, once, at
 * its first expansion, in pop order, and the cost so far of that expansion to
 * cost (if not NULL).  Only the first cap entries are written.  cells == NULL
 * turns tracing off.  The buffers must outlive every search while it is on. */
RT_API int rt_trace(rt_router *r, uint32_t *cells, int32_t *cost, int32_t cap);
/* Distinct (layer, cell) expanded by the last search; may exceed cap. */
RT_API int32_t rt_trace_count(rt_router *r);
#endif

#endif

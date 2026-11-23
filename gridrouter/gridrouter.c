/*
 * gridrouter.c - two-layer PCB maze router core.  See gridrouter.h.
 *
 * Search order is bit-for-bit that of the Python reference: heap entries are
 * ordered by (f, d, layer, idx, dir) exactly like the reference's heapq
 * tuples, and no two entries are ever equal, so the pop sequence (and so the
 * path, not just its cost) is independent of the heap implementation.
 */
#include "gridrouter.h"

#include <stdlib.h>
#include <string.h>

typedef struct { int32_t net, next; } rt_node;
typedef struct { uint32_t *a; int32_t n, cap; } rt_vec;
/* Heap entry: f << 96 | d << 64 | ord << 32 | sid, compared as one integer.
 * ord = layer << 31 | j << 18 | i << 5 | dir orders
 * exactly like the reference's (layer, idx, dir) because idx = j * nx + i, i < nx.
 * sid = (layer * nc + idx) * K + dir indexes the state array. */
#if defined(__SIZEOF_INT128__) && !defined(RT_NO_INT128)
typedef unsigned __int128 rt_key;
#define KEY_LT(a, b) ((a) < (b))
#define KEY_F(x) ((uint32_t)((x) >> 96))
#define KEY_D(x) ((uint32_t)((x) >> 64))
#define KEY_ORD(x) ((uint32_t)((x) >> 32))
#define KEY_SID(x) ((uint32_t)(x))
#define HKEY(f, d, ord, sid) (((rt_key)(uint32_t)(f) << 96) | ((rt_key)(uint32_t)(d) << 64) | \
                              ((rt_key)(uint32_t)(ord) << 32) | (rt_key)(uint32_t)(sid))
#else
/* No 128-bit integer (MSVC, 32-bit targets): the same key as two 64-bit halves,
 * hi = f << 32 | d, lo = ord << 32 | sid, compared hi first.  Same order. */
typedef struct { uint64_t hi, lo; } rt_key;
static inline int key_lt(rt_key a, rt_key b)
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}
static inline rt_key hkey(uint32_t f, uint32_t d, uint32_t ord, uint32_t sid)
{
    rt_key k;
    k.hi = (uint64_t)f << 32 | d;
    k.lo = (uint64_t)ord << 32 | sid;
    return k;
}
#define KEY_LT(a, b) key_lt((a), (b))
#define KEY_F(x) ((uint32_t)((x).hi >> 32))
#define KEY_D(x) ((uint32_t)(x).hi)
#define KEY_ORD(x) ((uint32_t)((x).lo >> 32))
#define KEY_SID(x) ((uint32_t)(x).lo)
#define HKEY(f, d, ord, sid) hkey((uint32_t)(f), (uint32_t)(d), (uint32_t)(ord), (uint32_t)(sid))
#endif
typedef struct { rt_key *a; uint32_t n, cap; } rt_bucket;
/* Search state sid = (layer * nc + idx) * K + dir: dist[sid] and prevd[sid], the
 * direction of the predecessor state (its cell follows from this state's own
 * direction), NOPREV for a source. */
#define NOPREV 0xFF
#define ORD(layer, i, j, dr) \
    (((uint32_t)(layer) << 31) | ((uint32_t)(j) << 18) | ((uint32_t)(i) << 5) | (uint32_t)(dr))

struct rt_router {
    int32_t nx, ny, nc, nd, K;
    int32_t dx[RT_MAXDIRS], dy[RT_MAXDIRS];
    int32_t step[2][RT_MAXDIRS];
    int32_t turn[RT_MAXDIRS + 1][RT_MAXDIRS];
    int32_t via_cost, rip_cost, h_max, h_min, h_off;

    int32_t *val[4], *hard[4];
    uint8_t *novia, *hard_novia;
    int snapped, oom;

    int32_t *head[5];                       /* per cell: first ownership node, -1 none */
    rt_node *nodes;
    int32_t nnodes, capnodes, freelist;
    rt_vec *owned;                          /* per net: g * nc + idx it owns */
    int32_t nowned;

    int32_t *dist;                          /* per search state, INT32_MAX = unreached */
    uint8_t *prevd;                         /* per search state: predecessor's direction */
    uint32_t *touched;                      /* states whose dist was set this search */
    size_t ntouched, captouched;
    uint32_t *tmark;
    uint32_t tgen;
    rt_bucket *bk;                          /* priority queue: a binary heap per f < RT_FBUCKETS */
    uint32_t nbk, bcur, bmax;
    size_t hn, nb;                          /* entries in all, in the buckets */
    rt_key *ov;                             /* entries with f >= RT_FBUCKETS: one binary heap */
    uint32_t nov, capov;
    int32_t *path;
    int32_t npath, cappath;
    int64_t stat_push, stat_pop, stat_maxheap;     /* last search */
#ifdef RT_TRACE
    uint32_t *tr_cell;                      /* caller's buffer, NULL = off */
    int32_t *tr_cost;                       /* optional: d (cost so far) of that first expansion */
    int32_t tr_cap, tr_n;                   /* buffer capacity; distinct cells the last search expanded */
    uint32_t *tr_mark;                      /* per (layer, cell): tgen of the search that recorded it */
#endif

    int32_t *disc[RT_MAXR + 1];
    int32_t ndisc[RT_MAXR + 1];
};

static inline int32_t claim_val(int32_t v, int32_t net)
{
    return v == 0 ? net : (v != net ? RT_BLOCK : v);
}

rt_router *rt_new(int32_t nx, int32_t ny, int32_t nd, const int32_t *dx, const int32_t *dy,
                  const int32_t *step, const int32_t *turn, int32_t via_cost, int32_t rip_cost,
                  int32_t h_max, int32_t h_min, int32_t h_off)
{
    if (nx < 1 || ny < 1 || nx > RT_MAXSIDE || ny > RT_MAXSIDE || nd < 1 || nd > RT_MAXDIRS)
        return NULL;
    int64_t nc = (int64_t)nx * ny;
    if (nc * 5 > INT32_MAX || nc * 2 * (nd + 1) > INT32_MAX)
        return NULL;
    rt_router *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->nx = nx;
    r->ny = ny;
    r->nc = (int32_t)nc;
    r->nd = nd;
    r->K = nd + 1;
    for (int k = 0; k < nd; k++) {
        r->dx[k] = dx[k];
        r->dy[k] = dy[k];
        r->step[0][k] = step[k];
        r->step[1][k] = step[nd + k];
    }
    for (int a = 0; a <= nd; a++)
        for (int b = 0; b < nd; b++)
            r->turn[a][b] = turn[a * nd + b];
    r->via_cost = via_cost;
    r->rip_cost = rip_cost;
    r->h_max = h_max;
    r->h_min = h_min;
    r->h_off = h_off;
    r->freelist = -1;
    int ok = 1;
    for (int g = 0; g < 4; g++) {
        r->val[g] = calloc((size_t)nc, sizeof(int32_t));
        r->hard[g] = calloc((size_t)nc, sizeof(int32_t));
        ok &= r->val[g] && r->hard[g];
    }
    r->novia = calloc((size_t)nc, 1);
    r->hard_novia = calloc((size_t)nc, 1);
    ok &= r->novia && r->hard_novia;
    for (int g = 0; g < 5; g++) {
        r->head[g] = malloc((size_t)nc * sizeof(int32_t));
        ok &= r->head[g] != NULL;
        if (r->head[g])
            memset(r->head[g], 0xff, (size_t)nc * sizeof(int32_t));
    }
    r->tmark = calloc((size_t)nc * 2, sizeof(uint32_t));
    ok &= r->tmark != NULL;
    if (!ok) {
        rt_free(r);
        return NULL;
    }
    return r;
}

void rt_free(rt_router *r)
{
    if (!r)
        return;
    for (int g = 0; g < 4; g++) {
        free(r->val[g]);
        free(r->hard[g]);
    }
    for (int g = 0; g < 5; g++)
        free(r->head[g]);
    free(r->novia);
    free(r->hard_novia);
    free(r->nodes);
    for (int32_t k = 0; k < r->nowned; k++)
        free(r->owned[k].a);
    free(r->owned);
    free(r->dist);
    free(r->prevd);
    free(r->touched);
    free(r->tmark);
#ifdef RT_TRACE
    free(r->tr_mark);
#endif
    for (uint32_t f = 0; f < r->nbk; f++)
        free(r->bk[f].a);
    free(r->bk);
    free(r->ov);
    free(r->path);
    for (int k = 0; k <= RT_MAXR; k++)
        free(r->disc[k]);
    free(r);
}

int32_t *rt_grid(rt_router *r, int32_t level, int32_t layer)
{
    if ((level | layer) & ~1)
        return NULL;
    return r->val[level * 2 + layer];
}

int32_t *rt_hard(rt_router *r, int32_t level, int32_t layer)
{
    if ((level | layer) & ~1)
        return NULL;
    return r->hard[level * 2 + layer];
}

uint8_t *rt_novia(rt_router *r) { return r->novia; }
uint8_t *rt_hard_novia(rt_router *r) { return r->hard_novia; }
int rt_snapped(rt_router *r) { return r->snapped; }

/* ---- occupancy ---------------------------------------------------------- */

static const int32_t *get_disc(rt_router *r, int32_t rad, int32_t *n)
{
    if (rad < 0 || rad > RT_MAXR)
        return NULL;
    if (!r->disc[rad]) {
        int32_t side = 2 * rad + 1, cnt = 0;
        int32_t *d = malloc((size_t)side * side * 2 * sizeof(int32_t));
        if (!d)
            return NULL;
        /* reference: dx*dx + dy*dy < (r + 0.5)^2  <=>  <= r*r + r for integers */
        int32_t lim = rad * rad + rad;
        for (int32_t a = -rad; a <= rad; a++)
            for (int32_t b = -rad; b <= rad; b++)
                if (a * a + b * b <= lim) {
                    d[2 * cnt] = a;
                    d[2 * cnt + 1] = b;
                    cnt++;
                }
        r->disc[rad] = d;
        r->ndisc[rad] = cnt;
    }
    *n = r->ndisc[rad];
    return r->disc[rad];
}

static void perm_claim(rt_router *r, int g, int32_t idx, int32_t net)
{
    if (g < 4) {
        r->hard[g][idx] = claim_val(r->hard[g][idx], net);
        r->val[g][idx] = claim_val(r->val[g][idx], net);
    } else {
        r->hard_novia[idx] = 1;
        r->novia[idx] = 1;
    }
}

static int vec_push(rt_vec *v, uint32_t x)
{
    if (v->n == v->cap) {
        int32_t cap = v->cap ? v->cap * 2 : 256;
        uint32_t *a = realloc(v->a, (size_t)cap * sizeof *a);
        if (!a)
            return -1;
        v->a = a;
        v->cap = cap;
    }
    v->a[v->n++] = x;
    return 0;
}

static rt_vec *owned_of(rt_router *r, int32_t net, int create)
{
    if (net >= r->nowned) {
        if (!create)
            return NULL;
        int32_t cap = r->nowned ? r->nowned : 64;
        while (cap <= net)
            cap *= 2;
        rt_vec *o = realloc(r->owned, (size_t)cap * sizeof *o);
        if (!o)
            return NULL;
        memset(o + r->nowned, 0, (size_t)(cap - r->nowned) * sizeof *o);
        r->owned = o;
        r->nowned = cap;
    }
    return &r->owned[net];
}

static void dyn_claim(rt_router *r, int g, int32_t idx, int32_t net)
{
    int32_t *h = &r->head[g][idx];
    int32_t k;
    for (k = *h; k >= 0; k = r->nodes[k].next)
        if (r->nodes[k].net == net)
            return;                         /* already owns it, value already accounts for it */
    rt_vec *v = owned_of(r, net, 1);
    if (!v) {
        r->oom = 1;
        return;
    }
    if (r->freelist >= 0) {
        k = r->freelist;
        r->freelist = r->nodes[k].next;
    } else {
        if (r->nnodes == r->capnodes) {
            int32_t cap = r->capnodes ? r->capnodes * 2 : 65536;
            rt_node *nn = realloc(r->nodes, (size_t)cap * sizeof *nn);
            if (!nn) {
                r->oom = 1;
                return;
            }
            r->nodes = nn;
            r->capnodes = cap;
        }
        k = r->nnodes++;
    }
    if (vec_push(v, (uint32_t)g * (uint32_t)r->nc + (uint32_t)idx)) {
        r->nodes[k].next = r->freelist;
        r->freelist = k;
        r->oom = 1;
        return;
    }
    r->nodes[k].net = net;
    r->nodes[k].next = *h;
    *h = k;
    if (g < 4)
        r->val[g][idx] = claim_val(r->val[g][idx], net);
    else
        r->novia[idx] = 1;
}

static inline void claim(rt_router *r, int g, int32_t idx, int32_t net)
{
    if (r->snapped && net > 0 && net < RT_BLOCK)
        dyn_claim(r, g, idx, net);
    else
        perm_claim(r, g, idx, net);
}

static int finish(rt_router *r)
{
    if (r->oom) {
        r->oom = 0;
        return RT_ENOMEM;
    }
    return 0;
}

int rt_claim_rect(rt_router *r, int32_t net, int32_t level, int32_t layer,
                  int32_t i0, int32_t j0, int32_t i1, int32_t j1)
{
    if ((level | layer) & ~1)
        return RT_EARG;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > r->nx - 1) i1 = r->nx - 1;
    if (j1 > r->ny - 1) j1 = r->ny - 1;
    int g = level * 2 + layer;
    for (int32_t j = j0; j <= j1; j++)
        for (int32_t i = i0; i <= i1; i++)
            perm_claim(r, g, j * r->nx + i, net);
    return 0;
}

int rt_novia_rect(rt_router *r, int32_t i0, int32_t j0, int32_t i1, int32_t j1)
{
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > r->nx - 1) i1 = r->nx - 1;
    if (j1 > r->ny - 1) j1 = r->ny - 1;
    for (int32_t j = j0; j <= j1; j++)
        for (int32_t i = i0; i <= i1; i++)
            perm_claim(r, 4, j * r->nx + i, 1);
    return 0;
}

static int disc_claim(rt_router *r, int32_t net, int g, int32_t i, int32_t j, int32_t rad)
{
    int32_t n;
    const int32_t *d = get_disc(r, rad, &n);
    if (!d)
        return rad < 0 || rad > RT_MAXR ? RT_EARG : RT_ENOMEM;
    int32_t nx = r->nx, ny = r->ny;
    for (int32_t k = 0; k < n; k++) {
        int32_t a = i + d[2 * k], b = j + d[2 * k + 1];
        if (a >= 0 && a < nx && b >= 0 && b < ny)
            claim(r, g, b * nx + a, net);
    }
    return 0;
}

int rt_claim_disc(rt_router *r, int32_t net, int32_t layer, int32_t i, int32_t j,
                  int32_t r_t, int32_t r_w)
{
    if (layer & ~1)
        return RT_EARG;
    int e = disc_claim(r, net, layer, i, j, r_t);
    if (!e)
        e = disc_claim(r, net, 2 + layer, i, j, r_w);
    return e ? e : finish(r);
}

int rt_claim_path(rt_router *r, int32_t net, const int32_t *cells, int32_t n,
                  int32_t r_t, int32_t r_w)
{
    for (int32_t k = 0; k < n; k++) {
        int32_t c = cells[k];
        if (c < 0 || c >= 2 * r->nc)
            return RT_EARG;
        int32_t layer = c / r->nc, idx = c % r->nc;
        int e = rt_claim_disc(r, net, layer, idx % r->nx, idx / r->nx, r_t, r_w);
        if (e)
            return e;
    }
    return 0;
}

int rt_add_via(rt_router *r, int32_t net, int32_t i, int32_t j, int32_t r_t, int32_t r_w,
               int32_t r_space)
{
    int e = rt_claim_disc(r, net, 0, i, j, r_t, r_w);
    if (!e)
        e = rt_claim_disc(r, net, 1, i, j, r_t, r_w);
    if (!e)
        e = disc_claim(r, net, 4, i, j, r_space);   /* via centre spacing */
    return e ? e : finish(r);
}

/* ---- snapshot / rip-up ---------------------------------------------------- */

int rt_snapshot(rt_router *r)
{
    size_t nc = (size_t)r->nc;
    for (int g = 0; g < 4; g++)
        memcpy(r->hard[g], r->val[g], nc * sizeof(int32_t));
    memcpy(r->hard_novia, r->novia, nc);
    for (int g = 0; g < 5; g++)
        memset(r->head[g], 0xff, nc * sizeof(int32_t));
    r->nnodes = 0;
    r->freelist = -1;
    for (int32_t k = 0; k < r->nowned; k++)
        r->owned[k].n = 0;
    r->snapped = 1;
    return 0;
}

int rt_rip(rt_router *r, int32_t net)
{
    rt_vec *v = (net > 0 && net < r->nowned) ? &r->owned[net] : NULL;
    if (!v)
        return 0;
    uint32_t nc = (uint32_t)r->nc;
    for (int32_t e = 0; e < v->n; e++) {
        uint32_t code = v->a[e];
        int g = (int)(code / nc);
        int32_t idx = (int32_t)(code % nc);
        int32_t *p = &r->head[g][idx];
        while (*p >= 0) {
            int32_t k = *p;
            if (r->nodes[k].net == net) {
                *p = r->nodes[k].next;
                r->nodes[k].next = r->freelist;
                r->freelist = k;
                break;
            }
            p = &r->nodes[k].next;
        }
        if (g < 4) {
            int32_t x = r->hard[g][idx];
            for (int32_t k = r->head[g][idx]; k >= 0; k = r->nodes[k].next)
                x = claim_val(x, r->nodes[k].net);
            r->val[g][idx] = x;
        } else {
            r->novia[idx] = (uint8_t)(r->hard_novia[idx] || r->head[4][idx] >= 0);
        }
    }
    int32_t n = v->n;
    v->n = 0;
    if (v->cap > 65536) {                   /* do not keep huge buffers for ripped nets */
        free(v->a);
        v->a = NULL;
        v->cap = 0;
    }
    return n;
}

int rt_owned_count(rt_router *r, int32_t net)
{
    return (net > 0 && net < r->nowned) ? r->owned[net].n : 0;
}

int rt_owners(rt_router *r, int32_t level, const int32_t *cells, int32_t n, int32_t *out,
              int32_t cap)
{
    if (level & ~1)
        return RT_EARG;
    int32_t cnt = 0;
    for (int32_t c = 0; c < n; c++) {
        int32_t code = cells[c];
        if (code < 0 || code >= 2 * r->nc)
            return RT_EARG;
        int32_t layer = code / r->nc, idx = code % r->nc;
        for (int32_t k = r->head[level * 2 + layer][idx]; k >= 0; k = r->nodes[k].next) {
            int32_t net = r->nodes[k].net, seen = 0;
            for (int32_t q = 0; q < cnt && q < cap; q++)
                if (out[q] == net) {
                    seen = 1;
                    break;
                }
            if (!seen) {
                if (cnt < cap)
                    out[cnt] = net;
                cnt++;
            }
        }
    }
    return cnt;
}

int rt_path_free(rt_router *r, int32_t level, const int32_t *cells, int32_t n, int32_t net)
{
    if (level & ~1)
        return RT_EARG;
    for (int32_t c = 0; c < n; c++) {
        int32_t code = cells[c];
        if (code < 0 || code >= 2 * r->nc)
            return RT_EARG;
        int32_t layer = code / r->nc, idx = code % r->nc;
        int32_t v = r->val[level * 2 + layer][idx];
        if (v != 0 && v != net)
            return 0;
    }
    return 1;
}

static inline int via_ok(const rt_router *r, int32_t idx, int32_t net)
{
    if (r->novia[idx])
        return 0;
    int32_t a = r->val[2][idx], b = r->val[3][idx];
    return (a == 0 || a == net) && (b == 0 || b == net);
}

int rt_via_ok(rt_router *r, int32_t idx, int32_t net)
{
    if (idx < 0 || idx >= r->nc)
        return RT_EARG;
    return via_ok(r, idx, net);
}

/* ---- search ----------------------------------------------------------------- */

/* Priority queue of 128-bit keys f << 96 | d << 64 | ord << 32 | sid, popped in
 * increasing key order.  (f, d, ord) is unique per entry, so sid never decides
 * the order and any correct priority queue pops exactly the reference's
 * heapq sequence.
 *
 * Layout: one binary min-heap per f value (bucket[f]), plus a cursor at the
 * lowest possibly non-empty f.  A* pops f in (nearly) increasing order and
 * very many entries share an f, so each heap is shallow.  A push below the
 * cursor (possible with an inconsistent heuristic) just moves the cursor
 * back, so the order is exact for any costs.  Entries with f >= RT_FBUCKETS
 * (only with huge cost constants) go to one overflow binary heap that is
 * popped once the buckets are empty. */
#define RT_FBUCKETS (1u << 20)
static int bucket_reserve(rt_router *r, uint32_t f)
{
    uint32_t cap = r->nbk ? r->nbk : 4096;
    while (cap <= f)
        cap *= 2;
    rt_bucket *b = realloc(r->bk, (size_t)cap * sizeof *b);
    if (!b)
        return -1;
    memset(b + r->nbk, 0, (size_t)(cap - r->nbk) * sizeof *b);
    r->bk = b;
    r->nbk = cap;
    return 0;
}

static int heap_push(rt_key **ha, uint32_t *n, uint32_t *capp, rt_key x)
{
    if (*n == *capp) {
        uint32_t cap = *capp ? *capp * 2 : 64;
        rt_key *a = realloc(*ha, (size_t)cap * sizeof *a);
        if (!a)
            return -1;
        *ha = a;
        *capp = cap;
    }
    rt_key *h = *ha;
    size_t i = (*n)++;
    while (i > 0) {
        size_t p = (i - 1) >> 1;
        if (!KEY_LT(x, h[p]))
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
    return 0;
}

static rt_key heap_pop(rt_key *h, uint32_t *np)
{
    rt_key top = h[0];
    size_t n = --*np;
    if (n == 0)
        return top;
    /* Floyd's variant: move the hole to a leaf along the smaller children
     * (branch-free choice), then sift the last element up from there */
    rt_key last = h[n];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n)
            c += (size_t)KEY_LT(h[c + 1], h[c]);
        h[i] = h[c];
        i = c;
    }
    while (i > 0) {
        size_t p = (i - 1) >> 1;
        if (!KEY_LT(last, h[p]))
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = last;
    return top;
}

static int hpush(rt_router *r, rt_key x)
{
    uint32_t f = KEY_F(x);
    int e;
    if (f < RT_FBUCKETS) {
        if (f >= r->nbk && bucket_reserve(r, f))
            return -1;
        rt_bucket *bk = &r->bk[f];
        e = heap_push(&bk->a, &bk->n, &bk->cap, x);
        if (!e) {
            if (f < r->bcur)
                r->bcur = f;
            if (f > r->bmax)
                r->bmax = f;
            r->nb++;
        }
    } else {
        e = heap_push(&r->ov, &r->nov, &r->capov, x);
    }
    if (e)
        return -1;
    r->hn++;
    r->stat_push++;
    if (r->hn > (size_t)r->stat_maxheap)
        r->stat_maxheap = (int64_t)r->hn;
    return 0;
}

/* Caller guarantees r->hn > 0.  Every bucket key is below every overflow key. */
static rt_key hpop(rt_router *r)
{
    r->hn--;
    r->stat_pop++;
    if (r->nb == 0)
        return heap_pop(r->ov, &r->nov);
    while (r->bk[r->bcur].n == 0)
        r->bcur++;
    r->nb--;
    return heap_pop(r->bk[r->bcur].a, &r->bk[r->bcur].n);
}

/* Empty the queue (buckets keep their memory). */
static void hclear(rt_router *r)
{
    for (uint32_t f = r->bcur; f <= r->bmax && f < r->nbk; f++)
        r->bk[f].n = 0;
    r->hn = r->nb = 0;
    r->nov = 0;
    r->bcur = UINT32_MAX;
    r->bmax = 0;
}

static int grow_touched(rt_router *r)
{
    size_t cap = r->captouched ? r->captouched * 2 : 65536;
    uint32_t *t = realloc(r->touched, cap * sizeof *t);
    if (!t)
        return -1;
    r->touched = t;
    r->captouched = cap;
    return 0;
}

int rt_search(rt_router *r, int32_t net, const int32_t *src, int32_t nsrc,
              const int32_t *tgt, int32_t ntgt, int32_t wide, int32_t has_goal,
              int32_t gi, int32_t gj, int32_t has_max, int64_t max_cost, int32_t via_goal,
              int32_t relax, int64_t *out_cost, int32_t *out_new_via, int64_t *out_expanded)
{
    const int32_t nc = r->nc, nx = r->nx, ny = r->ny, K = r->K, START = r->nd;
    const int64_t h_max = r->h_max, h_min = r->h_min, h_off = r->h_off;
    r->npath = 0;
#ifdef RT_TRACE
    r->tr_n = 0;
#endif
    if (relax && !r->snapped)
        return RT_ESTATE;
    for (int32_t k = 0; k < ntgt; k++)
        if (tgt[k] < 0 || tgt[k] >= 2 * nc)
            return RT_EARG;
    for (int32_t k = 0; k < nsrc; k++)
        if (src[k] < 0 || src[k] >= 2 * nc)
            return RT_EARG;
    if (!r->dist) {
        size_t ns = (size_t)nc * 2 * K;
        r->dist = malloc(ns * sizeof(int32_t));
        r->prevd = malloc(ns);
        if (!r->dist || !r->prevd) {
            free(r->dist);
            free(r->prevd);
            r->dist = NULL;
            r->prevd = NULL;
            return RT_ENOMEM;
        }
        for (size_t k = 0; k < ns; k++)
            r->dist[k] = INT32_MAX;         /* INT32_MAX = not reached */
    }
    if (++r->tgen == 0) {
        memset(r->tmark, 0, (size_t)nc * 2 * sizeof(uint32_t));
#ifdef RT_TRACE
        if (r->tr_mark)
            memset(r->tr_mark, 0, (size_t)nc * 2 * sizeof(uint32_t));
#endif
        r->tgen = 1;
    }
    const uint32_t tgen = r->tgen;
    const uint32_t *tmark = r->tmark;
#ifdef RT_TRACE
    uint32_t *const tr_cell = r->tr_cell, *const tr_mark = r->tr_mark;
    int32_t *const tr_cost = r->tr_cost;
    const int32_t tr_cap = r->tr_cap;
    int32_t tr_n = 0;
#endif
    int32_t *const dist = r->dist;
    uint8_t *const prevd = r->prevd;
    const int lev = wide ? 1 : 0;
    const int32_t *const g[2] = {r->val[lev * 2], r->val[lev * 2 + 1]};
    const int32_t *const hd[2] = {r->hard[lev * 2], r->hard[lev * 2 + 1]};
    const int32_t rip_cost = r->rip_cost, via_cost = r->via_cost;
    /* local copies: stores into dist[] must not force reloads of these */
    int32_t ddx[RT_MAXDIRS], ddy[RT_MAXDIRS], step2[2][RT_MAXDIRS], turn2[RT_MAXDIRS + 1][RT_MAXDIRS];
    memcpy(ddx, r->dx, sizeof ddx);
    memcpy(ddy, r->dy, sizeof ddy);
    memcpy(step2, r->step, sizeof step2);
    memcpy(turn2, r->turn, sizeof turn2);
    int64_t expanded = 0, found = -1;
    int32_t new_via = 0;
    int rc = 0;

/* reference: max(0, 10 * max(dx, dy) + 4 * min(dx, dy) - 30), or 10 * (dx + dy) - 30 */
#define HEUR(a, b, out)                                                  \
    do {                                                                 \
        if (has_goal) {                                                  \
            int32_t hx_ = (a) - gi, hy_ = (b) - gj;                      \
            if (hx_ < 0) hx_ = -hx_;                                     \
            if (hy_ < 0) hy_ = -hy_;                                     \
            int64_t hv_ = hx_ > hy_ ? h_max * hx_ + h_min * hy_          \
                                    : h_max * hy_ + h_min * hx_;         \
            hv_ -= h_off;                                                \
            (out) = hv_ > 0 ? hv_ : 0;                                   \
        } else {                                                         \
            (out) = 0;                                                   \
        }                                                                \
    } while (0)
/* relax state key to cost nd from direction dr, pushing (f, nd, ord) */
#define RELAX(key, nd, f, ord)                                                \
    do {                                                                      \
        if ((nd) >= INT32_MAX || (f) >= INT32_MAX) {                          \
            rc = RT_ERANGE;                                                   \
            goto done;                                                        \
        }                                                                     \
        if (dist[key] == INT32_MAX) {                                         \
            if (r->ntouched == r->captouched && grow_touched(r)) {            \
                rc = RT_ENOMEM;                                               \
                goto done;                                                    \
            }                                                                 \
            r->touched[r->ntouched++] = (key);                                \
        }                                                                     \
        dist[key] = (int32_t)(nd);                                            \
        if (hpush(r, HKEY((f), (nd), (ord), (key)))) {                        \
            rc = RT_ENOMEM;                                                   \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    for (int32_t k = 0; k < ntgt; k++)
        r->tmark[tgt[k]] = tgen;
    hclear(r);
    r->stat_push = r->stat_pop = r->stat_maxheap = 0;
    for (int32_t k = 0; k < nsrc; k++) {
        int32_t c = src[k];
        int32_t layer = c / nc, idx = c % nc;
        int32_t v = g[layer][idx];
        if (v == 0 || v == net || (relax && (hd[layer][idx] == 0 || hd[layer][idx] == net))) {
            uint32_t sid = (uint32_t)c * K + START;
            if (dist[sid] != INT32_MAX)
                continue;                   /* duplicate source */
            int32_t i = idx % nx, j = idx / nx;
            int64_t h;
            HEUR(i, j, h);
            prevd[sid] = NOPREV;
            RELAX(sid, 0, h, ORD(layer, i, j, START));
        }
    }

    while (r->hn) {
        const rt_key e = hpop(r);
        const int32_t d = (int32_t)KEY_D(e);
        const uint32_t esid = KEY_SID(e);
        if (dist[esid] < d)
            continue;                       /* stale entry */
        expanded++;
        const uint32_t o = KEY_ORD(e);
        const int32_t layer = (int32_t)(o >> 31), j = (int32_t)((o >> 18) & 0x1FFF),
                      i = (int32_t)((o >> 5) & 0x1FFF), dr = (int32_t)(o & 31);
        const int32_t idx = j * nx + i, ls = layer * nc + idx;
#ifdef RT_TRACE
        if (tr_cell && tr_mark[ls] != tgen) {   /* first expansion of (layer, cell) */
            tr_mark[ls] = tgen;
            if (tr_n < tr_cap) {
                tr_cell[tr_n] = (uint32_t)ls;
                if (tr_cost)
                    tr_cost[tr_n] = d;
            }
            tr_n++;
        }
#endif
        const int is_t = tmark[ls] == tgen;
        if (via_goal) {
            if (is_t || via_ok(r, idx, net)) {
                found = esid;
                new_via = !is_t;
                break;
            }
        } else if (is_t) {
            found = esid;
            break;
        }
        if (has_max && d > max_cost)
            break;
        const int32_t *gl = g[layer], *hl = hd[layer];
        const int32_t *turn = turn2[dr], *step = step2[layer];
        for (int32_t q = 0; q < START; q++) {
            int32_t t = turn[q];
            if (t < 0)
                continue;
            const int32_t a = i + ddx[q], b = j + ddy[q];
            if (a < 0 || a >= nx || b < 0 || b >= ny)
                continue;
            const int32_t n = b * nx + a;
            const int32_t v = gl[n];
            if (v != 0 && v != net) {
                if (!relax)
                    continue;
                const int32_t hv = hl[n];
                if (hv != 0 && hv != net)
                    continue;
                t += rip_cost;
            }
            const int64_t nd = (int64_t)d + step[q] + t;
            const uint32_t key = (uint32_t)(ls + (n - idx)) * (uint32_t)K + (uint32_t)q;
            if (nd >= INT32_MAX) {          /* before the test: INT32_MAX means unreached */
                rc = RT_ERANGE;
                goto done;
            }
            if (nd < dist[key]) {
                int64_t f;
                HEUR(a, b, f);
                f += nd;
                prevd[key] = (uint8_t)dr;
                RELAX(key, nd, f, ORD(layer, a, b, q));
            }
        }
        if (!via_goal && via_ok(r, idx, net)) {
            const int32_t other = 1 - layer;
            const int32_t v = g[other][idx];
            const int fr = v == 0 || v == net;
            if (fr || (relax && (hd[other][idx] == 0 || hd[other][idx] == net))) {
                const int64_t nd = (int64_t)d + via_cost + (fr ? 0 : rip_cost);
                const uint32_t key = (uint32_t)(other * nc + idx) * (uint32_t)K + (uint32_t)START;
                if (nd >= INT32_MAX) {
                    rc = RT_ERANGE;
                    goto done;
                }
                if (nd < dist[key]) {
                    int64_t f;
                    HEUR(i, j, f);
                    f += nd;
                    prevd[key] = (uint8_t)dr;
                    RELAX(key, nd, f, ORD(other, i, j, START));
                }
            }
        }
    }
#undef HEUR
#undef RELAX

done:
#ifdef RT_TRACE
    r->tr_n = tr_n;
#endif
    if (out_expanded)
        *out_expanded = expanded;
    if (rc == 0 && found >= 0) {
        /* walk back: a state with direction q < START came from the cell one step
         * back along q on the same layer, a START state from the other layer */
        int32_t len = 0;
        for (int64_t s = found;;) {
            len++;
            uint8_t p = prevd[s];
            if (p == NOPREV)
                break;
            int64_t ls = s / K, q = s % K;
            int64_t layer = ls / nc, idx = ls % nc;
            if (q < START)
                idx -= ddx[q] + (int64_t)ddy[q] * nx;
            else
                layer = 1 - layer;
            s = (layer * nc + idx) * K + p;
        }
        if (len > r->cappath) {
            int32_t *pp = realloc(r->path, (size_t)len * sizeof *pp);
            if (!pp) {
                rc = RT_ENOMEM;
                len = 0;
            } else {
                r->path = pp;
                r->cappath = len;
            }
        }
        if (len) {
            int32_t k = len;
            for (int64_t s = found;;) {
                r->path[--k] = (int32_t)(s / K);
                uint8_t p = prevd[s];
                if (p == NOPREV)
                    break;
                int64_t ls = s / K, q = s % K;
                int64_t layer = ls / nc, idx = ls % nc;
                if (q < START)
                    idx -= ddx[q] + (int64_t)ddy[q] * nx;
                else
                    layer = 1 - layer;
                s = (layer * nc + idx) * K + p;
            }
            r->npath = len;
            if (out_cost)
                *out_cost = dist[found];
            if (out_new_via)
                *out_new_via = new_via;
        }
    }
    hclear(r);
    for (size_t k = 0; k < r->ntouched; k++)       /* leave dist[] all unreached */
        dist[r->touched[k]] = INT32_MAX;
    r->ntouched = 0;
    if (rc < 0)
        return rc;
    return found >= 0 ? r->npath : 0;
}

int rt_get_path(rt_router *r, int32_t *out, int32_t cap)
{
    int32_t n = r->npath < cap ? r->npath : cap;
    memcpy(out, r->path, (size_t)n * sizeof *out);
    return r->npath;
}

void rt_stats(rt_router *r, int64_t *out3)
{
    out3[0] = r->stat_push;
    out3[1] = r->stat_pop;
    out3[2] = r->stat_maxheap;
}
#ifdef RT_TRACE

/* ---- search trace (only with -DRT_TRACE) ------------------------------------ */

int rt_trace(rt_router *r, uint32_t *cells, int32_t *cost, int32_t cap)
{
    if (!r || cap < 0)
        return RT_EARG;
    if (!cells) {                           /* off; tr_mark stays allocated */
        r->tr_cell = NULL;
        r->tr_cost = NULL;
        r->tr_cap = 0;
        return 0;
    }
    if (!r->tr_mark) {                      /* zeroed: tgen is never 0 during a search */
        r->tr_mark = calloc((size_t)r->nc * 2, sizeof(uint32_t));
        if (!r->tr_mark)
            return RT_ENOMEM;
    }
    r->tr_cell = cells;
    r->tr_cost = cost;
    r->tr_cap = cap;
    return 0;
}

int32_t rt_trace_count(rt_router *r)
{
    return r ? r->tr_n : RT_EARG;
}
#endif

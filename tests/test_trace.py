# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital

"""The search trace hook, the recorder and the trace file format.

1. The C trace equals the oracle's first-pop order and costs, exactly, on
   random boards (ref_router's heapq is replaced in memory by a spy for each
   oracle search; tests/ref_router.py is not edited).
2. Trace invariants (check_trace), on every search of these tests and on
   hand-made bad traces; buffer overflow with guard values; tracing off again.
3. Router and TracedRouter (no recorder, and with a recorder) give identical
   results over whole route / rip-up runs and identical exceptions.
4. Format: writer -> reader round trip of every chunk type and of BoardWriter,
   the worked example of the spec byte for byte, unknown tags and records,
   trailing bytes, corrupt chunks, every prefix of a small file, sampled
   prefixes of a larger one, following a growing file.
5. Replaying the recorded copper (spec section 8) gives the router's edges,
   vias, stubs and edge widths, at the end and after every route_net; ROUT
   equals routes(); the emission rules of spec section 5 on a hand-made run.
6. The reader decisions of 2026-11-23 on hand-made files: RIP before the first
   SNAP, corrupt RUN, id resynchronisation, error searches with bad cells, vias
   outside the grid, RIP of another net's copper; check()'s errors and
   warnings and the check command's exit status.

The traced library must build: if it cannot, these tests fail (they are not
skipped).

Run: python3 tests/test_trace.py   (GRIDROUTER_TEST_SCALE=0.2 for a quick run)
"""
import array
import copy
import ctypes
import heapq
import inspect
import math
import os
import random
import shutil
import struct
import tempfile
import unittest
from types import SimpleNamespace

from common import (SCALE, apply, bookkeeping, first_diff, grids, gridrouter, random_ops,
                    random_path, ref, rng_for)
from gridrouter import recorder
from gridrouter import tracefile as tf
from gridrouter.recorder import Recorder, TracedRouter, check_trace
from test_ripup import make_board, route_board
from test_search import random_search

STATS = {}
NONE32 = tf.NONE32
TMP = None


def setUpModule():
    global TMP
    TMP = tempfile.mkdtemp(prefix="gridrouter_test_trace_")


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


def tmp(name):
    return os.path.join(TMP, name)


# -- helpers --------------------------------------------------------------------------
class HeapSpy:
    """Stands in for the heapq module inside ref_router during one oracle search.

    The oracle sets dist[key] = d and pushes (f, d, layer, idx, dir) with
    key = (layer, idx, dir) in the same statement pair, for sources and for
    every relaxation, so the dict kept here equals the oracle's dist at every
    pop.  A pop is stale exactly when dist[key] < d (the oracle's own test);
    the first non-stale pop of each (layer, idx) is what the C hook records
    (it sits after the C's own stale test, `dist[esid] < d`).  (A stale entry
    of a state is always popped after that state's live entry, which has the
    same h and a smaller d, so the first pop of a cell is never stale; the
    stale test is applied anyway, exactly as both searches do.)"""

    def __init__(self, nc):
        self.nc = nc
        self.dist = {}
        self.seen = set()
        self.trace = []
        self.expanded = 0

    def heappush(self, heap, item):
        self.dist[item[2:]] = item[1]
        heapq.heappush(heap, item)

    def heappop(self, heap):
        item = heapq.heappop(heap)
        _, d, layer, idx, dr = item
        if not self.dist[(layer, idx, dr)] < d:
            self.expanded += 1
            if (layer, idx) not in self.seen:
                self.seen.add((layer, idx))
                self.trace.append((layer * self.nc + idx, d))
        return item


def oracle_search(pr, nc, args):
    net, src, tgt, wide, goal, mc, vg, rl = args
    spy = HeapSpy(nc)
    saved = ref.heapq
    ref.heapq = spy
    try:
        res = pr.search(net, src, tgt, wide, goal=goal, max_cost=mc, via_goal=vg, relax=rl)
    finally:
        ref.heapq = saved
    return res, spy


def norm(x):
    """Comparable plain data: namespaces -> dicts, arrays / tuples -> lists, NaN -> 'nan'."""
    if isinstance(x, SimpleNamespace):
        return {k: norm(v) for k, v in vars(x).items()}
    if isinstance(x, dict):
        return {k: norm(v) for k, v in x.items()}
    if isinstance(x, (list, tuple, array.array)):
        return [norm(v) for v in x]
    if isinstance(x, (bytes, bytearray)):
        return bytes(x)
    if isinstance(x, float) and math.isnan(x):
        return "nan"
    if isinstance(x, bool):
        return int(x)
    return x


def router_state(r):
    """Copper bookkeeping in the form replay() gives."""
    edges = {n: {l: set(s) for l, s in d.items() if s} for n, d in r.edges.items()}
    edges = {n: d for n, d in edges.items() if d}
    vias = {n: set(s) for n, s in r.vias.items() if s}
    stubs = {n: sorted(s) for n, s in r.stubs.items() if s}
    return edges, vias, stubs, dict(r.ewidth)


def replay_state(st):
    edges = {n: {l: set(s) for l, s in d.items() if s} for n, d in st.edges.items()}
    edges = {n: d for n, d in edges.items() if d}
    vias = {n: set(s) for n, s in st.vias.items() if s}
    stubs = {n: sorted(x[:5] for x in s) for n, s in st.stubs.items() if s}
    return edges, vias, stubs, dict(st.ewidth)


def chunks_of(trace, tag):
    return [c.data for c in trace.chunks if c.tag == tag]


class LogMixin:
    """Logs every search result (or exception) of a router."""

    def search(self, *a, **kw):
        ev = self.__dict__.setdefault("events", [])
        try:
            res = super().search(*a, **kw)
        except Exception as e:
            ev.append(("error", type(e).__name__, str(e)))
            raise
        ev.append(("search", res, self.last_cost, self.last_expanded))
        return res


class LogRouter(LogMixin, gridrouter.Router):
    pass


class LogTraced(LogMixin, TracedRouter):
    pass


# -- 1. oracle equivalence ----------------------------------------------------------------
class TraceVsOracle(unittest.TestCase):
    def run_config(self, strict, n_scenes, n_search, seed0):
        n_cases = entries = found = 0
        for sc in range(n_scenes):
            rng = rng_for(seed0 + sc)
            nx, ny = rng.randint(25, 90), rng.randint(20, 70)
            ref.configure(nx, ny, strict=strict)
            pr = ref.Router()
            cr = TracedRouter(nx, ny, 0.1, strict=strict, check=True)
            n_nets = rng.randint(2, 8)
            ops = random_ops(rng, nx, ny, ref.DIRS, n_nets, post_snapshot=rng.random() < 0.8)
            apply(pr, ops)
            apply(cr, ops)
            self.assertIsNone(first_diff(grids(pr), grids(cr), nx))
            nc = nx * ny
            for q in range(n_search):
                args = random_search(rng, nx, ny, n_nets, pr, True)
                (p_ref, nv_ref), spy = oracle_search(pr, nc, args)
                net, src, tgt, wide, goal, mc, vg, rl = args
                p_c, nv_c = cr.search(net, src, tgt, wide, goal=goal, max_cost=mc, via_goal=vg,
                                      relax=rl)
                where = "strict=%s scene %d search %d" % (strict, sc, q)
                # the spy's mirror of dist is right: its non-stale pops are the oracle's count
                self.assertEqual(spy.expanded, pr.last_expanded, where)
                self.assertEqual((p_ref, nv_ref, pr.last_cost, pr.last_expanded),
                                 (p_c, nv_c, cr.last_cost, cr.last_expanded), where)
                cells, costs, total = cr.last_trace
                self.assertEqual(total, len(spy.trace), where)
                got = list(zip(cells, costs))
                if got != spy.trace:
                    k = next(k for k in range(min(len(got), len(spy.trace)))
                             if got[k] != spy.trace[k])
                    self.fail("%s: trace differs at entry %d: C %r, oracle %r"
                              % (where, k, got[k], spy.trace[k]))
                n_cases += 1
                entries += total
                found += p_c is not None
        STATS.setdefault("oracle", []).append((strict, n_scenes, n_cases, found, entries))

    def test_non_strict(self):
        self.run_config(False, max(4, int(120 * SCALE)), 20, 1000)

    def test_strict(self):
        self.run_config(True, max(4, int(120 * SCALE)), 20, 5000)

    def test_route_net_searches(self):
        """The searches route_net and drop_to_plane issue (tree sources, relax, via_goal)."""
        n = 0
        for k in range(max(2, int(6 * SCALE))):
            strict = k % 2 == 1
            rng = rng_for(41000 + k)
            nx, ny = rng.randint(60, 100), rng.randint(40, 70)
            pads, blocks = make_board(41000 + k, strict, nx, ny, rng.randint(5, 9))
            ref.configure(nx, ny, strict=strict, max_rips=25)
            pr = ref.Router()
            cr = TracedRouter(nx, ny, 0.1, strict=strict, max_rips=25, check=True)
            calls = []
            orig = ref.Router.search

            def spy_search(self, net, sources, targets, wide, goal=None, max_cost=None,
                           via_goal=False, relax=False):
                args = (net, set(sources), set(targets), wide, goal, max_cost, via_goal, relax)
                saved = ref.heapq
                spy = HeapSpy(nx * ny)
                ref.heapq = spy
                try:
                    res = orig(self, *args[:4], goal=goal, max_cost=max_cost, via_goal=via_goal,
                               relax=relax)
                finally:
                    ref.heapq = saved
                calls.append((res, spy.trace))
                return res
            ref.Router.search = spy_search
            try:
                route_board(pr, nx, pads, blocks)
            finally:
                ref.Router.search = orig
            traces = []

            class Grab(TracedRouter):
                def search(self, *a, **kw):
                    res = super().search(*a, **kw)
                    c, d, _ = self.last_trace
                    traces.append((res, list(zip(c, d))))
                    return res
            cr = Grab(nx, ny, 0.1, strict=strict, max_rips=25, check=True)
            route_board(cr, nx, pads, blocks)
            self.assertEqual(len(calls), len(traces))
            for a, b in zip(calls, traces):
                self.assertEqual(a, b)
            n += len(calls)
        STATS["oracle_route"] = n


# -- 2. invariants, overflow, off ---------------------------------------------------------
class Invariants(unittest.TestCase):
    def test_checker_rejects_bad_traces(self):
        nc = 100
        ok = dict(nc=nc, cells=[5, 6, 7], costs=[0, 10, 20], total=3, cap=200)
        check_trace(**ok)
        check_trace(path=[(0, 5), (0, 6), (0, 7)], cost=20, expanded=3, **ok)
        bad = [dict(ok, cells=[5, 6, 5]),                       # a cell twice
               dict(ok, costs=[3, 10, 20]),                     # first entry not a source
               dict(ok, cells=[5, 6, 200]),                     # out of range
               dict(ok, total=2),                               # count / stored mismatch
               dict(ok, total=201, cap=3),                      # count > 2 nc
               dict(ok, costs=[0, -1, 20])]                     # negative cost
        for b in bad:
            with self.assertRaises(AssertionError, msg=repr(b)):
                check_trace(**b)
        with self.assertRaises(AssertionError):                 # count > expanded
            check_trace(expanded=2, **ok)
        with self.assertRaises(AssertionError):                 # found cell not last
            check_trace(path=[(0, 5), (0, 6)], cost=10, **ok)
        with self.assertRaises(AssertionError):                 # last cost != path cost
            check_trace(path=[(0, 5), (0, 6), (0, 7)], cost=19, **ok)
        with self.assertRaises(AssertionError):                 # a path cell missing
            check_trace(path=[(0, 5), (0, 9), (0, 7)], cost=20, **ok)
        with self.assertRaises(AssertionError):                 # d > max_cost before the end
            check_trace(max_cost=5, **ok)
        check_trace(max_cost=10, **ok)                          # only the last pop may exceed
        check_trace(**dict(ok, cells=[5, 6], costs=[0, 10], total=3, cap=2), max_cost=10)
        with self.assertRaises(AssertionError):                 # truncated: all stored <= max
            check_trace(**dict(ok, cells=[5, 6], costs=[0, 10], total=3, cap=2), max_cost=9)

    def _board(self):
        r = TracedRouter(60, 40, 0.1)
        nx = r.NX
        r.add_pad(1, (0,), 0.8, 0.8, 0.6, 0.6, 1.0, 1.0, 0)
        r.block_rect(2.0, 0.5, 2.4, 3.0)
        src = {(0, 8 * nx + 8)}
        tgt = {(0, 30 * nx + 50)}
        return r, src, tgt

    def test_overflow_and_off(self):
        L = recorder.load_traced()
        r, src, tgt = self._board()
        nc = r.NC
        self.assertFalse(r._tr_on)              # no recorder, no check: tracing off
        full_c = (ctypes.c_uint32 * (2 * nc))()
        full_d = (ctypes.c_int32 * (2 * nc))()
        self.assertEqual(L.rt_trace(r._r, ctypes.addressof(full_c), ctypes.addressof(full_d),
                                    2 * nc), 0)
        res = r.search(1, src, tgt, False, goal=(50, 30))
        total = L.rt_trace_count(r._r)
        self.assertGreater(total, 100)
        self.assertIsNotNone(res[0])
        check_trace(nc, list(full_c[:total]), list(full_d[:total]), total, 2 * nc, path=res[0],
                    cost=r.last_cost, expanded=r.last_expanded)
        GUARD, GC, GD = 64, 0xDEADBEEF, 0x5A5A5A5A
        for cap in (0, 1, total // 3, total - 1, total, total + 5):
            cells = (ctypes.c_uint32 * (cap + GUARD))(*([GC] * (cap + GUARD)))
            cost = (ctypes.c_int32 * (cap + GUARD))(*([GD] * (cap + GUARD)))
            self.assertEqual(L.rt_trace(r._r, ctypes.addressof(cells), ctypes.addressof(cost),
                                        cap), 0)
            res2 = r.search(1, src, tgt, False, goal=(50, 30))
            self.assertEqual(res2, res)
            self.assertEqual(L.rt_trace_count(r._r), total)
            n = min(cap, total)
            self.assertEqual(list(cells[:n]), list(full_c[:n]), "cap %d" % cap)
            self.assertEqual(list(cost[:n]), list(full_d[:n]), "cap %d" % cap)
            self.assertEqual(list(cells[n:]), [GC] * (cap + GUARD - n), "cap %d: written past" % cap)
            self.assertEqual(list(cost[n:]), [GD] * (cap + GUARD - n), "cap %d: written past" % cap)
        # cost buffer NULL: cells only
        cells = (ctypes.c_uint32 * (total + GUARD))(*([GC] * (total + GUARD)))
        self.assertEqual(L.rt_trace(r._r, ctypes.addressof(cells), None, total), 0)
        r.search(1, src, tgt, False, goal=(50, 30))
        self.assertEqual(list(cells[:total]), list(full_c[:total]))
        self.assertEqual(list(cells[total:]), [GC] * GUARD)
        # off again: nothing written, count 0
        for k in range(len(cells)):
            cells[k] = GC
        self.assertEqual(L.rt_trace(r._r, None, None, 0), 0)
        self.assertEqual(r.search(1, src, tgt, False, goal=(50, 30)), res)
        self.assertEqual(L.rt_trace_count(r._r), 0)
        self.assertEqual(list(cells), [GC] * len(cells))
        # bad arguments
        self.assertEqual(L.rt_trace(None, ctypes.addressof(cells), None, 4), -1)
        self.assertEqual(L.rt_trace(r._r, ctypes.addressof(cells), None, -1), -1)
        self.assertEqual(L.rt_trace_count(None), -1)
        del full_c, full_d                      # tracing is off: nothing points at them

    def test_small_cap_in_file(self):
        """A trace buffer smaller than the count: SRCH holds the first ntrace entries."""
        r = TracedRouter(60, 40, 0.1, check=True, trace_cap=50)
        nx = r.NX
        path = tmp("cap.grtrace")
        with Recorder(path) as rec:
            rec.attach(r)
            res = r.search(1, {(0, 8 * nx + 8)}, {(0, 30 * nx + 50)}, False, goal=(50, 30))
            full = TracedRouter(60, 40, 0.1, check=True)
            full.search(1, {(0, 8 * nx + 8)}, {(0, 30 * nx + 50)}, False, goal=(50, 30))
        self.assertIsNotNone(res[0])
        s = chunks_of(tf.read_trace(path), "SRCH")[0]
        self.assertEqual(s.ntrace, 50)
        self.assertGreater(s.ntrace_total, 50)
        self.assertEqual(list(s.trace_cell), list(full.last_trace[0][:50]))
        self.assertEqual(list(s.trace_d), list(full.last_trace[1][:50]))

    def test_trace_off_without_recorder(self):
        r = TracedRouter(30, 20, 0.1)
        self.assertFalse(r._tr_on)
        r.search(1, {(0, 5 * 30 + 5)}, {(0, 10 * 30 + 20)}, False)
        self.assertIsNone(r.last_trace)
        path = tmp("onoff.grtrace")
        rec = Recorder(path)
        rec.attach(r)
        self.assertTrue(r._tr_on)
        rec.close()
        self.assertFalse(r._tr_on)
        self.assertIsNone(r._rec)


# -- 3. identical results -----------------------------------------------------------------
class SameResults(unittest.TestCase):
    def test_route_runs(self):
        boards = steps = 0
        for k in range(max(2, int(10 * SCALE))):
            strict = k % 2 == 1
            rng = rng_for(42000 + k)
            nx, ny = rng.randint(70, 130), rng.randint(50, 90)
            pads, blocks = make_board(42000 + k, strict, nx, ny, rng.randint(6, 14))
            runs = []
            for mode in ("plain", "traced", "recorded"):
                if mode == "plain":
                    r = LogRouter(nx, ny, 0.1, strict=strict, max_rips=25)
                else:
                    r = LogTraced(nx, ny, 0.1, strict=strict, max_rips=25, check=True)
                rec = None
                if mode == "recorded":
                    rec = Recorder(tmp("same%d.grtrace" % k))
                    rec.attach(r)
                trace = []

                def chk(step, name, ripped, r=r):
                    trace.append((step, name, list(ripped), grids(r), bookkeeping(r), list(r.log),
                                  list(r.failed), r.rips, r.routes(), dict(r.width)))
                route_board(r, nx, pads, blocks, chk)
                # direct calls after routing: rip-ups, commits, vias
                names = sorted(r.log)
                r.rebuild(names[:2] + ["no such net"])
                if names:
                    r.rip(r.log[names[-1]][0][0] if names[-1] in r.log else 1)
                r.commit(3, [(0, 20 * nx + 20), (0, 20 * nx + 21), (1, 20 * nx + 21)], True, 1,
                         True)
                r.add_via(3, 40, 30, 0)
                r.add_via(3, 40, 30, 0)
                trace.append(("end", grids(r), bookkeeping(r), list(r.log), list(r.failed), r.rips,
                              r.routes(), dict(r.width), {n: dict(c) for n, c in r.padcell.items()}))
                if rec:
                    rec.close()
                runs.append((trace, r.events))
            self.assertEqual(runs[0][1], runs[1][1], "board %d: searches, traced" % k)
            self.assertEqual(runs[0][1], runs[2][1], "board %d: searches, recorded" % k)
            self.assertEqual(runs[0][0], runs[1][0], "board %d: state, traced" % k)
            self.assertEqual(runs[0][0], runs[2][0], "board %d: state, recorded" % k)
            boards += 1
            steps += len(runs[0][0])
        STATS["same"] = (boards, steps)

    def test_random_ops(self):
        n = max(10, int(100 * SCALE))
        for k in range(n):
            rng = rng_for(43000 + k)
            strict = k % 2 == 1
            nx, ny = rng.randint(25, 70), rng.randint(20, 60)
            ops = random_ops(rng, nx, ny, gridrouter.make_dirs(strict), 6)
            more = [("rebuild", ([rng.choice(["A", "B", "C"]) for _ in range(rng.randint(0, 3))],)),
                    ("rip", (rng.randint(1, 6),))]
            a = gridrouter.Router(nx, ny, 0.1, strict=strict)
            b = TracedRouter(nx, ny, 0.1, strict=strict, check=True)
            rec = Recorder(tmp("ops%d.grtrace" % k))
            rec.attach(b)
            for r in (a, b):
                apply(r, ops)                   # ends with snapshot(), which clears log
                r.log = {"A": [(1, None, False, 0)], "B": [(2, None, False, 0)], "C": []}
                apply(r, more)
            rec.close()
            self.assertIsNone(first_diff(grids(a), grids(b), nx), "case %d" % k)
            self.assertEqual(bookkeeping(a), bookkeeping(b), "case %d" % k)
            self.assertEqual((a.log, a.rips, a.failed), (b.log, b.rips, b.failed))

    def test_exceptions(self):
        def run(cls, rec_path=None):
            out = []
            r = cls(40, 30, 0.1, via_cost=2 ** 31 - 5)
            rec = None
            if rec_path:
                rec = Recorder(rec_path)
                rec.attach(r)
            nx = r.NX
            cases = [
                lambda: r.search(1, {(0, 100)}, {(0, 200)}, False, relax=True),
                lambda: r.search(1, {(0, 10 ** 6)}, {(0, 200)}, False),
                lambda: r.search(1, {(0, 15 * nx + 10)}, {(1, 15 * nx + 30)}, False),
                lambda: r.search(1, [(0,)], {(0, 200)}, False),
                lambda: r.search(1, {(0, 100)}, {(0, 200)}, False, goal=("x", 1)),
                lambda: r.search(1, {(0, 100)}, {(0, 200)}, False, max_cost="x"),
                lambda: r.search(1, {(1, 2 ** 40)}, {(0, 200)}, False),
                lambda: r.commit(1, [(0, 10 ** 7), (0, 10 ** 7 + 1)], False, 0),
                lambda: r.route_net(1, "n", [(1.0, 1.0, (0,), [(0, 100)], "a"),
                                             (2.0, 2.0, (0,), [(0, 10 ** 6)], "b")], False, 0),
                lambda: r.drop_to_plane(1, "g", (1.0, 1.0, (0,), [(0, 10 ** 6)], "p"), set()),
            ]
            for c in cases:
                try:
                    out.append(("ok", c()))
                except Exception as e:
                    out.append((type(e).__name__, str(e)))
            out.append((grids(r), bookkeeping(r), r.failed))
            if rec:
                rec.close()
            return out
        plain = run(gridrouter.Router)
        self.assertEqual(plain, run(TracedRouter))
        path = tmp("exc.grtrace")
        self.assertEqual(plain, run(lambda *a, **kw: TracedRouter(*a, check=True, **kw), path))
        tr = tf.read_trace(path)
        self.assertEqual(tr.errors, [])
        self.assertEqual(tf.check(tr), [])
        srch = chunks_of(tr, "SRCH")
        err = [s for s in srch if s.flags & tf.S_ERROR]
        self.assertGreaterEqual(len(err), 7)
        for s in err:
            self.assertEqual((s.cost, s.expanded, s.flags & tf.S_FOUND), (-1, -1, 0))
        # the cost overflow happens mid-search: a partial trace is stored
        self.assertTrue(any(s.ntrace > 0 and s.flags & tf.S_HAS_TRACE for s in err))
        # exceptions before the C call (bad goal, bad max_cost, bad cell tuple): no trace
        self.assertTrue(any(not s.flags & tf.S_HAS_TRACE for s in err))
        self.assertEqual([d.status for d in chunks_of(tr, "RNE ")], [1])
        self.assertEqual([d.ok for d in chunks_of(tr, "DROP")], [2])

    def test_signatures(self):
        names = ("search", "route_net", "drop_to_plane", "commit", "add_via", "rip", "rebuild",
                 "snapshot", "add_pad", "claim_rect", "block_rect", "block_cells", "claim_disc")
        for n in names:
            self.assertEqual(inspect.signature(getattr(TracedRouter, n)),
                             inspect.signature(getattr(gridrouter.Router, n)), n)
            self.assertIsNot(getattr(TracedRouter, n), getattr(gridrouter.Router, n), n)


# -- 4. format ----------------------------------------------------------------------------------
def sample_fields():
    """Fields for one chunk of every tag (the writer's input; the reader must give them back)."""
    dirs = gridrouter.make_dirs(False)
    nd = len(dirs)
    nc = 6 * 5
    rng = random.Random(5)
    board = tf.BoardWriter("placed", 1.6, (100000000, -2500000))
    f_cu = board.layer("F.Cu", 0, 0)
    b_cu = board.layer("B.Cu", 2, 0)
    edge = board.layer("Edge.Cuts", 25)
    board.stackup_item("F.Cu", 0.035)
    board.stackup_item("dielectric 1", 1.51, 4.5)
    board.stackup_item("B.Mask", 0.01, None)
    board.net(1, "GND")
    board.net(2, "naïve ✓")
    fp = board.footprint("U1", "MCU", "Lib:QFN-32", 1000, -2000, 90.0, True)
    board.footprint("R1", "", "R:0402", 0, 0)
    board.polyset(f_cu, [[[(0, 0), (10, 0), (10, 10), (0, 10)], [(2, 2), (3, 2), (3, 3)]],
                         [[(20, 20), (30, 20), (25, 28)]]], net=1, fp=fp, role=0)
    board.polyset(edge, [[[(0, 0), (50, 0), (50, 36), (0, 36), (0, 0)]]], role=5, sub=100000)
    board.polyset(b_cu, [], role=3)
    board.hole(5, 6, 300000, 300000, 0.0, True, 0, fp, 1)
    board.hole(-5, 6, 600000, 300000, 45.0, False, 1, -1, -1)
    board.padinfo(fp, 1, 5, 6, 0, "A3")
    board.track(f_cu, 2, 0, 0, 1000, 1000, 200000)
    board.via(1000, 1000, 600000, 300000, 2, f_cu, b_cu)
    board.text(edge, -1, 3, 4, -30.5, "rev A")
    board.raw_record(99, b"\x01\x02\x03")
    return {
        "RUN ": dict(nx=6, ny=5, block=tf.BLOCK, via_cost=250, rip_cost=150, max_rips=400,
                     h_max=10, h_min=4, h_off=30, r_track=[3, 5], r_wide=[5, 7], r_via=[5, 7],
                     r_via_space=8, flags=6, cell=0.1, origin_x=12.5, origin_y=-3.25, track=0.2,
                     track_wide=0.5, pad_clear=[0.3, 0.5], pad_novia=0.4, edge_margin=[0.45, 0.65],
                     via_dia=0.6, via_drill=0.3, dirs=[tuple(d) for d in dirs],
                     step=gridrouter.make_step(dirs, 1.6, 1.2),
                     turn=[[-1 if t is None else t for t in row]
                           for row in gridrouter.make_turn(dirs, 12, 40)],
                     producer="test é", layer_name=["F.Cu", "B.Cu"], nd=nd),
        "NETS": dict(entries=[(1, "GND"), (2, ""), (-7, "x" * 13)]),
        "PAD ": dict(net=3, layers=3, power=1.5, cx=1.0, cy=2.0, x0=0.5, y0=1.5, x1=1.5, y1=2.5,
                     d_t=0.45, d_w=0.65, d_novia=0.4),
        "RECT": dict(op=2, net=tf.BLOCK, layers=2, levels=1, x0=1.0, y0=2.0, x1=3.0, y1=4.0,
                     d_t=0.0, d_w=0.0),
        "DISC": dict(net=4, layer=1, i=-3, j=7, r_t=5, r_w=7),
        "VIA ": dict(copper_id=NONE32, net=2, i=3, j=4, power=1, added=0, ctx=0,
                     route_id=NONE32),
        "SNAP": dict(),
        "GRDS": dict(which=1, nc=nc, label="lbl",
                     T0=[rng.choice([0, 1, tf.BLOCK]) for _ in range(nc)],
                     T1=[rng.randrange(-5, 5) for _ in range(nc)], W0=list(range(nc)),
                     W1=[tf.BLOCK] * nc, novia=bytes(rng.randrange(3) for _ in range(nc))),
        "RNB ": dict(route_id=0, net=5, wide=1, power=3,
                     pads=[(1.25, 2.5, 1, 4), (3.0, -1.0, 3, 0)], name="N5", labels=["N5.0", "N5.1"]),
        "RNE ": dict(route_id=0, net=5, rips_after=2, status=0, ripped=["A", "B", "A"],
                     failed=["N5.1"]),
        "SRCH": dict(search_id=0, route_id=0, ctx=1, net=5,
                     flags=tf.S_HAS_GOAL | tf.S_FOUND | tf.S_HAS_COSTS | tf.S_HAS_TRACE, gi=3, gj=4,
                     rips_at_call=1, max_cost=-1, cost=42, expanded=17, duration_s=0.001,
                     src=[1, 2, 2], tgt=[59, 0xFFFFFFFF], path=[1, 7, 13], trace_cell=[1, 2, 7, 13],
                     trace_d=[0, 0, 14, 42], ntrace_total=4, repeat_of=NONE32, enc=0, ntrace=4,
                     nsrc=3, ntgt=2, npath=3),
        "CMIT": dict(copper_id=0, net=5, flags=3, power=1, ctx=1, route_id=0, width_mm=0.5,
                     stubs=[(0, 1.25, 2.5, 1.3, 2.5, 0.5), (1, 0.0, 0.0, 0.0, 0.1, 0.5)],
                     vias=[(1, 1), (-2, 3)], path=[1, 7, 37]),
        "RBLD": dict(route_id=NONE32, nets=[5, -1, 6], names=["N5", "zz", "N6"]),
        "RIP ": dict(net=5, ctx=1, route_id=0, removed=[0, 3, 9]),
        "DROP": dict(net=1, ok=2, max_cost=600, name="GND", pad_label="GND.3"),
        "MARK": dict(level=1, text="note ✓"),
        "ROUT": dict(segs=[(0, 5, 0.1, 0.2, 0.3, 0.4, 0.5), (1, 6, 1.0, 1.0, 2.0, 2.0, 0.2)],
                     vias=[(0.5, 0.6, 5)]),
        "BRD ": board,
        "END ": dict(searches=1, routes=1, rips=2, expanded_total=17, trace_entries=4, wall_s=1.5,
                     failed=[("N5", "N5.1"), ("GND", "")], nfailed=2),
    }


def board_fields(bw):
    return dict(thickness_mm=bw.thickness_mm, origin_x_nm=bw.origin_x_nm,
                origin_y_nm=bw.origin_y_nm, label=bw.label, layers=bw.layers, stackup=bw.stackup,
                nets=bw.nets, footprints=bw.footprints, records=bw.records)


class Format(unittest.TestCase):
    def test_worked_example(self):
        """Section 13 of the spec, byte for byte."""
        expect = bytes.fromhex(
            "4752545241434500" "0100000000000000" "00001080e0c0da41" "4d41524b0c000000"
            "000000000000f83f" "0000000003000000" "43414e0000000000")
        path = tmp("example.grtrace")
        w = tf.TraceWriter(path, open_time=1795392000.25)
        w.chunk("MARK", tf.pack("MARK", dict(level=0, text="CAN")), t=1.5)
        w.close()
        with open(path, "rb") as f:
            self.assertEqual(f.read(), expect)
        tr = tf.read_trace(expect)
        self.assertEqual(tr.header.open_time, 1795392000.25)
        self.assertEqual([(c.tag, c.t, c.data.level, c.data.text) for c in tr.chunks],
                         [("MARK", 1.5, 0, "CAN")])
        self.assertFalse(tr.complete)
        self.assertEqual(tr.off, 0x38)

    def test_round_trip_every_chunk(self):
        fields = sample_fields()
        self.assertEqual(set(fields), set(tf.TAGS))
        path = tmp("all.grtrace")
        w = tf.TraceWriter(path)
        for tag in tf.TAGS:                  # RUN first, END last: the spec's order
            w.chunk(tag, tf.pack(tag, fields[tag]))
        w.close()
        tr = tf.read_trace(path)
        self.assertTrue(tr.complete)
        self.assertEqual(tr.errors, [])
        self.assertEqual([c.tag for c in tr.chunks], list(tf.TAGS))
        prev = 0.0
        with open(path, "rb") as f:
            raw = f.read()
        for c in tr.chunks:
            self.assertEqual(c.offset % 8, 0)
            self.assertGreaterEqual(c.t, prev)
            prev = c.t
            want = fields[c.tag]
            if c.tag == "BRD ":
                want = board_fields(want)
            got = norm(c.data)
            for k, v in want.items():
                self.assertEqual(got[k], norm(v), "%s.%s" % (c.tag, k))
            # the payload re-packs to the same bytes
            src = c.data if c.tag != "BRD " else tf.BoardWriter.from_fields(c.data)
            self.assertEqual(tf.pack(c.tag, src), raw[c.offset + 16:c.offset + 16 + c.length],
                             c.tag)
        STATS["format"] = len(tr.chunks)

    def test_board_layout(self):
        """BRD alignment rules: stackup / footprint entries and records 8-aligned."""
        bw = sample_fields()["BRD "]
        p = bw.payload()
        tr = tf.read_trace(self._file([("BRD ", p)]))
        d = tr.chunks[0].data
        self.assertEqual([r.kind for r in d.records], [1, 1, 1, 2, 2, 3, 4, 5, 6, 99])
        self.assertTrue(math.isnan(d.stackup[2][1]))
        self.assertEqual(struct.pack("<d", d.stackup[2][1]), struct.pack("<Q", 0x7FF8000000000000))
        self.assertEqual(d.records[-1].raw, b"\x01\x02\x03\0\0\0\0\0")
        # walk the record list by hand: every header at a multiple of 8
        c = tf._Cur(p)
        c.st(tf._BRD)
        c.str()
        for _ in range(c.u32()):
            c.i32(), c.i32(), c.str()
        for _ in range(c.u32()):
            c.align8()
            self.assertEqual(c.o % 8, 0)
            c.raw(16), c.str()
        for _ in range(c.u32()):
            c.i32(), c.str()
        for _ in range(c.u32()):
            c.align8()
            self.assertEqual(c.o % 8, 0)
            c.raw(24), c.str(), c.str(), c.str()
        n = c.u32()
        c.align8()
        for _ in range(n):
            self.assertEqual(c.o % 8, 0)
            c.u32()
            c.raw(c.u32())
        self.assertEqual(c.o, len(p))

    def _file(self, chunks, header=True):
        b = bytearray()
        if header:
            b += tf.HEADER.pack(tf.MAGIC, 1, 0, 0, 0.0)
        for tag, payload in chunks:
            b += tf.CHUNK.pack(tag.encode() if isinstance(tag, str) else tag, len(payload), 0.0)
            b += payload + b"\0" * (-len(payload) % 8)
        return bytes(b)

    def test_unknown_trailing_corrupt(self):
        f = sample_fields()
        run = tf.pack("RUN ", f["RUN "])
        data = self._file([
            ("RUN ", run),
            ("ZZZ9", b"whatever it is"),                       # unknown tag: skipped
            ("MARK", tf.pack("MARK", dict(level=0, text="x")) + b"later minor fields"),
            ("PAD ", b"\0" * 40),                               # too short: corrupt, skipped
            ("RNE ", struct.pack("<IiiIII", 0, 1, 0, 5, 0, 0)),  # count runs past len
            ("GRDS", tf.pack("GRDS", dict(f["GRDS"], nc=30))),  # nc 30 = 6 x 5: fine
            ("GRDS", tf.pack("GRDS", dict(which=0, nc=4, label="", T0=[0] * 4, T1=[0] * 4,
                                          W0=[0] * 4, W1=[0] * 4, novia=b"\0" * 4))),  # wrong nc
            ("SRCH", tf.pack("SRCH", dict(f["SRCH"], enc=7, ntrace=3, trace_raw=b"abcdefgh"))),
            ("DISC", tf.pack("DISC", f["DISC"])),
        ])
        tr = tf.read_trace(data)
        self.assertEqual([c.tag for c in tr.chunks],
                         ["RUN ", "ZZZ9", "MARK", "PAD ", "RNE ", "GRDS", "GRDS", "SRCH", "DISC"])
        self.assertIsNone(tr.chunks[1].data)
        self.assertIsNone(tr.chunks[1].error)
        self.assertEqual(tr.chunks[2].data.text, "x")
        self.assertIsNotNone(tr.chunks[3].error)
        self.assertIsNotNone(tr.chunks[4].error)
        self.assertIsNotNone(tr.chunks[5].data)
        self.assertIsNotNone(tr.chunks[6].error)
        s = tr.chunks[7].data
        self.assertEqual((list(s.path), len(s.trace_cell), s.trace_raw), ([1, 7, 13], 0, b"abcdefgh"))
        self.assertEqual(tr.chunks[8].data.r_w, 7)
        self.assertEqual(len(tr.errors), 3)
        # a non-printable tag: stop there, report, never read further
        bad = data + self._file([(b"MA\x01K", b""), ("MARK", tf.pack("MARK", dict(level=0,
                                                                                  text="y")))],
                                header=False)
        tr = tf.read_trace(bad)
        self.assertEqual(len(tr.chunks), 9)
        self.assertTrue(tr.stopped)
        self.assertEqual(len(tr.errors), 4)
        with self.assertRaises(ValueError):
            tf.read_trace(b"GRTRACX\0" + data[8:])
        with self.assertRaises(ValueError):
            tf.read_trace(tf.HEADER.pack(tf.MAGIC, 2, 0, 0, 0.0))
        minor = tf.read_trace(tf.HEADER.pack(tf.MAGIC, 1, 7, 99, 0.0) + data[24:])
        self.assertEqual(len(minor.chunks), 9)

    def _prefix_check(self, data, offsets):
        full = tf.read_trace(data)
        self.assertTrue(full.complete)
        self.assertEqual(full.errors, [])
        sig = [(c.tag, c.offset, c.length, c.t) for c in full.chunks]
        ends = [c.offset + 16 + c.length + (-c.length % 8) for c in full.chunks]
        for n in offsets:
            tr = tf.read_trace(data[:n])
            self.assertEqual(tr.errors, [], "prefix %d" % n)
            got = [(c.tag, c.offset, c.length, c.t) for c in tr.chunks]
            self.assertEqual(got, sig[:len(got)], "prefix %d" % n)
            k = sum(1 for e in ends if e <= n)
            self.assertEqual(len(got), k if n >= 24 else 0, "prefix %d" % n)
            self.assertEqual(tr.complete, n >= len(data), "prefix %d" % n)
            for a, b in zip(tr.chunks, full.chunks):
                self.assertEqual(a.data is None, b.data is None)
        return len(offsets)

    def test_every_prefix_small(self):
        data = small_trace_bytes()
        self.assertLess(len(data), 40000)
        n = self._prefix_check(data, range(len(data) + 1))
        STATS["prefix_small"] = (len(data), n)

    def test_sampled_prefixes_large(self):
        path = tmp("large.grtrace")
        nx, ny = 100, 70
        pads, blocks = make_board(40001, False, nx, ny, 8)
        r = TracedRouter(nx, ny, 0.1, max_rips=25)
        with Recorder(path) as rec:
            rec.attach(r)
            route_board(r, nx, pads, blocks)
            rec.routes()
        with open(path, "rb") as f:
            data = f.read()
        full = tf.read_trace(data)
        rng = random.Random(3)
        offs = set(rng.randrange(len(data) + 1) for _ in range(max(40, int(200 * SCALE))))
        for c in full.chunks[::max(1, len(full.chunks) // 40)]:
            offs.update((c.offset - 1, c.offset, c.offset + 1, c.offset + 15, c.offset + 16))
        offs.update((0, 23, 24, len(data) - 1, len(data)))
        n = self._prefix_check(data, sorted(o for o in offs if 0 <= o <= len(data)))
        STATS["prefix_large"] = (len(data), n)

    def test_follow_growing_file(self):
        data = small_trace_bytes()
        path = tmp("grow.grtrace")
        rng = random.Random(9)
        with open(path, "wb") as f:
            rd = None
            pos = 0
            while pos < len(data):
                step = rng.randint(1, 300)
                f.write(data[pos:pos + step])
                f.flush()
                pos += step
                if rd is None:
                    rd = tf.TraceReader(path)
                else:
                    rd.refresh()
                self.assertEqual(rd.errors, [])
        rd.refresh()
        full = tf.read_trace(data)
        self.assertTrue(rd.complete)
        self.assertEqual([(c.tag, c.offset) for c in rd.chunks],
                         [(c.tag, c.offset) for c in full.chunks])
        self.assertEqual(norm([c.data for c in rd.chunks if c.tag != "BRD "]),
                         norm([c.data for c in full.chunks if c.tag != "BRD "]))


def small_trace_bytes():
    """A small complete trace (a few KB) with most chunk types, for the prefix tests."""
    path = tmp("small_prefix.grtrace")
    if not os.path.exists(path):
        # 18 x 14 cells, no edge frame, B.Cu blocked: B must cross A, rips it
        r = TracedRouter(18, 14, 0.1, max_rips=2, check=True, edge_margin=None)
        nx = r.NX
        bw = tf.BoardWriter("placed")
        bw.layer("F.Cu", 0, 0)
        bw.polyset(0, [[[(0, 0), (100, 0), (100, 100)]]], role=0)
        with Recorder(path) as rec:
            rec.board(bw)
            rec.attach(r, nets={1: "A", 2: "B"})
            rec.phase("pads")
            r.add_pad(1, (0,), 0.3, 0.7, 0.25, 0.65, 0.35, 0.75, 0)
            r.add_pad(1, (0,), 1.4, 0.7, 1.35, 0.65, 1.45, 0.75, 0)
            r.block_cells(1, 0, 0, 17, 13)
            r.snapshot()
            pads = [(0.3, 0.7, (0,), [(0, 7 * nx + 3)], "A.0"),
                    (1.4, 0.7, (0,), [(0, 7 * nx + 14)], "A.1")]
            r.route_net(1, "A", pads, False, 0)
            pads = [(0.9, 0.1, (0,), [(0, 1 * nx + 9)], "B.0"),
                    (0.9, 1.2, (0,), [(0, 12 * nx + 9)], "B.1")]
            r.route_net(2, "B", pads, False, 0)
            rec.mark("done")
            rec.routes()
        tr = tf.read_trace(path)
        assert [d.ripped for d in chunks_of(tr, "RNE ")] == [[], ["A"]], "small board: no rip-up"
    with open(path, "rb") as f:
        return f.read()


# -- 5. replay, emission rules ------------------------------------------------------------------
class Replay(unittest.TestCase):
    def test_route_runs(self):
        boards = checked = 0
        for k in range(max(2, int(8 * SCALE))):
            strict = k % 2 == 1
            rng = rng_for(44000 + k)
            nx, ny = rng.randint(70, 130), rng.randint(50, 90)
            pads, blocks = make_board(44000 + k, strict, nx, ny, rng.randint(6, 14))
            r = TracedRouter(nx, ny, 0.1, strict=strict, max_rips=25, check=True)
            r.skip_joined_stubs = k % 3 == 2
            path = tmp("replay%d.grtrace" % k)
            states = []
            with Recorder(path) as rec:
                rec.attach(r, nets={n: "N%d" % n for n in range(1, 20)})

                def chk(step, name, ripped):
                    states.append(copy.deepcopy(router_state(r)))
                route_board(r, nx, pads, blocks, chk)
                rec.routes()
                final = router_state(r)
                failed, rips = list(r.failed), r.rips
            tr = tf.read_trace(path)
            self.assertEqual(tr.errors, [])
            self.assertEqual(tf.check(tr), [], "board %d" % k)
            self.assertTrue(tr.complete)
            self.assertEqual(replay_state(tf.replay(tr)), final, "board %d" % k)
            rne = [i for i, c in enumerate(tr.chunks) if c.tag == "RNE "]
            self.assertEqual(len(rne), len(states))
            for i, st in zip(rne, states):
                self.assertEqual(replay_state(tf.replay(tr, upto=i)), st, "board %d chunk %d"
                                 % (k, i))
                checked += 1
            rout = chunks_of(tr, "ROUT")[-1]
            segs, vias = r.routes()
            self.assertEqual(rout.segs, [(l, n, x0, y0, x1, y1, w)
                                         for l, x0, y0, x1, y1, w, n in segs])
            self.assertEqual(rout.vias, vias)
            end = chunks_of(tr, "END ")[0]
            self.assertEqual(end.failed, [(str(a), str(b)) for a, b in failed])
            self.assertEqual(end.rips, rips)
            boards += 1
        STATS["replay"] = (boards, checked)

    def test_random_direct_calls(self):
        """Commits and vias before and after snapshot(), rips and rebuilds called directly."""
        n = max(10, int(120 * SCALE))
        for k in range(n):
            rng = rng_for(45000 + k)
            strict = k % 2 == 1
            nx, ny = rng.randint(30, 80), rng.randint(25, 60)
            dirs = gridrouter.make_dirs(strict)
            ops = random_ops(rng, nx, ny, dirs, 5, post_snapshot=rng.random() < 0.7)
            pre = []
            if rng.random() < 0.3:              # rip and rebuild before any snapshot
                pre = [("rip", (1,)), ("rebuild", (["X", "Z"],))]
            k_snap = ops.index(("snapshot", ()))
            ops = pre + ops[:k_snap] + [("snapshot", ()), ("setlog", ())] + ops[k_snap + 1:]
            for _ in range(rng.randint(0, 8)):
                c = rng.random()
                if c < 0.4:
                    ops.append(("commit", (rng.randint(1, 5), random_path(rng, nx, ny, dirs,
                                                                         rng.randint(2, 30)),
                                           rng.random() < 0.3, rng.choice([0, 1]),
                                           rng.random() < 0.3)))
                elif c < 0.6:
                    ops.append(("add_via", (rng.randint(1, 5), rng.randrange(nx), rng.randrange(ny),
                                            0)))
                elif c < 0.8:
                    ops.append(("rip", (rng.randint(1, 5),)))
                elif c < 0.9:
                    ops += [("snapshot", ()), ("setlog", ())]
                else:
                    ops.append(("rebuild", ([rng.choice("XYZ") for _ in range(rng.randint(0, 3))],)))
            r = TracedRouter(nx, ny, 0.1, strict=strict, check=True)
            path = tmp("direct%d.grtrace" % k)
            with Recorder(path) as rec:
                rec.attach(r)
                r.log = {"X": [(1, None, False, 0)], "Y": [(2, None, False, 0)], "Z": []}
                for op in ops:
                    if op[0] == "setlog":       # snapshot() clears the log
                        r.log = {"X": [(1, None, False, 0)], "Y": [(2, None, False, 0)], "Z": []}
                    else:
                        apply(r, [op])
            tr = tf.read_trace(path)
            self.assertEqual(tf.check(tr), [], "case %d" % k)
            self.assertEqual(replay_state(tf.replay(tr)), router_state(r), "case %d" % k)

    def test_attach_after_snapshot(self):
        """Router snapshotted before attaching: rip still removes this session's copper."""
        r = TracedRouter(40, 30, 0.1)
        nx = r.NX
        r.commit(1, [(0, 10 * nx + 10), (0, 10 * nx + 11)], False, 0)
        r.snapshot()
        path = tmp("late.grtrace")
        with Recorder(path) as rec:
            rec.attach(r)
            r.commit(1, [(0, 12 * nx + 10), (0, 12 * nx + 11), (1, 12 * nx + 11)], False, 0)
            r.rip(1)
        tr = tf.read_trace(path)
        self.assertEqual([d.removed for d in chunks_of(tr, "RIP ")], [[0]])
        self.assertEqual(tf.check(tr), [])
        self.assertEqual(tf.replay(tr).alive, {})

    def test_emission_rules(self):
        r = TracedRouter(50, 40, 0.1, max_rips=3)
        nx = r.NX
        path = tmp("rules.grtrace")
        rec = Recorder(path)
        rec.mark("before attach")
        rec.attach(r, nets={1: "A"}, origin=(10.0, 20.0))
        r.rip(1)                                    # no snapshot yet: nremoved 0
        r.add_pad(1, (0,), 1.0, 1.0, 0.8, 0.8, 1.2, 1.2, 1)
        r.block_rect(3.0, 3.0, 3.4, 3.4)
        r.claim_rect(2, (1,), 0.5, 2.0, 0.7, 2.2, 0.1, 0.2)
        r.block_cells(1, 1, 1, 2, 2, levels=iter([1]))
        r.claim_disc(3, 0, 20, 20, 2, 3)
        r.add_via(4, 30, 30, 0)
        r.add_via(4, 30, 30, 0)
        r.commit(5, [(0, 10 * nx + 30), (0, 10 * nx + 31), (1, 10 * nx + 31), (1, 11 * nx + 31)],
                 False, 0, end_via=True)
        r.snapshot()
        r.commit(5, [(0, 15 * nx + 30), (1, 15 * nx + 30)], False, 0)
        joined = set()
        pad = (1.0, 1.0, (0,), [(0, 10 * nx + 10)], "A.0")
        r.drop_to_plane(1, "A", pad, joined)
        # route_net with a pad that cannot be reached: thin attempts repeat each other
        r.block_cells(0, 0, 25, 49, 26)
        r.block_cells(1, 0, 25, 49, 26)
        pads = [(1.0, 1.0, (0,), [(0, 10 * nx + 10)], "A.0"),
                (1.0, 3.5, (0,), [(0, 35 * nx + 10)], "A.1")]
        r.route_net(1, "A", pads, False, 0)
        r.log["B"] = [(5, None, False, 0)]
        r.rebuild(["B", "B", "nope"])
        rec.routes()
        rec.grids("end")
        rec.close()
        tr = tf.read_trace(path)
        self.assertEqual(tr.errors, [])
        self.assertEqual(tf.check(tr), [])
        tags = [c.tag for c in tr.chunks]
        self.assertEqual(tags, [
            "MARK", "RUN ", "GRDS", "NETS",
            "RIP ", "PAD ", "RECT", "RECT", "RECT", "DISC", "VIA ", "VIA ", "CMIT", "SNAP", "GRDS",
            "CMIT", "SRCH", "CMIT", "DROP", "RECT", "RECT", "RNB ", "SRCH", "SRCH", "SRCH",
            "SRCH", "RNE ", "RBLD", "RIP ", "ROUT", "GRDS", "END "])
        d = [c.data for c in tr.chunks]
        run = d[1]
        self.assertEqual((run.nx, run.ny, run.origin_x, run.origin_y, run.flags), (50, 40, 10.0,
                                                                                  20.0, 4))
        self.assertEqual(run.layer_name, ["F.Cu", "B.Cu"])
        self.assertEqual((d[2].which, d[2].label, d[2].T0[0], d[2].T0[20 * nx + 20]),
                         (0, "attach", tf.BLOCK, 0))           # the edge frame, before any pad
        self.assertEqual(d[3].entries, [(1, "A")])
        self.assertEqual(d[4].removed, [])
        self.assertEqual((d[5].net, d[5].layers, d[5].power), (1, 1, 1.0))
        self.assertAlmostEqual(d[5].d_t, 0.3 + 0.1)
        self.assertEqual([(x.op, x.layers, x.levels) for x in d[6:9]], [(1, 3, 3), (0, 2, 3),
                                                                      (2, 2, 2)])
        self.assertEqual(d[7].net, 2)
        self.assertEqual((d[9].i, d[9].j, d[9].r_t, d[9].r_w), (20, 20, 2, 3))
        self.assertEqual([(v.copper_id, v.added) for v in d[10:12]], [(0, 1), (NONE32, 0)])
        c1 = d[12]
        self.assertEqual((c1.copper_id, c1.flags, c1.vias), (1, 2, [(31, 10), (31, 11)]))
        self.assertEqual((d[14].which, d[14].label), (1, "snapshot"))
        self.assertEqual((d[15].copper_id, d[15].vias), (2, [(30, 15)]))
        self.assertEqual((d[16].ctx, d[16].route_id, d[16].flags & tf.S_VIA_GOAL, d[16].max_cost),
                         (2, NONE32, tf.S_VIA_GOAL, 600))
        self.assertEqual((d[17].ctx, d[18].ok, d[18].pad_label), (2, 1, "A.0"))
        rnb = d[21]
        self.assertEqual((rnb.route_id, rnb.name, rnb.labels, [p[3] for p in rnb.pads]),
                         (0, "A", ["A.0", "A.1"], [1, 1]))
        s = d[22:26]
        self.assertEqual([x.ctx for x in s], [1] * 4)
        self.assertEqual([x.flags & tf.S_RELAX for x in s], [0, 0, tf.S_RELAX, tf.S_RELAX])
        self.assertEqual([x.flags & tf.S_FOUND for x in s], [0, 0, 0, 0])
        self.assertEqual(s[0].repeat_of, NONE32)
        self.assertEqual(s[1].repeat_of, s[0].search_id)     # thin twice: the same trace
        self.assertEqual((s[1].ntrace, s[1].ntrace_total), (0, s[0].ntrace_total))
        self.assertEqual(s[3].repeat_of, s[2].search_id)    # relaxed thin twice, too
        # the relaxed searches found nothing (the band is BLOCK), so no RBLD inside route_net
        self.assertEqual((d[26].status, d[26].failed, d[26].ripped), (0, ["A.1"], []))
        self.assertEqual((d[27].route_id, d[27].nets, d[27].names), (NONE32, [5, -1, -1],
                                                                     ["B", "B", "nope"]))
        self.assertEqual((d[28].net, d[28].ctx, d[28].route_id, d[28].removed), (5, 1, NONE32, [2]))
        end = d[-1]
        self.assertEqual((end.searches, end.routes, end.nfailed), (5, 1, 1))
        alive = tf.replay(tr).alive
        self.assertEqual(sorted(alive), [0, 1, 3])

    def test_recorder_api(self):
        with Recorder(tmp("x.grtrace")) as rec:
            with self.assertRaises(TypeError):
                rec.attach(gridrouter.Router(10, 10, 0.1))
        r1, r2 = TracedRouter(10, 10, 0.1), TracedRouter(12, 10, 0.1)
        path = tmp("two.grtrace")
        with Recorder(path) as rec:
            rec.attach(r1)
            r1.commit(1, [(0, 55), (0, 56)], False, 0)
            rec.attach(r2)                      # a second session; r1 is detached
            self.assertIsNone(r1._rec)
            r1.commit(1, [(0, 57), (0, 58)], False, 0)     # not recorded
            r2.commit(2, [(0, 55), (0, 56)], False, 0)
        tr = tf.read_trace(path)
        self.assertEqual([c.tag for c in tr.chunks], ["RUN ", "GRDS", "CMIT", "RUN ", "GRDS",
                                                     "CMIT", "END "])
        self.assertEqual([d.copper_id for d in chunks_of(tr, "CMIT")], [0, 1])
        self.assertEqual(list(tf.replay(tr).alive), [1])
        self.assertEqual(tf.check(tr), [])
        self.assertTrue(rec.closed)
        rec.close()                             # idempotent
        self.assertIsNone(r2._rec)


# -- 6. reader decisions of 2026-11-23 (spec 4.1, 6, 7.14, 10) -------------------------------------
def build_file(chunks):
    """Bytes of a trace file: header, then (tag, fields dict or payload bytes) chunks, t = 0."""
    b = bytearray(tf.HEADER.pack(tf.MAGIC, 1, 0, 0, 0.0))
    for tag, f in chunks:
        payload = f if isinstance(f, (bytes, bytearray)) else tf.pack(tag, f)
        b += tf.CHUNK.pack(tag.encode(), len(payload), 0.0) + payload + b"\0" * (-len(payload) % 8)
    return bytes(b)


class Decisions(unittest.TestCase):
    """Hand-made files for each point the spec settled on 2026-11-23 (second pass).

    The grid is the sample RUN's 6 x 5 (nc = 30; valid cells 0..59)."""

    def setUp(self):
        f = sample_fields()
        self.run = ("RUN ", f["RUN "])
        self.bad_run = ("RUN ", b"\0" * 40)                     # too short: corrupt

    @staticmethod
    def cmit(cid, net, vias=()):
        return ("CMIT", dict(copper_id=cid, net=net, flags=0, power=0, ctx=0, route_id=NONE32,
                             width_mm=0.2, stubs=[], vias=list(vias), path=[1, 2]))

    @staticmethod
    def via(cid, net, i=1, j=1):
        return ("VIA ", dict(copper_id=cid, net=net, i=i, j=j, power=0, added=1, ctx=0,
                             route_id=NONE32))

    @staticmethod
    def srch(sid, src=(1,), tgt=(2,), flags=0):
        return ("SRCH", dict(search_id=sid, route_id=NONE32, ctx=0, net=1, flags=flags,
                             src=list(src), tgt=list(tgt), path=[], trace_cell=[], trace_d=[]))

    @staticmethod
    def rip(net, removed):
        return ("RIP ", dict(net=net, ctx=0, route_id=NONE32, removed=list(removed)))

    @staticmethod
    def rnb(rid):
        return ("RNB ", dict(route_id=rid, net=1, wide=0, power=0, pads=[], name="A", labels=[]))

    @staticmethod
    def rne(rid):
        return ("RNE ", dict(route_id=rid, net=1, rips_after=0, status=0, ripped=[], failed=[]))

    def snap(self):
        g = sample_fields()["GRDS"]
        return [("SNAP", b"\0" * 4), ("GRDS", dict(g, which=1))]

    def levels(self, chunks):
        tr = tf.read_trace(build_file(chunks))
        errs, warns = tf.check(tr, levels=True)
        return tr, errs, warns

    def test_rip_before_first_snap(self):
        """Decision 1: before the session's first SNAP a RIP may list the net's copper of the
        session (router snapshotted before attaching) or nothing; both are clean."""
        for removed, alive in (([0, 2], [1]), ([], [0, 1, 2])):
            tr, errs, warns = self.levels([self.run, self.cmit(0, 1), self.cmit(1, 2),
                                           self.via(2, 1), self.rip(1, removed)])
            self.assertEqual((errs, warns), ([], []), removed)
            self.assertEqual(sorted(tf.replay(tr).alive), alive)
        # since RUN, not since an earlier session; a partial list is a warning
        tr, errs, warns = self.levels([self.run, self.cmit(0, 1), self.run, self.cmit(1, 1),
                                       self.cmit(2, 1), self.rip(1, [1])])
        self.assertEqual(errs, [])
        self.assertEqual(len(warns), 1)
        self.assertIn("expected [1, 2]", warns[0])
        # after a SNAP an empty list is a warning when the net has copper since then
        tr, errs, warns = self.levels([self.run] + self.snap() + [self.cmit(0, 1), self.rip(1, [])])
        self.assertEqual((errs, len(warns)), ([], 1))

    def test_corrupt_run(self):
        """Decision 2: a corrupt RUN starts a new session with no copper; its router chunks
        are errors until the next valid RUN."""
        g = sample_fields()["GRDS"]
        other_grid = dict(which=0, nc=4, label="", T0=[0] * 4, T1=[0] * 4, W0=[0] * 4,
                          W1=[0] * 4, novia=b"\0" * 4)
        chunks = [self.run, self.cmit(0, 1), self.srch(0), self.bad_run, self.cmit(1, 1),
                  self.srch(1), ("GRDS", other_grid), ("MARK", dict(level=1, text="ok")),
                  self.run, ("GRDS", dict(g, which=0)), self.cmit(2, 1), self.srch(2)]
        tr, errs, warns = self.levels(chunks)
        # the reader reports only the RUN as corrupt: the GRDS is not checked against the
        # previous session's grid
        self.assertEqual(len(tr.errors), 1)
        self.assertIsNone(tr.chunks[3].data)
        self.assertIsNotNone(tr.chunks[6].data)
        self.assertEqual(warns, [])
        self.assertEqual(len(errs), 4, errs)                   # RUN, CMIT, SRCH, GRDS
        self.assertTrue(all("whose RUN is corrupt" in e for e in errs[1:]), errs)
        self.assertFalse(any("expected" in e for e in errs), errs)   # no id mismatch
        self.assertEqual(sorted(tf.replay(tr, upto=2).alive), [0])
        self.assertEqual(tf.replay(tr, upto=5).alive, {})      # previous copper gone
        self.assertIsNone(tf.replay(tr, upto=7).run)
        self.assertEqual(sorted(tf.replay(tr).alive), [2])
        # a corrupt first RUN: chunks after it are errors, not "before any RUN"
        tr, errs, warns = self.levels([self.bad_run, self.cmit(0, 1)])
        self.assertEqual(len(errs), 2)
        self.assertIn("RUN is corrupt", errs[1])

    def test_id_resync(self):
        """Decision 3: one error per gap, then the stored id is followed; a corrupt SRCH,
        RNB or CMIT takes the next id, a corrupt VIA does not."""
        for chunks in ([self.srch(0), self.srch(5), self.srch(6)],
                       [self.cmit(0, 1), self.cmit(2, 1), self.via(3, 1), self.cmit(4, 1)],
                       [self.rnb(0), self.rne(0), self.rnb(3), self.rne(3), self.rnb(4),
                        self.rne(4)]):
            tr, errs, warns = self.levels([self.run] + chunks)
            self.assertEqual(len(errs), 1, errs)
            self.assertIn("expected", errs[0])
            self.assertEqual(warns, [])
        # corrupt SRCH / CMIT / RNB in the sequence: only the corrupt chunk is reported
        for chunks in ([self.srch(0), ("SRCH", b"\0" * 20), self.srch(2)],
                       [self.cmit(0, 1), ("CMIT", b"\0" * 20), self.cmit(2, 1)],
                       [self.rnb(0), self.rne(0), ("RNB ", b"\0" * 8), self.rnb(2), self.rne(2)]):
            tr, errs, warns = self.levels([self.run] + chunks)
            self.assertEqual(len(errs), 1, errs)
            self.assertIn("payload too short", errs[0])
        # a corrupt VIA that had created copper: the corrupt chunk, one mismatch, then in step
        tr, errs, warns = self.levels([self.run, self.cmit(0, 1), ("VIA ", b"\0" * 8),
                                       self.cmit(2, 1), self.cmit(3, 1)])
        self.assertEqual(len(errs), 2, errs)
        self.assertIn("copper_id 2, expected 1", errs[1])
        self.assertEqual(sorted(tf.replay(tr).alive), [0, 2, 3])   # keyed by the stored id

    def test_error_search_cells(self):
        """Decision 4: src / tgt outside the grid are allowed when error is set."""
        bad = (0xFFFFFFFF, 60)
        tr, errs, warns = self.levels([self.run, self.srch(0, src=bad, tgt=bad,
                                                           flags=tf.S_ERROR)])
        self.assertEqual((tr.errors, errs, warns), ([], [], []))
        tr, errs, warns = self.levels([self.run, self.srch(0, src=bad)])
        self.assertEqual(len(errs), 1)
        self.assertIn("src cell out of range", errs[0])

    def test_via_outside_grid(self):
        """Decision 5: a VIA or CMIT via outside the grid is a warning; the VIA keeps its
        copper id."""
        tr, errs, warns = self.levels([self.run, self.via(0, 1, i=-1, j=3), self.cmit(1, 1),
                                       self.cmit(2, 1, vias=[(6, 0)]), self.via(3, 2, i=2, j=5)])
        self.assertEqual(errs, [])
        self.assertEqual(len(warns), 3, warns)
        self.assertTrue(all("outside the grid" in w for w in warns))
        self.assertEqual(sorted(tf.replay(tr).alive), [0, 1, 2, 3])

    def test_rip_other_net_and_bad_ids(self):
        """Decision 6: another net's copper of the session is removed with a warning;
        a missing, earlier-session or already-removed id is an error."""
        tr, errs, warns = self.levels([self.run, self.cmit(0, 1), self.cmit(1, 2),
                                       self.rip(1, [0, 1]), self.rip(2, [])])
        self.assertEqual(errs, [])
        self.assertTrue(any("removes copper id 1 of net 2" in w for w in warns), warns)
        self.assertEqual(tf.replay(tr).alive, {})
        # after a SNAP the net-2 RIP expects nothing: its copper was removed already
        tr, errs, warns = self.levels([self.run] + self.snap() + [
            self.cmit(0, 1), self.cmit(1, 2), self.rip(1, [0, 1]), self.rip(2, [])])
        self.assertEqual(errs, [])
        self.assertEqual(len(warns), 2, warns)                 # other net, list not exact
        cases = [([self.cmit(0, 1), self.rip(1, [0, 7])], "does not exist"),
                 ([self.cmit(0, 1), self.run, self.rip(1, [0])], "earlier router session"),
                 ([self.cmit(0, 1), self.rip(1, [0]), self.rip(1, [0])], "already removed")]
        for chunks, text in cases:
            tr, errs, warns = self.levels([self.run] + chunks)
            self.assertEqual(len(errs), 1, errs)
            self.assertIn(text, errs[0])
        # check() without levels: warnings carry a prefix, errors do not
        tr = tf.read_trace(build_file([self.run, self.cmit(0, 1), self.cmit(1, 2),
                                       self.rip(1, [0, 1]), self.rip(1, [5])]))
        probs = tf.check(tr)
        self.assertEqual([p.startswith("warning: ") for p in probs], [True, True, False, True])

    def test_command_line(self):
        """The check command: clean output unchanged, warnings only exit 0, errors exit 1."""
        import contextlib
        import io
        files = {"clean": [self.run, self.cmit(0, 1)],
                 "warn": [self.run, self.via(0, 1, i=-1)],
                 "error": [self.run, self.cmit(3, 1)]}
        got = {}
        for name, chunks in files.items():
            path = tmp("cli_%s.grtrace" % name)
            with open(path, "wb") as f:
                f.write(build_file(chunks))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = tf._main(["check", path])
            got[name] = (code, buf.getvalue().splitlines())
        self.assertEqual(got["clean"], (0, ["%s: 2 chunks, incomplete, 0 problems"
                                            % tmp("cli_clean.grtrace")]))
        code, lines = got["warn"]
        self.assertEqual(code, 0)
        self.assertTrue(lines[0].startswith("warning: chunk 1 (VIA  at "), lines)
        self.assertTrue(lines[-1].endswith("0 problems, 1 warnings"), lines)
        code, lines = got["error"]
        self.assertEqual(code, 1)
        self.assertTrue(lines[-1].endswith("1 problems"), lines)

    def test_command_line_on_a_file_that_is_not_a_trace(self):
        """check, summary and dump on a non-trace, a missing file or a directory: one line on
        stderr, exit 1, no traceback."""
        import contextlib
        import io
        bad = tmp("cli_not_a_trace.txt")
        with open(bad, "wb") as f:
            f.write(b"this is not a gridrouter trace file, just some text\n")
        for path, text in ((bad, "not a gridrouter trace file"),
                           (tmp("cli_missing.grtrace"), "No such file"),
                           (tmp(""), "directory")):
            for command in ("check", "summary", "dump"):
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    code = tf._main([command, path])
                self.assertEqual(code, 1, (command, path))
                self.assertEqual(out.getvalue(), "")
                lines = err.getvalue().splitlines()
                self.assertEqual(len(lines), 1, lines)
                self.assertIn(text, lines[0])
                self.assertNotIn("Traceback", lines[0])

    def test_run_grid_limit(self):
        """Spec 3.4: nx and ny are 1 to 8192 (the router's limit); a RUN beyond is corrupt
        and its session has no grid."""
        f = sample_fields()["RUN "]
        for nx, ny, ok in ((8192, 8192, True), (8193, 5, False), (6, 8193, False),
                           (0, 5, False), (6, -1, False)):
            tr, errs, warns = self.levels([("RUN ", dict(f, nx=nx, ny=ny)), self.cmit(0, 1)])
            self.assertEqual(tr.chunks[0].data is not None, ok, (nx, ny))
            if ok:
                self.assertEqual(errs, [])
            else:
                self.assertEqual(len(tr.errors), 1)
                self.assertIn("1 to 8192", tr.errors[0])
                self.assertEqual(len(errs), 2, errs)                # the RUN, then its CMIT
                self.assertIn("RUN is corrupt", errs[1])


class Library(unittest.TestCase):
    def test_traced_library(self):
        L = recorder.load_traced()               # must build: a failure here is a test failure
        self.assertIs(L, recorder.load_traced())
        lib = recorder.traced_lib_path()
        self.assertTrue(os.path.exists(lib))
        self.assertIn("_trace_", os.path.basename(lib))
        default = gridrouter._lib_path()
        self.assertEqual(os.path.dirname(lib), os.path.dirname(default))
        self.assertTrue(hasattr(L, "rt_trace"))
        self.assertIsNot(gridrouter._lib, L)
        r = TracedRouter(20, 20, 0.1)
        self.assertIs(r._L, L)
        self.assertIsNot(gridrouter.Router(20, 20, 0.1)._L, L)
        # the default library has no trace symbols
        with self.assertRaises(AttributeError):
            gridrouter._load().rt_trace_count

    def test_lib_naming_follows_environment(self):
        saved = dict(os.environ)
        try:
            os.environ["GRIDROUTER_LIB"] = "/somewhere/else/libgr.dylib"
            os.environ.pop("GRIDROUTER_CFLAGS", None)
            a = recorder.traced_lib_path()
            os.environ["GRIDROUTER_CFLAGS"] = "-DRT_NO_INT128"
            b = recorder.traced_lib_path()
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.assertTrue(a.startswith("/somewhere/else/libgr_trace_"), a)
        self.assertTrue(a.endswith(".dylib"))
        self.assertNotEqual(a, b)


if __name__ == "__main__":
    prog = unittest.main(exit=False, verbosity=2)
    for strict, scenes, n, found, entries in STATS.get("oracle", []):
        print("trace vs oracle %-10s %d scenes, %d searches (%d found a path), %d trace entries, "
              "all identical" % ("strict" if strict else "non-strict", scenes, n, found, entries))
    if "oracle_route" in STATS:
        print("trace vs oracle, route_net / drop_to_plane searches: %d, all identical"
              % STATS["oracle_route"])
    if "same" in STATS:
        print("Router vs TracedRouter: %d boards, %d checkpoints identical" % STATS["same"])
    if "replay" in STATS:
        print("replay: %d boards, %d route_net checkpoints identical to the router" % STATS["replay"])
    if "prefix_small" in STATS:
        print("prefixes: small file %d bytes, %d prefixes read cleanly" % STATS["prefix_small"])
    if "prefix_large" in STATS:
        print("prefixes: large file %d bytes, %d sampled prefixes read cleanly"
              % STATS["prefix_large"])
    raise SystemExit(0 if prog.result.wasSuccessful() else 1)

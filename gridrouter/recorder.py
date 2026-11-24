# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital

"""Record a routing run of gridrouter into a trace file (docs/TRACE_FORMAT.md).

    from gridrouter.recorder import TracedRouter, Recorder

    r = TracedRouter(nx, ny, 0.1)               # same arguments as gridrouter.Router
    with Recorder("run.grtrace") as rec:
        rec.attach(r, nets={1: "GND", 2: "SDA"})
        rec.phase("pads")
        ...                                      # use r exactly like a Router
        rec.routes()
    # close() (or leaving the with block) writes END and detaches the router

TracedRouter uses a second build of the C core, compiled with -DRT_TRACE
(load_traced()); the default library and gridrouter.Router are not touched.
Without a recorder attached a TracedRouter gives exactly the results of a
Router.  check=True asserts the trace invariants of every search (also
without a recorder).

Standard library only, Python 3.9 compatible.
"""
import array
import ctypes
import hashlib
import inspect
import os
import platform
import sys
import time
import types

import gridrouter
from gridrouter import tracefile as tf

__all__ = ["load_traced", "traced_lib_path", "TracedRouter", "Recorder", "check_trace"]

VERSION = "1.0"
NONE32 = tf.NONE32

_Router = gridrouter.Router                 # the class TracedRouter extends (captured at import)
_traced = {}                                # library path -> loaded CDLL


def _i32(v):
    """A Python int as the C core sees it after ctypes' c_int32 conversion."""
    return ((int(v) + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def _rebind(fn, g):
    """fn with its global names looked up in g instead of its module.  Runs the
    module's own code with a different configuration, without changing the
    module or the process (environment, module globals)."""
    nf = types.FunctionType(fn.__code__, g, fn.__name__, fn.__defaults__, fn.__closure__)
    nf.__kwdefaults__ = fn.__kwdefaults__
    return nf


def _traced_flags():
    return (os.environ.get("GRIDROUTER_CFLAGS", "") + " -DRT_TRACE").strip()


def traced_lib_path():
    """Where load_traced() builds the traced library: beside the default library
    (gridrouter/ or the GRIDROUTER_LIB path), named <stem>_trace_<hash><ext>.
    The hash covers everything that changes the build (platform, $CC,
    $GRIDROUTER_ARCHS, $GRIDROUTER_CFLAGS plus -DRT_TRACE), so a change of flags
    gives a new file; gridrouter.build() itself only compares times."""
    base = gridrouter._lib_path()
    stem, ext = os.path.splitext(base)
    key = repr((sys.platform, os.environ.get("CC", ""), os.environ.get("GRIDROUTER_ARCHS", ""),
                _traced_flags()))
    return "%s_trace_%s%s" % (stem, hashlib.sha1(key.encode("utf-8")).hexdigest()[:10], ext)


def load_traced():
    """Build (if needed) and load the C core compiled with -DRT_TRACE.

    gridrouter.build() and gridrouter._load() are reused as they are: their code
    runs with its globals rebound (_lib_path -> traced_lib_path(), os.environ ->
    a copy with -DRT_TRACE appended to GRIDROUTER_CFLAGS, _lib -> a private
    cache), so the build commands, the compiler choice and the ctypes signature
    table are exactly the default library's.  gridrouter's module state, the
    default library and the process environment are not changed."""
    path = traced_lib_path()
    L = _traced.get(path)
    if L is not None:
        return L
    env = dict(os.environ)
    env["GRIDROUTER_CFLAGS"] = _traced_flags()
    fake_os = types.ModuleType("os")
    fake_os.__dict__.update(os.__dict__)
    fake_os.environ = env
    g = dict(vars(gridrouter))
    g.update(os=fake_os, _lib=None, _lib_path=lambda: path)
    g["_build_windows"] = _rebind(gridrouter._build_windows, g)
    g["build"] = _rebind(gridrouter.build, g)
    L = _rebind(gridrouter._load, g)()
    vp = ctypes.c_void_p
    L.rt_trace.restype, L.rt_trace.argtypes = ctypes.c_int, [vp, vp, vp, ctypes.c_int32]
    L.rt_trace_count.restype, L.rt_trace_count.argtypes = ctypes.c_int32, [vp]
    _traced[path] = L
    return L


def check_trace(nc, cells, costs, total, cap, path=None, cost=None, expanded=None, max_cost=None):
    """Assert what the C trace hook guarantees for one search (gridrouter.c, rt_search).

    cells / costs: the stored entries; total: rt_trace_count; cap: buffer size;
    path / cost: the result if a path was found; expanded: last_expanded (None
    after an error); max_cost: the integer bound passed to the C core, or None.

    - at most one entry per (layer, cell), every cell < 2 nc, total <= 2 nc;
    - total <= expanded (an entry is a first expansion, expanded counts all);
    - min(total, cap) entries stored;
    - the first entry is a source, cost 0 (only sources are in the heap at the
      first pop, all at cost 0); no cost is negative;
    - with max_cost, only the search's last pop can have d > max_cost (it ends
      the search), so every stored entry but the overall last is <= max_cost;
    - if a path was found and nothing was cut off: the found state's cell is
      the last entry (an earlier expansion of that cell would have ended the
      search there: the target / via test depends on the cell only) and its
      cost is the path cost; every path cell was expanded, so is in the trace."""
    n = len(cells)
    assert 0 <= total <= 2 * nc, "trace count %d outside 0..%d" % (total, 2 * nc)
    assert n == min(total, cap), "%d entries stored, count %d, cap %d" % (n, total, cap)
    assert len(costs) == n, "cells and costs differ in length"
    if expanded is not None:
        assert total <= expanded, "trace count %d > expanded %d" % (total, expanded)
    if not n:
        return
    assert max(cells) < 2 * nc, "trace cell out of range"
    assert len(set(cells)) == n, "a (layer, cell) is in the trace twice"
    assert min(costs) >= 0, "negative cost in the trace"
    assert costs[0] == 0, "first trace entry has cost %d, not 0" % costs[0]
    if max_cost is not None:
        k = n if total > n else n - 1
        if k:
            assert max(costs[:k]) <= max_cost, "an expansion before the last has d > max_cost"
    if path is not None and total == n:
        enc = [layer * nc + idx for layer, idx in path]
        assert cells[-1] == enc[-1], "the found cell is not the last trace entry"
        assert costs[-1] == cost, "last trace cost %d != path cost %d" % (costs[-1], cost)
        cs = set(cells)
        assert all(c in cs for c in enc), "a path cell is not in the trace"


class TracedRouter(_Router):
    """gridrouter.Router on the -DRT_TRACE library, reporting to an attached Recorder.

    Extra keyword arguments: check=True asserts check_trace() on every search;
    trace_cap sets the trace buffer size (default 2 * nx * ny, the most a search
    can record)."""

    def __init__(self, *args, check=False, trace_cap=None, **kw):
        init = _rebind(_Router.__init__, dict(vars(gridrouter), _load=load_traced))
        init(self, *args, **kw)
        ba = inspect.signature(_Router.__init__).bind(self, *args, **kw)
        ba.apply_defaults()
        em = ba.arguments.get("edge_margin")
        self._edge_margin = None if em is None else tuple(em)
        self._check_trace = bool(check)
        self._tr_cap = 2 * self.NC if trace_cap is None else int(trace_cap)
        self._tr_cells = self._tr_cost = None
        self._tr_on = False
        self._rec = None
        self._depth = 0                     # overridden methods active (for "outermost")
        self._ctxs = []                     # 1 inside route_net, 2 inside drop_to_plane
        self._route = None                  # route_id of the active route_net
        self._commit_vias = []              # one list per active commit: vias it created
        self._in_rebuild = 0
        self.last_trace = None              # (cells, costs, count) of the last traced search
        self.trace_checks = 0               # searches check_trace() passed
        self._sync_trace()

    # -- trace buffers ------------------------------------------------------------
    def _sync_trace(self):
        want = self._rec is not None or self._check_trace
        if want and not self._tr_on:
            if self._tr_cells is None:
                n = max(1, self._tr_cap)
                self._tr_cells = (ctypes.c_uint32 * n)()
                self._tr_cost = (ctypes.c_int32 * n)()
            gridrouter._check(self._L.rt_trace(self._r, ctypes.addressof(self._tr_cells),
                                               ctypes.addressof(self._tr_cost), self._tr_cap))
            self._tr_on = True
        elif not want and self._tr_on:
            gridrouter._check(self._L.rt_trace(self._r, None, None, 0))
            self._tr_on = False

    def _grab_trace(self):
        total = self._L.rt_trace_count(self._r)
        n = max(0, min(total, self._tr_cap))
        cells, costs = array.array("I"), array.array("i")
        if n:
            cells.frombytes(ctypes.string_at(ctypes.addressof(self._tr_cells), 4 * n))
            costs.frombytes(ctypes.string_at(ctypes.addressof(self._tr_cost), 4 * n))
        return cells, costs, total

    def _ctx(self):
        return (self._ctxs[-1] if self._ctxs else 0,
                NONE32 if self._route is None else self._route)

    # -- search -------------------------------------------------------------------
    def search(self, net, sources, targets, wide, goal=None, max_cost=None, via_goal=False,
               relax=False):
        if self._rec is None and not self._check_trace:
            return _Router.search(self, net, sources, targets, wide, goal, max_cost, via_goal,
                                  relax)
        NC = self.NC
        sa = ta = None
        rips0 = self.rips
        t0 = time.perf_counter()
        self._depth += 1
        try:
            # the same conversion Router.search makes; it accepts the arrays as they are
            sa = sources if isinstance(sources, array.array) else gridrouter._cells(sources, NC)[0]
            ta = targets if isinstance(targets, array.array) else gridrouter._cells(targets, NC)[0]
            res = _Router.search(self, net, sa, ta, wide, goal, max_cost, via_goal, relax)
        except BaseException as e:
            dt = time.perf_counter() - t0
            self._depth -= 1
            self._searched(net, sa, ta, wide, goal, max_cost, via_goal, relax, None, e, dt, rips0)
            raise
        dt = time.perf_counter() - t0
        self._depth -= 1
        self._searched(net, sa, ta, wide, goal, max_cost, via_goal, relax, res, None, dt, rips0)
        return res

    def _searched(self, net, sa, ta, wide, goal, max_cost, via_goal, relax, res, exc, dt, rips0):
        # A RouterError comes from rt_search itself (Router.search raises nothing
        # else after the C call), so the trace count belongs to this search.  Any
        # other exception was raised before the C call: no trace.
        traced = self._tr_on and (exc is None or isinstance(exc, gridrouter.RouterError))
        tr = self._grab_trace() if traced else None
        self.last_trace = tr
        found = exc is None and res[0] is not None
        mc = None
        if max_cost is not None:
            try:
                mc = int(max_cost // 1)
            except Exception:
                mc = None
        if traced and self._check_trace:
            check_trace(self.NC, tr[0], tr[1], tr[2], self._tr_cap,
                        path=res[0] if found else None, cost=self.last_cost if found else None,
                        expanded=self.last_expanded if exc is None else None, max_cost=mc)
            self.trace_checks += 1
        if self._rec is not None:
            self._rec._on_search(self, net, sa, ta, wide, goal, mc, via_goal, relax, res, exc, dt,
                                 rips0, tr)

    # -- copper -------------------------------------------------------------------
    def commit(self, net, path, wide, power, end_via=False):
        rec = self._rec
        if rec is None:
            return _Router.commit(self, net, path, wide, power, end_via)
        n0 = len(self.stubs.get(net, ()))
        vias = []
        self._commit_vias.append(vias)
        self._depth += 1
        try:
            out = _Router.commit(self, net, path, wide, power, end_via)
        finally:
            self._depth -= 1
            self._commit_vias.pop()
        rec._on_commit(self, net, path, wide, power, end_via, list(self.stubs.get(net, ())[n0:]),
                       vias)
        return out

    def add_via(self, net, i, j, power):
        rec = self._rec
        if rec is None:
            return _Router.add_via(self, net, i, j, power)
        added = (i, j) not in self.vias.get(net, ())
        self._depth += 1
        try:
            out = _Router.add_via(self, net, i, j, power)
        finally:
            self._depth -= 1
        if self._commit_vias:
            if added:
                self._commit_vias[-1].append((i, j))
        else:
            rec._on_via(self, net, i, j, power, added)
        return out

    def rip(self, net):
        rec = self._rec
        if rec is None:
            return _Router.rip(self, net)
        self._depth += 1
        try:
            out = _Router.rip(self, net)
        finally:
            self._depth -= 1
        rec._on_rip(self, net, 1 if self._in_rebuild else 0)
        return out

    def rebuild(self, drop):
        rec = self._rec
        if rec is None:
            return _Router.rebuild(self, drop)
        names = drop if isinstance(drop, (list, tuple)) else list(drop)
        rec._on_rebuild(self, names)
        self._in_rebuild += 1
        self._depth += 1
        try:
            return _Router.rebuild(self, names)
        finally:
            self._depth -= 1
            self._in_rebuild -= 1

    def snapshot(self):
        rec = self._rec
        if rec is None:
            return _Router.snapshot(self)
        self._depth += 1
        try:
            out = _Router.snapshot(self)
        finally:
            self._depth -= 1
        rec._on_snapshot(self)
        return out

    # -- nets ---------------------------------------------------------------------
    def route_net(self, net, name, pads, wide, power):
        rec = self._rec
        if rec is None:
            return _Router.route_net(self, net, name, pads, wide, power)
        rid = rec._on_route_begin(self, net, name, pads, wide, power)
        prev = self._route
        self._route = rid
        self._ctxs.append(1)
        self._depth += 1
        try:
            ripped = _Router.route_net(self, net, name, pads, wide, power)
        except BaseException:
            self._depth -= 1
            self._ctxs.pop()
            self._route = prev
            rec._on_route_end(self, rid, net, name, [], 1)
            raise
        self._depth -= 1
        self._ctxs.pop()
        self._route = prev
        rec._on_route_end(self, rid, net, name, ripped, 0)
        return ripped

    def drop_to_plane(self, net, name, pad, joined, max_cost=600):
        rec = self._rec
        if rec is None:
            return _Router.drop_to_plane(self, net, name, pad, joined, max_cost)
        self._ctxs.append(2)
        self._depth += 1
        try:
            ok = _Router.drop_to_plane(self, net, name, pad, joined, max_cost)
        except BaseException:
            self._depth -= 1
            self._ctxs.pop()
            rec._on_drop(self, net, name, pad, max_cost, 2)
            raise
        self._depth -= 1
        self._ctxs.pop()
        rec._on_drop(self, net, name, pad, max_cost, 1 if ok else 0)
        return ok

    # -- occupancy ------------------------------------------------------------------
    def add_pad(self, net, layers, cx, cy, x0, y0, x1, y1, power):
        rec = self._rec
        if rec is None:
            return _Router.add_pad(self, net, layers, cx, cy, x0, y0, x1, y1, power)
        outer = self._depth == 0
        self._depth += 1
        try:
            out = _Router.add_pad(self, net, layers, cx, cy, x0, y0, x1, y1, power)
        finally:
            self._depth -= 1
        if outer:
            G = self.G
            rec._emit("PAD ", dict(
                net=_i32(net), layers=tf._layer_mask(layers), power=float(power), cx=float(cx),
                cy=float(cy), x0=float(x0), y0=float(y0), x1=float(x1), y1=float(y1),
                d_t=float(self.pad_clear[0] + G * power), d_w=float(self.pad_clear[1] + G * power),
                d_novia=float(self.pad_novia)))
        return out

    def claim_rect(self, net, layers, x0, y0, x1, y1, d_t, d_w):
        rec = self._rec
        if rec is None:
            return _Router.claim_rect(self, net, layers, x0, y0, x1, y1, d_t, d_w)
        outer = self._depth == 0
        self._depth += 1
        try:
            out = _Router.claim_rect(self, net, layers, x0, y0, x1, y1, d_t, d_w)
        finally:
            self._depth -= 1
        if outer:
            rec._emit("RECT", dict(op=0, net=_i32(net), layers=tf._layer_mask(layers), levels=3,
                                   x0=float(x0), y0=float(y0), x1=float(x1), y1=float(y1),
                                   d_t=float(d_t), d_w=float(d_w)))
        return out

    def block_rect(self, x0, y0, x1, y1):
        rec = self._rec
        if rec is None:
            return _Router.block_rect(self, x0, y0, x1, y1)
        outer = self._depth == 0
        self._depth += 1
        try:
            out = _Router.block_rect(self, x0, y0, x1, y1)
        finally:
            self._depth -= 1
        if outer:
            rec._emit("RECT", dict(op=1, net=gridrouter.BLOCK, layers=3, levels=3, x0=float(x0),
                                   y0=float(y0), x1=float(x1), y1=float(y1), d_t=0.0, d_w=0.0))
        return out

    def block_cells(self, layer, i0, j0, i1, j1, levels=(0, 1)):
        rec = self._rec
        if rec is None:
            return _Router.block_cells(self, layer, i0, j0, i1, j1, levels)
        levels = tuple(levels)              # iterated once by Router, and again here
        outer = self._depth == 0
        self._depth += 1
        try:
            out = _Router.block_cells(self, layer, i0, j0, i1, j1, levels)
        finally:
            self._depth -= 1
        if outer:
            rec._emit("RECT", dict(op=2, net=gridrouter.BLOCK, layers=tf._layer_mask((layer,)),
                                   levels=tf._layer_mask(levels), x0=float(i0), y0=float(j0),
                                   x1=float(i1), y1=float(j1), d_t=0.0, d_w=0.0))
        return out

    def claim_disc(self, net, layer, i, j, r_t, r_w):
        rec = self._rec
        if rec is None:
            return _Router.claim_disc(self, net, layer, i, j, r_t, r_w)
        outer = self._depth == 0
        self._depth += 1
        try:
            out = _Router.claim_disc(self, net, layer, i, j, r_t, r_w)
        finally:
            self._depth -= 1
        if outer:
            rec._emit("DISC", dict(net=_i32(net), layer=_i32(layer), i=_i32(i), j=_i32(j),
                                   r_t=_i32(r_t), r_w=_i32(r_w)))
        return out


def _enc_path(path, nc):
    return array.array("I", [layer * nc + idx for layer, idx in path])


def _count(x):
    """Number of 4-byte cells in x (bytes, or an array of 4-byte items)."""
    return len(x) // 4 if isinstance(x, (bytes, bytearray)) else len(x)


class Recorder:
    """Writes a trace file of one TracedRouter at a time (spec section 5).

    Usable as a context manager; close() writes END.  Every chunk is flushed
    when written, so a viewer can follow the file while it grows."""

    def __init__(self, path, producer=None):
        self.path = path
        self._w = tf.TraceWriter(path)
        self.producer = producer or "gridrouter.recorder %s; Python %s" % (
            VERSION, platform.python_version())
        self.router = None
        self.closed = False
        self._next_search = self._next_route = self._next_copper = 0
        self._n_routes = 0
        self._expanded_total = self._trace_entries = 0
        self._since_snap = {}               # net -> copper ids created since SNAP (this session)
        self._traces = {}                   # (count, n, digest) -> (search_id, offset, nbytes)
        self._rd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    # -- user calls -----------------------------------------------------------------
    def attach(self, router, origin=(0.0, 0.0), via_dia=0.60, via_drill=0.30,
               layer_names=("F.Cu", "B.Cu"), nets=None):
        """Start a router session: RUN (settings now), GRDS "attach" (the grids now)
        and NETS if nets is given ({router net: name} or [(net, name)])."""
        if not isinstance(router, TracedRouter):
            raise TypeError("Recorder.attach needs a gridrouter.recorder.TracedRouter")
        self._live()
        if self.router is not None and self.router is not router:
            self._detach()
        if router._rec is not None and router._rec is not self:
            router._rec._detach()
        r = router
        em = r._edge_margin
        flags = (tf.R_STRICT if r.strict else 0) | (tf.R_SKIP_JOINED if r.skip_joined_stubs else 0) \
            | (tf.R_EDGE_MARGIN if em is not None else 0)
        self._emit("RUN ", dict(
            nx=r.NX, ny=r.NY, dirs=[tuple(d) for d in r.DIRS], block=gridrouter.BLOCK,
            via_cost=_i32(r.VIA_COST), rip_cost=_i32(r.RIP_COST), max_rips=_i32(r.MAX_RIPS),
            h_max=_i32(r.heuristic[0]), h_min=_i32(r.heuristic[1]), h_off=_i32(r.heuristic[2]),
            r_track=r.r_track, r_wide=r.r_wide, r_via=r.r_via, r_via_space=r.r_via_space,
            flags=flags, cell=float(r.G), origin_x=float(origin[0]), origin_y=float(origin[1]),
            track=float(r.TRACK), track_wide=float(r.TRACK_PWR), pad_clear=r.pad_clear,
            pad_novia=float(r.pad_novia), edge_margin=em if em is not None else (0.0, 0.0),
            via_dia=float(via_dia), via_drill=float(via_drill), step=r.STEP, turn=r.TURN,
            producer=self.producer, layer_name=list(layer_names)))
        self._since_snap = {}
        self._traces = {}
        self.router = router
        router._rec = self
        router._sync_trace()
        self.grids("attach")                # spec 6.4: right after RUN
        if nets:
            self.nets(nets)
        return self

    def nets(self, mapping):
        """NETS: {router net: name} or [(net, name)]."""
        items = mapping.items() if isinstance(mapping, dict) else mapping
        self._emit("NETS", dict(entries=[(_i32(k), str(v)) for k, v in items]))

    def phase(self, text):
        """MARK level 0: a named phase starts here."""
        self._emit("MARK", dict(level=0, text=str(text)))

    def mark(self, text, level=1):
        """MARK level 1 (a note), or level 0 (a phase)."""
        self._emit("MARK", dict(level=int(level), text=str(text)))

    def grids(self, label=""):
        """GRDS which = 0: the attached router's live grids."""
        r = self._need_router()
        self._emit("GRDS", dict(which=0, nc=r.NC, label=str(label), T0=r.T[0], T1=r.T[1],
                                W0=r.W[0], W1=r.W[1], novia=r.novia))

    def routes(self):
        """ROUT: the attached router's routes() now."""
        r = self._need_router()
        segs, vias = r.routes()
        self._emit("ROUT", dict(
            segs=[(layer, _i32(net), x0, y0, x1, y1, w) for layer, x0, y0, x1, y1, w, net in segs],
            vias=[(x, y, _i32(net)) for x, y, net in vias]))

    def board(self, board):
        """BRD: a tracefile.BoardWriter (or a ready payload, bytes)."""
        payload = board if isinstance(board, (bytes, bytearray)) else board.payload()
        self._emit("BRD ", bytes(payload))

    def close(self):
        """END, then detach the router and close the file.  Idempotent."""
        if self.closed:
            return
        r = self.router
        failed = [(str(n), str(lb)) for n, lb in r.failed] if r is not None else []
        self._emit("END ", dict(searches=self._next_search, routes=self._n_routes,
                                failed=failed, rips=_i32(r.rips) if r is not None else 0,
                                expanded_total=self._expanded_total,
                                trace_entries=self._trace_entries, wall_s=self._w.now()))
        if r is not None:
            self._detach()
        self._w.close()
        if self._rd is not None:
            self._rd.close()
            self._rd = None
        self.closed = True

    # -- internals -----------------------------------------------------------------
    def _live(self):
        if self.closed:
            raise ValueError("recorder is closed")

    def _need_router(self):
        if self.router is None:
            raise ValueError("no router attached")
        return self.router

    def _detach(self):
        r = self.router
        self.router = None
        if r is not None and r._rec is self:
            r._rec = None
            r._sync_trace()

    def _emit(self, tag, fields):
        self._live()
        payload = fields if isinstance(fields, bytes) else tf.pack(tag, fields)
        return self._w.chunk(tag, payload)

    def _new_copper(self, net):
        cid = self._next_copper
        self._next_copper += 1
        self._since_snap.setdefault(net, []).append(cid)
        return cid

    def _on_search(self, r, net, sa, ta, wide, goal, mc, via_goal, relax, res, exc, dt, rips0, tr):
        ctx, rid = r._ctx()
        flags = 0
        if wide:
            flags |= tf.S_WIDE
        gi = gj = 0
        if goal:
            try:
                gi, gj = _i32(goal[0]), _i32(goal[1])
                flags |= tf.S_HAS_GOAL
            except Exception:
                gi = gj = 0
        if mc is not None:
            flags |= tf.S_HAS_MAX
        if via_goal:
            flags |= tf.S_VIA_GOAL
        if relax:
            flags |= tf.S_RELAX
        path, cost = b"", -1
        if exc is None and res[0] is not None:
            flags |= tf.S_FOUND
            if res[1]:
                flags |= tf.S_NEW_VIA
            path = _enc_path(res[0], r.NC)
            cost = r.last_cost
        expanded = r.last_expanded if exc is None else -1
        if exc is not None:
            flags |= tf.S_ERROR
        sid = self._next_search
        self._next_search += 1
        cells = costs = b""
        total = 0
        repeat_of = NONE32
        if tr is not None:
            flags |= tf.S_HAS_TRACE | tf.S_HAS_COSTS
            cells, costs = tf.le_array("I", tr[0]), tf.le_array("i", tr[1])
            total = tr[2]
            if cells:
                key = (total, len(cells), hashlib.blake2b(cells + costs, digest_size=16).digest())
                hit = self._traces.get(key)
                if hit is not None and self._same_bytes(hit[1], cells + costs):
                    repeat_of = hit[0]
                    cells = costs = b""
        src = sa if sa is not None else b""
        tgt = ta if ta is not None else b""
        fields = dict(search_id=sid, route_id=rid, ctx=ctx, net=_i32(net), flags=flags, gi=gi,
                      gj=gj, rips_at_call=_i32(rips0), max_cost=-1 if mc is None else mc,
                      cost=cost, expanded=expanded, duration_s=dt, src=src, tgt=tgt, path=path,
                      trace_cell=cells, trace_d=costs, ntrace_total=total, repeat_of=repeat_of)
        off = self._emit("SRCH", fields)
        if expanded >= 0:
            self._expanded_total += expanded
        ntrace = len(cells) // 4
        self._trace_entries += ntrace
        if ntrace and repeat_of == NONE32:
            # file offset of trace_cell: chunk header, fixed part, src, tgt, path
            at = off + tf.CHUNK.size + 96 + 4 * (_count(src) + _count(tgt) + _count(path))
            self._traces.setdefault(key, (sid, at, len(cells) + len(costs)))

    def _same_bytes(self, offset, data):
        """True if the file holds data at offset (the trace arrays of an earlier SRCH)."""
        if self._rd is None:
            self._rd = open(self.path, "rb")
        self._rd.seek(offset)
        return self._rd.read(len(data)) == data

    def _on_commit(self, r, net, path, wide, power, end_via, stubs, vias):
        ctx, rid = r._ctx()
        w = r.TRACK_PWR if wide else r.TRACK
        flags = (tf.C_WIDE if wide else 0) | (tf.C_END_VIA if end_via else 0) | \
            (tf.C_SKIP_JOINED if r.skip_joined_stubs else 0)
        self._emit("CMIT", dict(
            copper_id=self._new_copper(net), net=_i32(net), flags=flags, power=_i32(int(power)),
            ctx=ctx, route_id=rid, width_mm=float(w),
            stubs=[(layer, x0, y0, x1, y1, float(w)) for layer, x0, y0, x1, y1 in stubs],
            vias=[(_i32(i), _i32(j)) for i, j in vias], path=_enc_path(path, r.NC)))

    def _on_via(self, r, net, i, j, power, added):
        ctx, rid = r._ctx()
        self._emit("VIA ", dict(copper_id=self._new_copper(net) if added else NONE32, net=_i32(net),
                                i=_i32(i), j=_i32(j), power=_i32(int(power)), added=1 if added else 0,
                                ctx=ctx, route_id=rid))

    def _on_rip(self, r, net, ctx):
        # Router.rip resets the net to its snapshot() state; without a snapshot it
        # changes nothing (the C core has no ownership records, the Python side
        # returns early).
        removed = sorted(self._since_snap.pop(net, ())) if r._snap is not None else []
        self._emit("RIP ", dict(net=_i32(net), ctx=ctx,
                                route_id=NONE32 if r._route is None else r._route, removed=removed))

    def _on_rebuild(self, r, names):
        nets, gone = [], set()
        for name in names:                  # as Router.rebuild: log.pop(name), rip if non-empty
            try:
                items = None if name in gone else r.log.get(name)
                gone.add(name)
            except TypeError:               # unhashable name: Router.rebuild raises too
                items = None
            nets.append(_i32(items[0][0]) if items else -1)
        self._emit("RBLD", dict(route_id=NONE32 if r._route is None else r._route, nets=nets,
                                names=[str(n) for n in names]))

    def _on_snapshot(self, r):
        self._emit("SNAP", b"\0\0\0\0")
        self._emit("GRDS", dict(which=1, nc=r.NC, label="snapshot", T0=r.hardT[0], T1=r.hardT[1],
                                W0=r.hardW[0], W1=r.hardW[1], novia=r.hard_novia))
        self._since_snap = {}

    def _on_route_begin(self, r, net, name, pads, wide, power):
        rid = self._next_route
        self._next_route += 1
        self._n_routes += 1
        recs, labels = [], []
        for p in pads:
            try:
                cx, cy = float(p[0]), float(p[1])
            except Exception:
                cx = cy = 0.0
            try:
                layers = tf._layer_mask(p[2])
            except Exception:
                layers = 0
            try:
                ncells = len(p[3])
            except Exception:
                ncells = NONE32
            try:
                label = str(p[4])
            except Exception:
                label = ""
            recs.append((cx, cy, layers, ncells))
            labels.append(label)
        try:
            ipower = _i32(int(power))
        except Exception:
            ipower = 0
        self._emit("RNB ", dict(route_id=rid, net=_i32(net), wide=1 if wide else 0, power=ipower,
                                pads=recs, name=str(name), labels=labels))
        return rid

    def _on_route_end(self, r, rid, net, name, ripped, status):
        failed = [str(f[1]) for f in r.failed if f[0] == name]
        self._emit("RNE ", dict(route_id=rid, net=_i32(net), rips_after=_i32(r.rips),
                                ripped=[str(n) for n in ripped], failed=failed, status=status))

    def _on_drop(self, r, net, name, pad, max_cost, ok):
        mc = -1
        if max_cost is not None:
            try:
                mc = int(max_cost // 1)
            except Exception:
                mc = -1
        try:
            label = str(pad[4])
        except Exception:
            label = ""
        self._emit("DROP", dict(net=_i32(net), ok=ok, max_cost=mc, name=str(name),
                                pad_label=label))

"""gridrouter - fast two-layer PCB grid (maze) router.

A C core (gridrouter.c, loaded with ctypes) owns the occupancy grids and runs
the A* search, the claims and the rip-up; this module keeps the light
bookkeeping (edges, pad stubs, vias, the routing log) and the per-net driver
logic (route_net, drop_to_plane) in Python.  Standard library only.

The behaviour follows the pure-Python Router of obd2_dongle/scripts/gen_pcb.py; the
differences are listed in README.md.  The shared library is compiled on
first use (or when gridrouter.c / gridrouter.h is newer than it) with the
system C compiler.
"""
import array
import copy
import ctypes
import os
import subprocess
import sys

__all__ = ["Router", "BLOCK", "build", "make_dirs", "make_step", "make_turn", "disc",
           "RouterError"]

BLOCK = 1 << 30

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "gridrouter.c")
_HDR = os.path.join(_HERE, "gridrouter.h")
if sys.platform == "win32":
    _LIBNAME = "gridrouter.dll"
elif sys.platform == "darwin":
    _LIBNAME = "libgridrouter.dylib"
else:
    _LIBNAME = "libgridrouter.so"
_ERRORS = {-1: "bad argument", -2: "out of memory", -3: "path cost overflowed 31 bits",
           -4: "not allowed before snapshot()"}


class RouterError(RuntimeError):
    pass


def _lib_path():
    return os.environ.get("GRIDROUTER_LIB") or os.path.join(_HERE, _LIBNAME)


def build(force=False, quiet=True):
    """Compile the shared library if it is missing or older than the C source.

    Returns its path.  On macOS a universal (arm64 + x86_64) dylib is built so
    that it loads in any Python; override with GRIDROUTER_ARCHS="arm64".
    The compiler is $CC or cc; on Windows $CC, else the first of cl (MSVC, from a
    Developer Command Prompt), gcc and clang found on PATH.
    """
    lib = _lib_path()
    src_time = max(os.path.getmtime(_SRC), os.path.getmtime(_HDR))
    if not force and os.path.exists(lib) and os.path.getmtime(lib) >= src_time:
        return lib
    if sys.platform == "win32":
        return _build_windows(lib)
    cc = os.environ.get("CC", "cc")
    if sys.platform == "darwin":
        archs = os.environ.get("GRIDROUTER_ARCHS", "arm64 x86_64").split()
        flags = ["-dynamiclib"]
        for a in archs:
            flags += ["-arch", a]
    else:
        flags = ["-shared", "-fPIC"]
    tmp = "%s.%d.tmp" % (lib, os.getpid())
    extra = os.environ.get("GRIDROUTER_CFLAGS", "").split()
    cmd = [cc, "-O2", "-std=c99", "-Wall", "-Wextra", *flags, *extra, "-o", tmp, _SRC]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, encoding="utf-8", errors="replace")
    if res.returncode != 0:
        raise RouterError("building %s failed:\n%s\n%s" % (lib, " ".join(cmd), res.stdout))
    if res.stdout and not quiet:
        print(res.stdout, file=sys.stderr)
    os.replace(tmp, lib)                # atomic: a running process keeps its old copy
    return lib


def _build_windows(lib):
    """gridrouter.dll with MSVC (cl) or a gcc / clang (MinGW, MSYS2, LLVM) on PATH.
    The compiler must target the same architecture as this Python (64-bit for 64-bit)."""
    import shutil
    import tempfile
    names = [os.environ["CC"]] if os.environ.get("CC") else ["cl", "gcc", "clang"]
    cc = next((shutil.which(n) for n in names if shutil.which(n)), None)
    if cc is None:
        raise RouterError(
            "no C compiler found to build %s (tried %s).  Install Visual Studio Build Tools and run "
            "from an 'x64 Native Tools Command Prompt', or put MinGW-w64 gcc or LLVM clang on PATH, "
            "or set CC.  Or build gridrouter.dll elsewhere and set GRIDROUTER_LIB to its path."
            % (lib, ", ".join(names)))
    extra = os.environ.get("GRIDROUTER_CFLAGS", "").split()
    tmpdir = tempfile.mkdtemp()
    out = os.path.join(tmpdir, "gridrouter.dll")
    if os.path.splitext(os.path.basename(cc))[0].lower() in ("cl", "clang-cl"):
        cmd = [cc, "/nologo", "/O2", "/LD", "/W3", *extra, _SRC, "/Fe" + out, "/Fo" + tmpdir + os.sep]
    else:
        cmd = [cc, "-O2", "-std=c99", "-Wall", "-Wextra", "-shared", *extra, "-o", out, _SRC]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True, encoding="utf-8", errors="replace", cwd=tmpdir)
        if res.returncode != 0 or not os.path.exists(out):
            raise RouterError("building %s failed:\n%s\n%s" % (lib, " ".join(cmd), res.stdout))
        shutil.copyfile(out, lib)       # a DLL in use cannot be replaced: close other Pythons first
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return lib


_lib = None


def _load():
    global _lib
    if _lib is not None:
        return _lib
    L = ctypes.CDLL(build())
    i32, i64, p32 = ctypes.c_int32, ctypes.c_int64, ctypes.POINTER(ctypes.c_int32)
    vp = ctypes.c_void_p
    sigs = {
        "rt_new": (vp, [i32, i32, i32, p32, p32, p32, p32, i32, i32, i32, i32, i32]),
        "rt_free": (None, [vp]),
        "rt_grid": (vp, [vp, i32, i32]),
        "rt_hard": (vp, [vp, i32, i32]),
        "rt_novia": (vp, [vp]),
        "rt_hard_novia": (vp, [vp]),
        "rt_claim_rect": (ctypes.c_int, [vp, i32, i32, i32, i32, i32, i32, i32]),
        "rt_novia_rect": (ctypes.c_int, [vp, i32, i32, i32, i32]),
        "rt_claim_disc": (ctypes.c_int, [vp, i32, i32, i32, i32, i32, i32]),
        "rt_claim_path": (ctypes.c_int, [vp, i32, vp, i32, i32, i32]),
        "rt_add_via": (ctypes.c_int, [vp, i32, i32, i32, i32, i32, i32]),
        "rt_snapshot": (ctypes.c_int, [vp]),
        "rt_snapped": (ctypes.c_int, [vp]),
        "rt_rip": (ctypes.c_int, [vp, i32]),
        "rt_owned_count": (ctypes.c_int, [vp, i32]),
        "rt_via_ok": (ctypes.c_int, [vp, i32, i32]),
        "rt_owners": (ctypes.c_int, [vp, i32, vp, i32, vp, i32]),
        "rt_path_free": (ctypes.c_int, [vp, i32, vp, i32, i32]),
        "rt_search": (ctypes.c_int, [vp, i32, vp, i32, vp, i32, i32, i32, i32, i32, i32, i64,
                                     i32, i32, ctypes.POINTER(i64), ctypes.POINTER(i32),
                                     ctypes.POINTER(i64)]),
        "rt_get_path": (ctypes.c_int, [vp, vp, i32]),
        "rt_stats": (None, [vp, vp]),
    }
    for name, (res, args) in sigs.items():
        f = getattr(L, name)
        f.restype = res
        f.argtypes = args
    _lib = L
    return L


def _check(rc):
    if rc < 0:
        raise RouterError(_ERRORS.get(rc, "error %d" % rc))
    return rc


def _cells(items, nc):
    """[(layer, idx)] -> (int32 array, pointer) in the C encoding layer * nc + idx."""
    a = array.array("i", [l * nc + i for l, i in items])
    if not a:
        return a, None
    return a, a.buffer_info()[0]


# -- cost tables (same formulas as gen_pcb.py) --------------------------------
def make_dirs(strict):
    """Direction list [(dx, dy, base cost)]: 4 directions if strict, else 8."""
    if strict:
        return [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10)]
    return [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10),
            (1, 1, 14), (1, -1, 14), (-1, 1, 14), (-1, -1, 14)]


def make_step(dirs, wrong_way, diagonal):
    """STEP[layer][dir]: layer 0 prefers x, layer 1 prefers y."""
    def one(along_x):
        out = []
        for dx, dy, c in dirs:
            f = diagonal if dx and dy else 1.0 if (dx if along_x else dy) else wrong_way
            out.append(int(round(c * f)))
        return out
    return [one(True), one(False)]


def make_turn(dirs, bend_45, bend_90):
    """TURN[incoming][outgoing], None = forbidden; row len(dirs) is a path start / after a via."""
    rows = []
    for ax, ay, _ in dirs:
        row = []
        for bx, by, _ in dirs:
            dot = (ax * bx + ay * by) / ((ax * ax + ay * ay) ** 0.5 * (bx * bx + by * by) ** 0.5)
            row.append(0 if dot > 0.9 else bend_45 if dot > 0.5 else bend_90 if dot > -0.1 else None)
        rows.append(row)
    rows.append([0] * len(dirs))
    return rows


_disc = {}


def disc(r):
    """Cell offsets of a disc of radius r cells (same rule as the C core)."""
    if r not in _disc:
        _disc[r] = [(dx, dy) for dx in range(-r, r + 1) for dy in range(-r, r + 1)
                    if dx * dx + dy * dy < (r + 0.5) ** 2]
    return _disc[r]


class Router:
    """Two-layer grid router.  See README.md for the full API."""

    BLOCK = BLOCK

    def __init__(self, nx, ny, cell=0.1, strict=False, dirs=None, wrong_way=None,
                 diagonal=None, step=None, turn=None, bend_45=12, bend_90=40, via_cost=250,
                 rip_cost=150, max_rips=400, heuristic=None, track=0.20, track_wide=0.50,
                 r_track=(3, 5), r_wide=(5, 7), r_via=(5, 7), r_via_space=8,
                 pad_clear=(0.30, 0.50), pad_novia=0.60 / 2 + 0.1,
                 edge_margin=(0.3 + 0.15, 0.3 + 0.35)):
        L = self._L = _load()
        self.NX, self.NY = int(nx), int(ny)
        self.NC = self.NX * self.NY
        self.G = cell
        self.strict = strict
        self.DIRS = list(dirs) if dirs is not None else make_dirs(strict)
        if wrong_way is None:
            wrong_way = 3.0 if strict else 1.6
        if diagonal is None:
            diagonal = 1.0 if strict else 1.2
        self.STEP = [list(s) for s in step] if step is not None else \
            make_step(self.DIRS, wrong_way, diagonal)
        self.TURN = [list(t) for t in turn] if turn is not None else \
            make_turn(self.DIRS, bend_45, bend_90)
        self.START = len(self.DIRS)
        if heuristic is None:
            heuristic = (10, 10, 30) if strict else (10, 4, 30)
        self.heuristic = tuple(heuristic)
        self.VIA_COST, self.RIP_COST, self.MAX_RIPS = via_cost, rip_cost, max_rips
        self.TRACK, self.TRACK_PWR = track, track_wide
        self.r_track, self.r_wide, self.r_via = tuple(r_track), tuple(r_wide), tuple(r_via)
        self.r_via_space = r_via_space
        self.pad_clear, self.pad_novia = tuple(pad_clear), pad_novia

        nd = len(self.DIRS)
        if len(self.TURN) != nd + 1 or any(len(t) != nd for t in self.TURN):
            raise ValueError("turn must have len(dirs) + 1 rows of len(dirs)")
        if len(self.STEP) != 2 or any(len(s) != nd for s in self.STEP):
            raise ValueError("step must have 2 rows of len(dirs)")
        I = ctypes.c_int32 * nd
        dx = I(*[d[0] for d in self.DIRS])
        dy = I(*[d[1] for d in self.DIRS])
        st = (ctypes.c_int32 * (2 * nd))(*(self.STEP[0] + self.STEP[1]))
        tu = (ctypes.c_int32 * ((nd + 1) * nd))(
            *[-1 if t is None else int(t) for row in self.TURN for t in row])
        hmax, hmin, hoff = self.heuristic
        self._r = L.rt_new(self.NX, self.NY, nd, dx, dy, st, tu, via_cost, rip_cost,
                           hmax, hmin, hoff)
        if not self._r:
            raise RouterError("rt_new failed (bad size or out of memory)")
        G32 = ctypes.c_int32 * self.NC
        # live views onto the C grids: r.T[layer][idx], r.W[layer][idx], r.novia[idx]
        self.T = [G32.from_address(L.rt_grid(self._r, 0, k)) for k in (0, 1)]
        self.W = [G32.from_address(L.rt_grid(self._r, 1, k)) for k in (0, 1)]
        self.novia = (ctypes.c_uint8 * self.NC).from_address(L.rt_novia(self._r))
        self.hardT = [G32.from_address(L.rt_hard(self._r, 0, k)) for k in (0, 1)]
        self.hardW = [G32.from_address(L.rt_hard(self._r, 1, k)) for k in (0, 1)]
        self.hard_novia = (ctypes.c_uint8 * self.NC).from_address(L.rt_hard_novia(self._r))

        self.edges = {}                     # net -> layer -> set of ((i,j),(i,j))
        self.stubs = {}                     # net -> [(layer, x0, y0, x1, y1)]
        self.vias = {}                      # net -> set of (i, j)
        self.padcell = {}                   # net -> {(layer, idx): (x, y) pad centre}
        self.width = {}
        self.failed = []
        self._ew = {}                       # net -> {(layer, edge): width}
        self.skip_joined_stubs = False      # True: no pad stub where a path ends on the net's own track
        self._sw = {}                     # (net, layer, x0, y0, x1, y1) -> width of that pad stub
        self.log = {}                       # net name -> [(net, path, wide, power)]
        self.rips = 0
        self._snap = None
        self.last_cost = None               # cost of the last successful search
        self.last_expanded = 0              # states expanded by the last search
        if edge_margin is not None:
            NX, NY = self.NX, self.NY
            for level, margin in enumerate(edge_margin):
                m = int(margin / cell + 0.999)
                for layer in (0, 1):        # same cells as the reference: a frame m cells wide
                    for (i0, j0, i1, j1) in ((0, 0, NX - 1, m - 1), (0, NY - m, NX - 1, NY - 1),
                                             (0, 0, m - 1, NY - 1), (NX - m, 0, NX - 1, NY - 1)):
                        _check(L.rt_claim_rect(self._r, BLOCK, level, layer, i0, j0, i1, j1))

    @classmethod
    def for_board(cls, width_mm, height_mm, cell=0.1, **kw):
        """Grid covering a width x height board (same size rule as gen_pcb.py)."""
        return cls(int(round(width_mm / cell)) + 1, int(round(height_mm / cell)) + 1, cell, **kw)

    def __del__(self):
        r = getattr(self, "_r", None)
        if r:
            self._r = None
            self._L.rt_free(r)

    @property
    def ewidth(self):
        """Flat {(net, layer, edge): width} dict, as in the reference (a copy)."""
        return {(net, layer, e): w for net, d in self._ew.items() for (layer, e), w in d.items()}

    # -- occupancy ---------------------------------------------------------
    def claim_rect(self, net, layers, x0, y0, x1, y1, d_t, d_w):
        """Permanent claim of the cells within d_t (T) / d_w (W) of a rectangle in mm."""
        G, NX, NY = self.G, self.NX, self.NY
        for level, d in ((0, d_t), (1, d_w)):
            i0 = max(0, int((x0 - d) / G - 1e-6) + (1 if x0 - d > 0 else 0))
            j0 = max(0, int((y0 - d) / G - 1e-6) + (1 if y0 - d > 0 else 0))
            i1 = min(NX - 1, int((x1 + d) / G + 1e-6))
            j1 = min(NY - 1, int((y1 + d) / G + 1e-6))
            for layer in layers:
                _check(self._L.rt_claim_rect(self._r, net, level, layer, i0, j0, i1, j1))

    def claim_disc(self, net, layer, i, j, r_t, r_w):
        _check(self._L.rt_claim_disc(self._r, net, layer, i, j, r_t, r_w))

    def block_cells(self, layer, i0, j0, i1, j1, levels=(0, 1)):
        """Permanently set cells i0..i1 x j0..j1 (inclusive) of one layer to BLOCK.

        Same effect as `r.T[layer][idx] = r.W[layer][idx] = BLOCK` on each cell,
        but also valid after snapshot()."""
        for level in levels:
            _check(self._L.rt_claim_rect(self._r, BLOCK, level, layer, i0, j0, i1, j1))

    def block_rect(self, x0, y0, x1, y1):
        G, NX, NY = self.G, self.NX, self.NY
        self.claim_rect(BLOCK, (0, 1), x0, y0, x1, y1, 0.0, 0.0)
        _check(self._L.rt_novia_rect(self._r, max(0, int(x0 / G)), max(0, int(y0 / G)),
                                     min(NX - 1, int(x1 / G) + 1), min(NY - 1, int(y1 / G) + 1)))

    def add_pad(self, net, layers, cx, cy, x0, y0, x1, y1, power):
        G, NX, NY = self.G, self.NX, self.NY
        self.claim_rect(net, layers, x0, y0, x1, y1,
                        self.pad_clear[0] + G * power, self.pad_clear[1] + G * power)
        d = self.pad_novia
        _check(self._L.rt_novia_rect(self._r, max(0, int((x0 - d) / G)), max(0, int((y0 - d) / G)),
                                     min(NX - 1, int((x1 + d) / G) + 1),
                                     min(NY - 1, int((y1 + d) / G) + 1)))
        cells = self.padcell.setdefault(net, {})
        for j in range(int(y0 / G) + 1, int(y1 / G) + 1):
            for i in range(int(x0 / G) + 1, int(x1 / G) + 1):
                if x0 < i * G < x1 and y0 < j * G < y1:
                    for layer in layers:
                        cells[(layer, j * NX + i)] = (cx, cy)

    def via_ok(self, idx, net):
        return _check(self._L.rt_via_ok(self._r, idx, net)) == 1

    # -- rip-up support ------------------------------------------------------
    def snapshot(self):
        """Everything committed so far (pads, plane stubs, escapes) becomes permanent."""
        _check(self._L.rt_snapshot(self._r))
        self._snap = copy.deepcopy((self.edges, self.stubs, self.vias, self._ew))
        self.log = {}
        self.rips = 0

    def rip(self, net):
        """Remove everything net committed since snapshot() (grids and bookkeeping)."""
        _check(self._L.rt_rip(self._r, net))
        if self._snap is None:
            return
        for live, snap in zip((self.edges, self.stubs, self.vias, self._ew), self._snap):
            if net in snap:
                live[net] = copy.deepcopy(snap[net])
            else:
                live.pop(net, None)

    def rebuild(self, drop):
        """Remove the nets named in drop (names as in self.log).

        Same result as the reference's restore-and-replay, done incrementally."""
        for name in drop:
            items = self.log.pop(name, None)
            if items:
                self.rip(items[0][0])

    def owners(self, level, path):
        """Nets other claims of which (made since snapshot) cover any cell of path on
        level 0 (T) or 1 (W).  Replaces the reference's self.own lookup."""
        a, p = _cells(path, self.NC)
        cap = 256
        while True:
            out = (ctypes.c_int32 * cap)()
            n = _check(self._L.rt_owners(self._r, level, p, len(a), out, cap))
            if n <= cap:
                return set(out[:n])
            cap = n

    def path_free(self, level, path, net):
        a, p = _cells(path, self.NC)
        return _check(self._L.rt_path_free(self._r, level, p, len(a), net)) == 1

    def owned_count(self, net):
        """Number of (grid, cell) claims net made since snapshot()."""
        return self._L.rt_owned_count(self._r, net)

    def search_stats(self):
        """{"pushes", "pops", "max_heap", "expanded"} of the last search."""
        out = (ctypes.c_int64 * 3)()
        self._L.rt_stats(self._r, out)
        return {"pushes": out[0], "pops": out[1], "max_heap": out[2],
                "expanded": self.last_expanded}

    # -- search ------------------------------------------------------------
    def encode(self, cells):
        """[(layer, idx)] -> array.array("i") accepted by search() as sources / targets."""
        return _cells(cells, self.NC)[0]

    def search(self, net, sources, targets, wide, goal=None, max_cost=None, via_goal=False,
               relax=False):
        """A* from a set of (layer, idx) to a set of (layer, idx).

        sources / targets may also be array.array("i") of encoded cells
        (layer * NC + idx), which skips the conversion (see encode()).
        Returns (path, new_via): path is a list of (layer, idx) or None.
        via_goal=True instead stops at the first cell where a via fits (or at a
        target); new_via is then True if the path ends at a new via site.
        relax=True may pass through cells claimed since snapshot() by other nets,
        at rip_cost per cell.  The path cost is left in self.last_cost.
        """
        NC = self.NC
        sa = sources if isinstance(sources, array.array) else _cells(sources, NC)[0]
        ta = targets if isinstance(targets, array.array) else _cells(targets, NC)[0]
        sp = sa.buffer_info()[0] if sa else None
        tp = ta.buffer_info()[0] if ta else None
        if goal:
            has_goal, gi, gj = 1, int(goal[0]), int(goal[1])
        else:
            has_goal, gi, gj = 0, 0, 0
        if max_cost is not None:
            has_max, mc = 1, int(max_cost // 1)      # d > max_cost <=> d > floor(max_cost)
        else:
            has_max, mc = 0, 0
        cost, new_via, expanded = ctypes.c_int64(), ctypes.c_int32(), ctypes.c_int64()
        n = _check(self._L.rt_search(self._r, net, sp, len(sa), tp, len(ta), 1 if wide else 0,
                                     has_goal, gi, gj, has_max, mc, 1 if via_goal else 0,
                                     1 if relax else 0, ctypes.byref(cost), ctypes.byref(new_via),
                                     ctypes.byref(expanded)))
        self.last_expanded = expanded.value
        if n == 0:
            self.last_cost = None
            return None, False
        buf = array.array("i", bytes(4 * n))
        self._L.rt_get_path(self._r, buf.buffer_info()[0], n)
        self.last_cost = cost.value
        return [divmod(c, NC) for c in buf], bool(new_via.value)

    # -- committing --------------------------------------------------------
    def commit(self, net, path, wide, power, end_via=False):
        G, NX = self.G, self.NX
        self.width[net] = self.TRACK_PWR if wide else self.width.get(net, self.TRACK)
        r_t, r_w = self.r_wide if wide else self.r_track
        r_t, r_w = r_t + int(power), r_w + int(power)     # power: extra clearance in grid cells
        a, p = _cells(path, self.NC)
        _check(self._L.rt_claim_path(self._r, net, p, len(a), r_t, r_w))
        edges = self.edges.setdefault(net, {0: set(), 1: set()})
        ew = self._ew.setdefault(net, {})
        w = self.TRACK_PWR if wide else self.TRACK
        cells = self.padcell.get(net, {})
        # An end that lands on copper this net already has there needs no stub to the pad centre:
        # it would only double up the connection.
        joined = set()
        if self.skip_joined_stubs:
            for layer, idx in (path[0], path[-1]):
                ij = (idx % NX, idx // NX)
                if any(ij in e for e in edges[layer]):
                    joined.add((layer, idx))
        for k in range(1, len(path)):
            layer, idx = path[k]
            pl, pidx = path[k - 1]
            ij = (idx % NX, idx // NX)
            if pl == layer:
                q = (pidx % NX, pidx // NX)
                e = (min(q, ij), max(q, ij))
                edges[layer].add(e)
                ew[(layer, e)] = w
            else:
                self.add_via(net, ij[0], ij[1], power)
        for layer, idx in (path[0], path[-1]):
            c = cells.get((layer, idx))
            if c and (layer, idx) not in joined:
                x, y = (idx % NX) * G, (idx // NX) * G
                st = self.stubs.setdefault(net, [])
                if abs(x - c[0]) > 1e-6:
                    st.append((layer, c[0], c[1], x, c[1]))
                    self._sw[(net,) + st[-1]] = w       # the stub is as wide as the track it ends
                if abs(y - c[1]) > 1e-6:
                    st.append((layer, x, c[1], x, y))
                    self._sw[(net,) + st[-1]] = w
        if end_via:
            idx = path[-1][1]
            self.add_via(net, idx % NX, idx // NX, power)

    def add_via(self, net, i, j, power):
        vs = self.vias.setdefault(net, set())
        if (i, j) in vs:
            return
        vs.add((i, j))
        p = int(power)
        _check(self._L.rt_add_via(self._r, net, i, j, self.r_via[0] + p, self.r_via[1] + p,
                                  self.r_via_space))

    # -- nets --------------------------------------------------------------
    def route_net(self, net, name, pads, wide, power):
        """pads: [(cx, cy, layers, cells, label, ...)].  Prim-style: grow one tree.

        If a pad cannot be reached, the path is searched again through other
        nets' tracks; the nets in the way are ripped up and returned so that
        the caller can route them again.  Rip-up needs snapshot() first.
        """
        G = self.G
        self.failed = [f for f in self.failed if f[0] != name]
        NC = self.NC
        todo = list(pads[1:])
        tree = set(pads[0][3])
        tree_a = _cells(tree, NC)[0]        # the same cells, encoded once and extended
        done = [(pads[0][0], pads[0][1])]
        ripped = []
        snapped = self._snap is not None
        while todo:
            todo.sort(key=lambda p: min(abs(p[0] - x) + abs(p[1] - y) for x, y in done))
            p = todo.pop(0)
            targets = set(p[3])
            goal = (int(round(p[0] / G)), int(round(p[1] / G)))
            path = None
            for w, relax in ((wide, False), (False, False), (wide, True), (False, True)):
                if relax and (not snapped or self.rips >= self.MAX_RIPS):
                    break
                path, _ = self.search(net, tree_a, targets, w, goal=goal, relax=relax)
                if path is None:
                    continue
                if relax:
                    li = 1 if w else 0
                    nets = self.owners(li, path)
                    nets.discard(net)
                    victims = [nm for nm, items in self.log.items() if items[0][0] in nets]
                    self.rips += 1
                    self.rebuild(victims)
                    ripped += victims
                    if not self.path_free(li, path, net):
                        path = None
                        continue
                break
            if path is None:
                self.failed.append((name, p[4]))
                continue
            self.log.setdefault(name, []).append((net, path, w, power))
            self.commit(net, path, w, power)
            for c in path:
                if c not in tree:
                    tree.add(c)
                    tree_a.append(c[0] * NC + c[1])
            for c in targets:
                if c not in tree:
                    tree.add(c)
                    tree_a.append(c[0] * NC + c[1])
            done.append((p[0], p[1]))
        return ripped

    def drop_to_plane(self, net, name, pad, joined, max_cost=600):
        """Short stub from an SMD pad to a via into the plane (or to an existing stub)."""
        for wide in (True, False):
            path, new_via = self.search(net, set(pad[3]), joined, wide,
                                        max_cost=max_cost, via_goal=True)
            if path is not None:
                self.commit(net, path, wide, False, end_via=new_via)
                if not wide:
                    self.width[net] = min(self.width.get(net, self.TRACK), self.TRACK)
                joined.update(path)
                return True
        self.failed.append((name, pad[4]))
        return False

    # -- output ------------------------------------------------------------
    def routes(self):
        """Plain-data result: (segments, vias).

        segments: [(layer, x0, y0, x1, y1, width, net)] in mm from the grid
        origin (layer 0 = F.Cu, 1 = B.Cu); collinear grid edges of equal width
        are merged and dangling spurs are dropped, as the reference emit() did.
        vias: [(x, y, net)].  Does not modify the router (the reference emit()
        pruned self.edges in place)."""
        G, NX = self.G, self.NX
        segs = []
        for net, layers in self.edges.items():
            ew = self._ew.get(net, {})
            for layer, edges0 in layers.items():
                edges = set(edges0)
                anchor = {(idx % NX, idx // NX) for (l, idx) in self.padcell.get(net, {})
                          if l == layer}
                anchor |= self.vias.get(net, set())
                while True:                 # drop spurs: unused ends of pin escapes
                    deg = {}
                    for a, b in edges:
                        deg[a] = deg.get(a, 0) + 1
                        deg[b] = deg.get(b, 0) + 1
                    spur = {e for e in edges
                            if any(deg[c] == 1 and c not in anchor for c in e)}
                    if not spur:
                        break
                    edges -= spur
                adj = {}
                for a, b in edges:
                    adj.setdefault(a, []).append(b)
                    adj.setdefault(b, []).append(a)
                seen = set()
                for a, b in sorted(edges):
                    if (a, b) in seen:
                        continue
                    d = (b[0] - a[0], b[1] - a[1])
                    w = ew[(layer, (a, b))]
                    start, end = a, b
                    seen.add((a, b))
                    while True:             # extend forward
                        nxt = (end[0] + d[0], end[1] + d[1])
                        e = (min(end, nxt), max(end, nxt))
                        if len(adj[end]) == 2 and e in edges and e not in seen \
                                and ew[(layer, e)] == w:
                            seen.add(e)
                            end = nxt
                        else:
                            break
                    while True:             # extend backward
                        nxt = (start[0] - d[0], start[1] - d[1])
                        e = (min(start, nxt), max(start, nxt))
                        if len(adj[start]) == 2 and e in edges and e not in seen \
                                and ew[(layer, e)] == w:
                            seen.add(e)
                            start = nxt
                        else:
                            break
                    segs.append((layer, start[0] * G, start[1] * G, end[0] * G, end[1] * G,
                                 w, net))
        for net, stubs in self.stubs.items():
            for layer, x0, y0, x1, y1 in stubs:
                segs.append((layer, x0, y0, x1, y1,
                             self._sw.get((net, layer, x0, y0, x1, y1), self.TRACK), net))
        vias = [(i * G, j * G, net) for net, vs in self.vias.items() for i, j in vs]
        return segs, vias

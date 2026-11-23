"""Shared helpers for the gridrouter tests: build identical scenes in the C
router and the pure-Python oracle, and compare their state."""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import gridrouter               # noqa: E402
import ref_router as ref        # noqa: E402

BLOCK = gridrouter.BLOCK
SCALE = float(os.environ.get("GRIDROUTER_TEST_SCALE", "1"))


def make_pair(nx, ny, strict, **kw):
    """(oracle Router, C Router) with the same constants."""
    ref.configure(nx, ny, strict=strict, **kw)
    return ref.Router(), gridrouter.Router(nx, ny, 0.1, strict=strict, **kw)


def make_c(nx, ny, strict, **kw):
    return gridrouter.Router(nx, ny, 0.1, strict=strict, **kw)


def grids(r):
    """All occupancy grids as plain lists (works for both router classes)."""
    return [list(r.T[0]), list(r.T[1]), list(r.W[0]), list(r.W[1]), list(r.novia)]


def bookkeeping(r):
    ew = r.ewidth
    return ({n: {l: set(s) for l, s in d.items()} for n, d in r.edges.items() if any(d.values())},
            {n: sorted(s) for n, s in r.stubs.items() if s},
            {n: set(s) for n, s in r.vias.items() if s},
            dict(ew))


def first_diff(a, b, nx):
    names = ("T0", "T1", "W0", "W1", "novia")
    for g, (x, y) in enumerate(zip(a, b)):
        if x != y:
            k = next(k for k in range(len(x)) if x[k] != y[k])
            return "%s cell (%d,%d): %r != %r" % (names[g], k % nx, k // nx, x[k], y[k])
    return None


def random_path(rng, nx, ny, dirs, n):
    """A random walk of n cells: mostly straight, sometimes turning or changing layer."""
    layer = rng.randint(0, 1)
    i, j = rng.randrange(nx), rng.randrange(ny)
    path = [(layer, j * nx + i)]
    d = rng.randrange(len(dirs))
    while len(path) < n:
        r = rng.random()
        if r < 0.08 and path[-1][0] == layer and len(path) > 1:
            layer = 1 - layer           # via: same cell, other layer
            path.append((layer, j * nx + i))
            continue
        if r < 0.3:
            d = rng.randrange(len(dirs))
        a, b = i + dirs[d][0], j + dirs[d][1]
        if not (0 <= a < nx and 0 <= b < ny):
            d = rng.randrange(len(dirs))
            continue
        i, j = a, b
        path.append((layer, j * nx + i))
    return path


def random_ops(rng, nx, ny, dirs, n_nets, post_snapshot=True):
    """A list of (method, args) building a random board; ("snapshot", ()) included."""
    W, H = (nx - 1) * 0.1, (ny - 1) * 0.1
    ops = []
    for _ in range(rng.randint(0, 2)):
        x0, y0 = rng.uniform(-0.5, W), rng.uniform(-0.5, H)
        ops.append(("block_rect", (x0, y0, x0 + rng.uniform(0.2, 2.0), y0 + rng.uniform(0.2, 2.0))))
    for _ in range(rng.randint(4, 4 + int(nx * ny / 300))):
        net = rng.randint(1, n_nets)
        layers = rng.choice([(0,), (1,), (0, 1)])
        hw, hh = rng.uniform(0.1, 0.8), rng.uniform(0.1, 0.8)
        # pads lie inside the grid (the reference would wrap negative indices)
        cx, cy = rng.uniform(hw + 0.05, W - hw - 0.05), rng.uniform(hh + 0.05, H - hh - 0.05)
        power = rng.choice([0, 0, 0, 1, 3])
        ops.append(("add_pad", (net, layers, cx, cy, cx - hw, cy - hh, cx + hw, cy + hh, power)))
    for _ in range(rng.randint(0, 3)):
        net = rng.randint(1, n_nets)
        ops.append(("commit", (net, random_path(rng, nx, ny, dirs, rng.randint(2, 40)),
                               rng.random() < 0.3, rng.choice([0, 0, 1, 3]), rng.random() < 0.3)))
    if rng.random() < 0.5:
        ops.append(("add_via", (rng.randint(1, n_nets), rng.randrange(nx), rng.randrange(ny),
                                rng.choice([0, 1]))))
    if rng.random() < 0.4:              # direct writes like gen_pcb's BUCK_AREA
        i0, j0 = rng.randrange(nx), rng.randrange(ny)
        ops.append(("setblock", (rng.randint(0, 1), i0, j0, min(nx - 1, i0 + rng.randint(0, 15)),
                                 min(ny - 1, j0 + rng.randint(0, 15)))))
    ops.append(("snapshot", ()))
    if post_snapshot:
        for _ in range(rng.randint(0, 6)):
            net = rng.randint(1, n_nets)
            ops.append(("commit", (net, random_path(rng, nx, ny, dirs, rng.randint(2, 60)),
                                   rng.random() < 0.3, rng.choice([0, 0, 1, 3]), rng.random() < 0.2)))
    return ops


def apply(r, ops):
    nx = r.NX if hasattr(r, "NX") else ref.NX
    for name, args in ops:
        if name == "setblock":
            layer, i0, j0, i1, j1 = args
            for j in range(j0, j1 + 1):
                for i in range(i0, i1 + 1):
                    r.T[layer][j * nx + i] = r.W[layer][j * nx + i] = BLOCK
        else:
            getattr(r, name)(*args)


def pad_cells(r, net, layers, box, nx):
    """Usable cells of a pad, as route_all computes them."""
    x0, y0, x1, y1 = box
    G = 0.1
    cells = []
    for j in range(int(y0 / G) + 1, int(y1 / G) + 1):
        for i in range(int(x0 / G) + 1, int(x1 / G) + 1):
            if x0 < i * G < x1 and y0 < j * G < y1:
                for layer in layers:
                    if r.T[layer][j * nx + i] in (0, net):
                        cells.append((layer, j * nx + i))
    return cells


def rng_for(seed):
    return random.Random(seed)

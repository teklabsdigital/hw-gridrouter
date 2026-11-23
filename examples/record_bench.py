"""Record a routing run to a trace file, without KiCad.

    python3 examples/record_bench.py OUT.grtrace                 # the tests/bench.py board
    python3 examples/record_bench.py OUT.grtrace --seed 3        # another random bench board
    python3 examples/record_bench.py OUT.grtrace --size small    # a small trace (< 1 MB)
    python3 examples/record_bench.py OUT.grtrace --timing        # also time Router vs TracedRouter

The bench board is tests/bench.py's (501 x 361 cells, its board(), setup() and
route_all() are imported, not copied).  The small board is 80 x 50 cells: a
net that must cross another (B.Cu is blocked there), so it is ripped up, and a
plane pad too far from any via site, so its drop fails.

A synthetic "BRD " chunk (tracefile.BoardWriter) gives the viewer a board to
draw: outline, pads as polygons on their layers with nets and footprints,
keep-outs, and in the "final" chunk the routed tracks and vias.
"""
import argparse
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tests"))

import gridrouter                                   # noqa: E402
from gridrouter import tracefile as tf              # noqa: E402
from gridrouter.recorder import Recorder, TracedRouter  # noqa: E402
import bench                                        # noqa: E402  (tests/bench.py: board, setup)
from common import pad_cells                        # noqa: E402

ORIGIN = (100.0, 50.0)          # board position of router cell (0, 0), mm


def llround(x):
    """Round half away from zero (spec section 9)."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def nm(mm, o):
    return llround(o * 1e6) + llround(mm * 1e6)


def board_chunk(label, nx, ny, cell, pads, blocks, names, routes=None, title="bench"):
    """BoardWriter for a synthetic board.  pads: [(name, net, layers, cx, cy, box, power)]
    in router mm; names: {router net: name}; routes: Router.routes() for "final"."""
    ox, oy = ORIGIN
    X = lambda v: nm(v, ox)                         # noqa: E731
    Y = lambda v: nm(v, oy)                         # noqa: E731
    bw = tf.BoardWriter(label, 1.6, (llround(ox * 1e6), llround(oy * 1e6)))
    cu = (bw.layer("F.Cu", 0, 0), bw.layer("B.Cu", 2, 0))
    # layer names and ids as KiCad 10's BOARD.GetStandardLayerName gives them (spec 7.18)
    silk = bw.layer("F.Silkscreen", 5)
    bw.layer("F.Mask", 1)
    edge = bw.layer("Edge.Cuts", 25)
    for name, th, er in (("F.Silkscreen", 0.01, None), ("F.Mask", 0.01, 3.3),
                         ("F.Cu", 0.035, None), ("dielectric 1", 1.51, 4.5), ("B.Cu", 0.035, None),
                         ("B.Mask", 0.01, 3.3)):
        bw.stackup_item(name, th, er)
    code = {}
    for net in sorted(names):
        code[net] = len(code) + 1
        bw.net(code[net], names[net])
    w, h = (nx - 1) * cell, (ny - 1) * cell
    bw.polyset(edge, [[[(X(0), Y(0)), (X(w), Y(0)), (X(w), Y(h)), (X(0), Y(h)), (X(0), Y(0))]]],
               role=tf.ROLE_OUTLINE, sub=50000)
    for k, (name, net, layers, cx, cy, box, power) in enumerate(pads):
        kc = code.get(net, -1)
        fp = bw.footprint("P%d" % (k + 1), name or "NC", "Synthetic:Pad", X(cx), Y(cy))
        x0, y0, x1, y1 = box
        rect = [[(X(x0), Y(y0)), (X(x1), Y(y0)), (X(x1), Y(y1)), (X(x0), Y(y1))]]
        for layer in layers:
            bw.polyset(cu[layer], [rect], net=kc, fp=fp, role=tf.ROLE_PAD)
        bw.padinfo(fp, kc, X(cx), Y(cy), 1, "1")
        m = 0.1
        bw.polyset(silk, [[[(X(x0 - m), Y(y0 - m)), (X(x1 + m), Y(y0 - m)), (X(x1 + m), Y(y1 + m)),
                            (X(x0 - m), Y(y1 + m)), (X(x0 - m), Y(y0 - m))]]],
                   fp=fp, role=tf.ROLE_GRAPHIC, sub=120000)
    for x0, y0, x1, y1 in blocks:
        x1, y1 = min(x1, w), min(y1, h)
        rect = [[(X(x0), Y(y0)), (X(x1), Y(y0)), (X(x1), Y(y1)), (X(x0), Y(y1))]]
        for layer in (0, 1):
            bw.polyset(cu[layer], [rect], role=tf.ROLE_RULE_AREA)
    bw.text(silk, -1, X(0.5), Y(0.5), 0.0, "gridrouter %s board (synthetic)" % title)
    if routes is not None:
        segs, vias = routes
        for layer, x0, y0, x1, y1, wd, net in segs:
            bw.track(cu[layer], code.get(net, -1), X(x0), Y(y0), X(x1), Y(y1), llround(wd * 1e6))
        for x, y, net in vias:
            bw.via(X(x), Y(y), 600000, 300000, code.get(net, -1), cu[0], cu[1])
            bw.hole(X(x), Y(y), 300000, fp=-1, net=code.get(net, -1))
    return bw


# -- bench board ------------------------------------------------------------------------------
def record_bench(out, seed, strict, rec_on=True, router_cls=TracedRouter):
    pads, blocks = bench.board(seed)
    names = {net: name for name, net, *_ in pads if name}
    r = router_cls(bench.NX, bench.NY, 0.1, strict=strict)
    rec = Recorder(out) if rec_on else None
    t = time.perf_counter()
    if rec:
        rec.board(board_chunk("placed", bench.NX, bench.NY, 0.1, pads, blocks, names))
        rec.attach(r, origin=ORIGIN, nets=names)
        rec.phase("setup: keep-out and pads")
    pads_of, power_of, net_of = bench.setup(r, pads, blocks)
    names_sorted = sorted(pads_of, key=lambda n: net_of[n])
    if rec:
        rec.phase("routing")
    bench.route_all(r, pads_of, power_of, net_of, names_sorted)
    t_route = time.perf_counter() - t
    if rec:
        rec.phase("done")
        rec.mark("%d rip-ups, %d failed pad connections" % (r.rips, len(r.failed)))
        rec.routes()
        rec.grids("final")
        rec.board(board_chunk("final", bench.NX, bench.NY, 0.1, pads, blocks, names, r.routes()))
        rec.close()
    return r, t_route


# -- small board ------------------------------------------------------------------------------
SMALL_NX, SMALL_NY = 80, 50


def small_pads():
    """[(name, net, layers, cx, cy, box, power)] of the small board, and its keep-outs."""
    def pad(name, net, cx, cy, hw, hh):
        return (name, net, (0,), cx, cy, (cx - hw, cy - hh, cx + hw, cy + hh), 0)
    pads = [pad("A", 1, 0.75, 2.5, 0.2, 0.2), pad("A", 1, 7.15, 2.5, 0.2, 0.2),
            pad("B", 2, 4.0, 1.0, 0.15, 0.15), pad("B", 2, 4.0, 4.0, 0.15, 0.15),
            pad("GND", 3, 7.0, 1.0, 0.15, 0.15), pad("GND", 3, 1.0, 4.0, 0.15, 0.15)]
    return pads, [(2.4, 3.3, 2.8, 3.7)]


def record_small(out):
    nx, ny = SMALL_NX, SMALL_NY
    pads, blocks = small_pads()
    names = {1: "A", 2: "B", 3: "GND"}
    r = TracedRouter(nx, ny, 0.1, max_rips=2)
    with Recorder(out) as rec:
        rec.board(board_chunk("placed", nx, ny, 0.1, pads, blocks, names, title="small"))
        rec.attach(r, origin=ORIGIN, nets=names)
        rec.phase("setup")
        for b in blocks:
            r.block_rect(*b)
        for name, net, layers, cx, cy, box, power in pads:
            r.add_pad(net, layers, cx, cy, *box, power)
        # B.Cu blocked except a pocket in the top right corner: no via under the tracks
        r.block_cells(1, 0, 0, 59, ny - 1)
        r.block_cells(1, 60, 15, nx - 1, ny - 1)
        rec.mark("B.Cu blocked except cols 60.. rows 0..14")
        pads_of, net_of = {}, {}
        for k, (name, net, layers, cx, cy, box, power) in enumerate(pads):
            net_of[name] = net
            cells = pad_cells(r, net, layers, box, nx)
            pads_of.setdefault(name, []).append((cx, cy, layers, cells,
                                                 "%s.%d" % (name, len(pads_of.get(name, [])))))
        rec.phase("plane drops")
        joined = set()
        for p in pads_of.pop("GND"):
            r.drop_to_plane(net_of["GND"], "GND", p, joined)
        rec.phase("routing")
        r.snapshot()
        queue = ["A", "B"]
        while queue:
            name = queue.pop(0)
            for v in r.route_net(net_of[name], name, pads_of[name], False, 0):
                if v not in queue:
                    queue.append(v)
        rec.phase("done")
        rec.routes()
        rec.board(board_chunk("final", nx, ny, 0.1, pads, blocks, names, r.routes(),
                              title="small"))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("output", help="trace file to write (.grtrace)")
    ap.add_argument("--size", choices=("bench", "small"), default="bench")
    ap.add_argument("--seed", type=int, default=1, help="bench board seed (default 1, as bench.py)")
    ap.add_argument("--strict", action="store_true", help="4 directions")
    ap.add_argument("--timing", action="store_true",
                    help="bench only: also time the same run with Router and with TracedRouter "
                         "without a recorder")
    a = ap.parse_args()
    if a.size == "small":
        r = record_small(a.output)
        print("small board %d x %d: %d rip-ups, failed %r" % (SMALL_NX, SMALL_NY, r.rips, r.failed))
    else:
        if a.timing:
            for label, cls, rec_on in (("Router", gridrouter.Router, False),
                                       ("TracedRouter, no recorder", TracedRouter, False),
                                       ("TracedRouter + Recorder", TracedRouter, True)):
                _, t = record_bench(a.output, a.seed, a.strict, rec_on, cls)
                print("%-28s setup + route_all: %.2f s" % (label, t))
        else:
            r, t = record_bench(a.output, a.seed, a.strict)
            print("bench board seed %d: routed and recorded in %.2f s, %d rip-ups, %d failed"
                  % (a.seed, t, r.rips, len(r.failed)))
    print("wrote %s (%d bytes)" % (a.output, os.path.getsize(a.output)))


if __name__ == "__main__":
    main()

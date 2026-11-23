"""Benchmark on a 501 x 361 grid (a 50 x 36 mm board at 0.1 mm), like obd2_dongle.

Builds a synthetic board (ICs with pin rows, two-pad passives, a keep-out),
routes part of it with the C router to get realistic obstacles, rebuilds
the same state in the pure-Python oracle, then times
  * A* searches as route_net issues them (thin and wide, goal heuristic, and
    relaxed through other nets), C versus oracle, checking the results agree;
  * commit (claims) and rip-up (C incremental rip vs oracle snapshot-restore
    plus replay);
  * a complete route_all-style run with rip-up, C only.

Run: python3 tests/bench.py [--strict] [--oracle-searches N]
"""
import argparse
import random
import time

from common import grids, make_c, make_pair, pad_cells

NX, NY = 501, 361


def board(seed=1):
    """pads: [(name, net, layers, cx, cy, box, power)], blocks: [(x0, y0, x1, y1)]."""
    rng = random.Random(seed)
    parts = []                          # (pins [(cx, cy, hw, hh)])
    for _ in range(10):                 # ICs: 2 or 4 rows of pins
        cx, cy = rng.uniform(6, 38), rng.uniform(5, 31)
        n = rng.choice([4, 6, 8, 10])
        pitch = rng.choice([0.5, 0.65, 0.8, 1.27])
        span = (n - 1) * pitch
        pins = []
        for side in range(rng.choice([2, 4])):
            for k in range(n):
                t = -span / 2 + k * pitch
                if side == 0:
                    pins.append((cx + t, cy - span / 2 - 1.0, 0.14, 0.4))
                elif side == 1:
                    pins.append((cx + t, cy + span / 2 + 1.0, 0.14, 0.4))
                elif side == 2:
                    pins.append((cx - span / 2 - 1.0, cy + t, 0.4, 0.14))
                else:
                    pins.append((cx + span / 2 + 1.0, cy + t, 0.4, 0.14))
        parts.append(pins)
    for _ in range(60):                 # 0402 / 0603 passives
        cx, cy = rng.uniform(2, 42), rng.uniform(2, 34)
        d = rng.choice([0.5, 0.8])
        if rng.random() < 0.5:
            parts.append([(cx - d, cy, 0.3, 0.3), (cx + d, cy, 0.3, 0.3)])
        else:
            parts.append([(cx, cy - d, 0.3, 0.3), (cx, cy + d, 0.3, 0.3)])
    # drop pins that overlap an earlier pin
    pins, boxes = [], []
    for pk, ps in enumerate(parts):
        for (cx, cy, hw, hh) in ps:
            b = (cx - hw - 0.15, cy - hh - 0.15, cx + hw + 0.15, cy + hh + 0.15)
            if b[0] < 1 or b[1] < 1 or b[2] > 43.5 or b[3] > 35 or any(
                    b[0] < o[2] and o[0] < b[2] and b[1] < o[3] and o[1] < b[3] for o in boxes):
                continue
            boxes.append(b)
            pins.append((pk, cx, cy, hw, hh))
    # nets: chain pins of nearby parts, 2 to 5 pins each
    rng.shuffle(pins)
    free = list(pins)
    pads, k = [], 0
    while len(free) >= 2 and k < 110:
        a = free.pop()
        if k % 2:                       # half the nets local, half across the board
            free.sort(key=lambda p: -((p[1] - a[1]) ** 2 + (p[2] - a[2]) ** 2))
        members = [a]
        for _ in range(rng.choice([1, 1, 1, 2, 3, 4])):
            if free:
                members.append(free.pop())
        k += 1
        power = 3 if k <= 4 else 1 if k <= 10 else 0
        for (_, cx, cy, hw, hh) in members:
            pads.append(("N%d" % k, k, (0,), cx, cy, (cx - hw, cy - hh, cx + hw, cy + hh), power))
    for j, (_, cx, cy, hw, hh) in enumerate(free):            # unconnected pins
        pads.append((None, 1000 + j, (0,), cx, cy, (cx - hw, cy - hh, cx + hw, cy + hh), 0))
    return pads, [(44.1, 0, 50, 36)]


def setup(r, pads, blocks):
    for b in blocks:
        r.block_rect(*b)
    pads_of, power_of, net_of = {}, {}, {}
    for name, net, layers, cx, cy, box, power in pads:
        r.add_pad(net, layers, cx, cy, *box, power)
        if name:
            power_of[name], net_of[name] = power, net
            pads_of.setdefault(name, []).append([cx, cy, layers, box, name])
    for name, ps in pads_of.items():
        for p in ps:
            p[3] = pad_cells(r, net_of[name], p[2], p[3], NX)
    return pads_of, power_of, net_of


def route_all(r, pads_of, power_of, net_of, names):
    r.snapshot()
    queue = list(names)
    while queue:
        name = queue.pop(0)
        for v in r.route_net(net_of[name], name, pads_of[name], power_of[name] == 3,
                             power_of[name]):
            if v not in queue:
                queue.append(v)


def queries(pads_of, net_of, names, r):
    """The first search route_net would issue for each net (tree = first pad)."""
    out = []
    for name in names:
        ps = pads_of[name]
        if len(ps) < 2 or not ps[0][3] or not ps[1][3]:
            continue
        p = min(ps[1:], key=lambda p: abs(p[0] - ps[0][0]) + abs(p[1] - ps[0][1]))
        goal = (int(round(p[0] / r.G)), int(round(p[1] / r.G)))
        out.append((net_of[name], set(ps[0][3]), set(p[3]), goal))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--oracle-searches", type=int, default=1000,
                    help="how many of the queries the (slow) oracle runs; default all")
    ap.add_argument("--full-oracle", action="store_true",
                    help="also route the whole board with the oracle (a few minutes)")
    args = ap.parse_args()
    strict = args.strict
    pads, blocks = board()
    print("board: %d x %d cells, %d pads, %d nets, %s" % (
        NX, NY, len(pads), len({p[0] for p in pads if p[0]}),
        "STRICT (4 directions)" if strict else "8 directions"))

    # 1. obstacles: route the first half of the nets with the C router
    cr = make_c(NX, NY, strict)
    pads_of, power_of, net_of = setup(cr, pads, blocks)
    names = sorted(pads_of, key=lambda n: net_of[n])
    first, rest = names[: len(names) // 2], names[len(names) // 2:]
    t = time.perf_counter()
    route_all(cr, pads_of, power_of, net_of, first)
    print("C: routed %d nets as obstacles in %.2f s (%d rip-ups)" % (
        len(first), time.perf_counter() - t, cr.rips))

    # same state in the oracle
    pr, cr2 = make_pair(NX, NY, strict)
    t = time.perf_counter()
    setup(pr, pads, blocks)
    t_setup_ref = time.perf_counter() - t
    t = time.perf_counter()
    setup(cr2, pads, blocks)
    t_setup_c = time.perf_counter() - t
    pr.snapshot()
    cr2.snapshot()
    log = [(name, item) for name, items in cr.log.items() for item in items]
    t = time.perf_counter()
    for name, item in log:
        pr.log.setdefault(name, []).append(item)
        pr.commit(*item)
    t_commit_ref = time.perf_counter() - t
    t = time.perf_counter()
    for name, item in log:
        cr2.log.setdefault(name, []).append(item)
        cr2.commit(*item)
    t_commit_c = time.perf_counter() - t
    assert grids(pr) == grids(cr2), "oracle and C disagree on the obstacle board"
    print("setup (edge frame, pads, keep-out): oracle %.2f s, C %.3f s (incl. Python pad loops)"
          % (t_setup_ref, t_setup_c))
    print("commit %d paths: oracle %.3f s, C %.4f s  (%.0fx)" % (
        len(log), t_commit_ref, t_commit_c, t_commit_ref / max(t_commit_c, 1e-9)))

    # 2. searches
    qs = queries(pads_of, net_of, rest, cr2)
    kinds = [("thin", False, False), ("wide", True, False), ("relax", False, True)]
    sub = random.Random(7).sample(qs, min(args.oracle_searches, len(qs)))
    print("%d search queries (first connection of each unrouted net); the oracle runs %d of them"
          % (len(qs), len(sub)))
    tot_ref = tot_c = 0.0
    for label, wide, relax in kinds:
        t = time.perf_counter()
        res_ref, exp_ref = [], 0
        for net, src, tgt, goal in sub:
            res_ref.append((pr.search(net, src, tgt, wide, goal=goal, relax=relax), pr.last_cost))
            exp_ref += pr.last_expanded
        t_ref = time.perf_counter() - t
        reps = 5
        t = time.perf_counter()
        for _ in range(reps):
            res_c = []
            for net, src, tgt, goal in sub:
                res_c.append((cr2.search(net, src, tgt, wide, goal=goal, relax=relax),
                              cr2.last_cost))
        t_c = (time.perf_counter() - t) / reps
        same = res_ref == res_c
        t = time.perf_counter()
        exp_all = found = 0
        for net, src, tgt, goal in qs:
            found += cr2.search(net, src, tgt, wide, goal=goal, relax=relax)[0] is not None
            exp_all += cr2.last_expanded
        t_all = time.perf_counter() - t
        tot_ref += t_ref
        tot_c += t_c
        print("search %-5s: same %d queries: oracle %.2f s (%.1f searches/s, %.0f k states/s), "
              "C %.4f s (%.0f searches/s, %.1f M states/s), speed-up %.0fx, identical: %s"
              % (label, len(sub), t_ref, len(sub) / t_ref, exp_ref / t_ref / 1e3, t_c,
                 len(sub) / t_c, exp_ref / t_c / 1e6, t_ref / t_c, same))
        print("               all %d queries in C: %.3f s (%.0f searches/s), %d found a path"
              % (len(qs), t_all, len(qs) / t_all, found))
        assert same
    print("search total: oracle %.2f s, C %.4f s, speed-up %.0fx" % (tot_ref, tot_c, tot_ref / tot_c))

    # 3. rip-up: drop one net, C incremental vs oracle restore + replay
    victim = max(cr2.log, key=lambda n: sum(len(i[1]) for i in cr2.log[n]))
    t = time.perf_counter()
    pr.rebuild([victim])
    t_rip_ref = time.perf_counter() - t
    t = time.perf_counter()
    cr2.rebuild([victim])
    t_rip_c = time.perf_counter() - t
    assert grids(pr) == grids(cr2)
    print("rip-up of %s (%d of %d logged nets): oracle rebuild %.2f s, C %.5f s (%.0fx); grids identical"
          % (victim, 1, len(log and cr.log), t_rip_ref, t_rip_c, t_rip_ref / max(t_rip_c, 1e-9)))

    # 4. a whole board
    cr3 = make_c(NX, NY, strict)
    t = time.perf_counter()
    pads_of, power_of, net_of = setup(cr3, pads, blocks)
    route_all(cr3, pads_of, power_of, net_of, names)
    segs, vias = cr3.routes()
    t_c = time.perf_counter() - t
    print("C full run: %d nets, %.2f s, %d rip-ups, %d segments, %d vias, %d pad connections failed"
          % (len(names), t_c, cr3.rips, len(segs), len(vias), len(cr3.failed)))
    if args.full_oracle:
        pr3 = make_pair(NX, NY, strict)[0]
        t = time.perf_counter()
        pads_of, power_of, net_of = setup(pr3, pads, blocks)
        route_all(pr3, pads_of, power_of, net_of, names)
        segs_r, vias_r = pr3.routes()
        t_r = time.perf_counter() - t
        same = (grids(pr3) == grids(cr3) and sorted(segs_r) == sorted(segs)
                and sorted(vias_r) == sorted(vias) and pr3.failed == cr3.failed
                and pr3.rips == cr3.rips)
        print("oracle full run: %.1f s, %d rip-ups; C is %.0fx faster; identical result: %s"
              % (t_r, pr3.rips, t_r / t_c, same))
        assert same


if __name__ == "__main__":
    main()

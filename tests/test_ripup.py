"""Incremental rip-up.

1. A/B: after committing nets A and B (interleaved, with vias, end vias,
   overlapping claims) ripping A leaves the grids and bookkeeping identical
   to a fresh router that only committed B; ripping B as well gives back the
   snapshot.  C only, many random cases, plus the oracle's rebuild() as a
   third opinion on a subset.
2. A/B with real routing: route_net(A), route_net(B), rip A, compare with a
   fresh router that committed B's logged paths.
3. End to end: a congested random board routed net by net with rip-up and
   re-queueing (the route_all loop) by the C router and by the oracle;
   everything (grids, edges, stubs, vias, widths, log order, ripped lists,
   failures, rip count, final segments) must agree after every net.

Run: python3 tests/test_ripup.py
"""
import unittest

from common import gridrouter  # noqa: F401  (common puts the package on sys.path)
from common import (SCALE, apply, bookkeeping, first_diff, grids, make_c, make_pair,
                    pad_cells, random_ops, random_path, ref, rng_for)

STATS = {}


def ab_case(seed, strict, with_oracle):
    rng = rng_for(seed)
    nx, ny = rng.randint(30, 120), rng.randint(25, 90)
    dirs = gridrouter.make_dirs(strict)
    ops = [o for o in random_ops(rng, nx, ny, dirs, 6, post_snapshot=False)]
    A, B = 7, 8                         # nets that only exist after the snapshot
    if rng.random() < 0.5:
        A, B = rng.randint(1, 6), rng.randint(1, 6)     # nets that also own pads
        if A == B:
            B = A % 6 + 1
    commits = []
    for _ in range(rng.randint(1, 6)):
        net = rng.choice((A, B))
        commits.append((net, random_path(rng, nx, ny, dirs, rng.randint(2, 80)),
                        rng.random() < 0.4, rng.choice([0, 0, 1, 3]), rng.random() < 0.3))
    commits.append((A, random_path(rng, nx, ny, dirs, 30), False, 0, True))
    commits.append((B, random_path(rng, nx, ny, dirs, 30), True, 1, False))
    vias = [(rng.choice((A, B)), rng.randrange(nx), rng.randrange(ny), rng.choice((0, 3)))
            for _ in range(rng.randint(0, 3))]

    r1, r2 = make_c(nx, ny, strict), make_c(nx, ny, strict)
    for r in (r1, r2):
        apply(r, ops)
    snap = grids(r1)
    for c in commits:
        r1.log.setdefault("A" if c[0] == A else "B", []).append(c[:4])
        r1.commit(*c)
    for v in vias:
        r1.add_via(*v)
    for c in commits:
        if c[0] == B:
            r2.commit(*c)
    for v in vias:
        if v[0] == B:
            r2.add_via(*v)
    both = grids(r1)
    r1.rebuild(["A"])
    out = {"changed": both != grids(r1)}
    d = first_diff(grids(r1), grids(r2), nx)
    if d:
        return "rip A vs fresh B: %s" % d, out
    if bookkeeping(r1) != bookkeeping(r2):
        return "rip A vs fresh B: bookkeeping differs", out
    if r1.owned_count(A) != 0:
        return "A still owns cells", out
    r1.rebuild(["B"])
    d = first_diff(grids(r1), snap, nx)
    if d:
        return "rip A and B vs snapshot: %s" % d, out
    if with_oracle:
        ref.configure(nx, ny, strict=strict)
        pr = ref.Router()
        apply(pr, ops)
        for c in commits:
            pr.log.setdefault("A" if c[0] == A else "B", []).append(c[:4])
            pr.commit(*c)
        for v in vias:
            pr.add_via(*v)
        if grids(pr) != both:
            return "oracle disagrees before rip", out
        pr.rebuild(["A"])
        # the oracle's replay drops add_via calls that are not in the log and the
        # end_via flag (its log has no such field; route_net never sets it):
        # compare against the C router committing what the log holds
        r3 = make_c(nx, ny, strict)
        apply(r3, ops)
        for c in commits:
            if c[0] == B:
                r3.commit(*c[:4])
        d = first_diff(grids(pr), grids(r3), nx)
        if d:
            return "oracle rebuild(A) vs C: %s" % d, out
    return None, out


def make_board(seed, strict, nx, ny, n_nets):
    """Random pads: a list of (name, net, layers, cx, cy, box, power)."""
    rng = rng_for(seed)
    W, H = (nx - 1) * 0.1, (ny - 1) * 0.1
    pads = []
    for k in range(n_nets):
        name = "N%d" % (k + 1)
        power = 3 if rng.random() < 0.15 else 1 if rng.random() < 0.2 else 0
        for _ in range(rng.randint(2, 4)):
            hw, hh = rng.choice([(0.15, 0.4), (0.4, 0.15), (0.3, 0.3), (0.6, 0.6)])
            cx = round(rng.uniform(1.2, W - 1.2), 2)
            cy = round(rng.uniform(1.2, H - 1.2), 2)
            layers = rng.choice([(0,), (0,), (1,), (0, 1)])
            pads.append((name, k + 1, layers, cx, cy, (cx - hw, cy - hh, cx + hw, cy + hh), power))
    for _ in range(rng.randint(2, 6)):  # a plane net: each pad drops to a via
        cx, cy = round(rng.uniform(1.2, W - 1.2), 2), round(rng.uniform(1.2, H - 1.2), 2)
        pads.append(("GND", n_nets + 1, (0,), cx, cy, (cx - 0.3, cy - 0.3, cx + 0.3, cy + 0.3), 0))
    blocks = []
    if rng.random() < 0.5:
        x0, y0 = rng.uniform(0, W - 2), rng.uniform(0, H - 2)
        blocks.append((x0, y0, x0 + rng.uniform(0.5, 2), y0 + rng.uniform(0.5, 2)))
    return pads, blocks


def setup_board(r, nx, pads, blocks):
    """Pads, keep-outs, plane drops, snapshot: the part of route_all before routing."""
    for b in blocks:
        r.block_rect(*b)
    pads_of, power_of = {}, {}
    for name, net, layers, cx, cy, box, power in pads:
        r.add_pad(net, layers, cx, cy, *box, power)
        power_of[name] = power
        pads_of.setdefault(name, []).append([cx, cy, layers, box, "%s.%d" % (name, len(pads_of.get(name, [])))])
    net_of = {p[0]: p[1] for p in pads}
    for name, ps in pads_of.items():
        for p in ps:
            p[3] = pad_cells(r, net_of[name], p[2], p[3], nx)
    joined = set()
    for p in sorted(pads_of.pop("GND", []), key=lambda p: (p[0], p[1])):
        r.drop_to_plane(net_of["GND"], "GND", p, joined)
    r.snapshot()
    return pads_of, power_of, net_of


def route_board(r, nx, pads, blocks, check=None, max_steps=200):
    """The route_all loop of gen_pcb.py, cut down.  check(step, name, ripped) after each net."""
    pads_of, power_of, net_of = setup_board(r, nx, pads, blocks)
    queue = sorted(pads_of, key=lambda n: -len(pads_of[n]))
    steps = 0
    while queue and steps < max_steps:
        steps += 1
        name = queue.pop(0)
        wide = power_of[name] == 3
        ripped = r.route_net(net_of[name], name, pads_of[name], wide, power_of[name])
        for v in ripped:
            if v not in queue:
                queue.append(v)
        if check:
            check(steps, name, ripped)
    return steps


class RipUp(unittest.TestCase):
    def test_ab_random(self):
        n = max(10, int(400 * SCALE))
        changed = 0
        for strict in (False, True):
            for k in range(n):
                err, out = ab_case(20000 + k + (100000 if strict else 0), strict,
                                   with_oracle=k % 8 == 0)
                self.assertIsNone(err, "case %d strict=%s: %s" % (k, strict, err))
                changed += out["changed"]
        STATS["ab"] = (2 * n, changed)

    def test_ab_routed(self):
        cases = ok = 0
        for k in range(max(4, int(40 * SCALE))):
            strict = k % 2 == 1
            nx, ny = 100, 70
            pads, blocks = make_board(30000 + k, strict, nx, ny, 2)
            r1, r2 = make_c(nx, ny, strict), make_c(nx, ny, strict)
            route_board(r1, nx, pads, blocks)
            if "N1" not in r1.log or "N2" not in r1.log:
                continue
            cases += 1
            setup_board(r2, nx, pads, blocks)
            for net, path, w, power in r1.log["N2"]:
                r2.commit(net, path, w, power)
            r1.rebuild(["N1"])
            d = first_diff(grids(r1), grids(r2), nx)
            self.assertIsNone(d, "case %d: %s" % (k, d))
            self.assertEqual(bookkeeping(r1), bookkeeping(r2))
            self.assertEqual([sorted(x) for x in r1.routes()], [sorted(x) for x in r2.routes()])
            ok += 1
        self.assertGreater(cases, 0)
        STATS["ab_routed"] = (cases, ok)

    def test_end_to_end_vs_oracle(self):
        total_rips = total_steps = boards = 0
        for k in range(max(2, int(16 * SCALE))):
            strict = k % 2 == 1
            rng = rng_for(40000 + k)
            nx, ny = rng.randint(70, 130), rng.randint(50, 90)
            pads, blocks = make_board(40000 + k, strict, nx, ny, rng.randint(6, 14))
            pr, cr = make_pair(nx, ny, strict, max_rips=25)
            trace = []

            def rec(step, name, ripped, r):
                trace.append((step, name, list(ripped), grids(r), bookkeeping(r), list(r.log),
                              list(r.failed), r.rips, r.routes()))
            route_board(pr, nx, pads, blocks, lambda *a: rec(*a, pr))
            ref_trace, trace = trace, []
            route_board(cr, nx, pads, blocks, lambda *a: rec(*a, cr))
            self.assertEqual(len(ref_trace), len(trace))
            for a, b in zip(ref_trace, trace):
                self.assertEqual(a[:3], b[:3], "board %d step %d: net/ripped" % (k, a[0]))
                d = first_diff(a[3], b[3], nx)
                self.assertIsNone(d, "board %d step %d (%s): %s" % (k, a[0], a[1], d))
                self.assertEqual(a[4], b[4], "board %d step %d: bookkeeping" % (k, a[0]))
                self.assertEqual(a[5:8], b[5:8], "board %d step %d: log/failed/rips" % (k, a[0]))
                sa, va = a[8]
                sb, vb = b[8]
                # Same geometry and nets.  Widths match too, except pad stubs: the reference draws
                # them thin, this library draws them as wide as the track they end.
                geo = lambda s: sorted(x[:5] + x[6:] for x in s)
                self.assertEqual(geo(sa), geo(sb), "board %d step %d: segments" % (k, a[0]))
                wa = {x[:5] + x[6:]: x[5] for x in sa}
                for x in sb:
                    w = wa[x[:5] + x[6:]]
                    self.assertTrue(x[5] == w or x[5] > w, "board %d step %d: width" % (k, a[0]))
                self.assertEqual(sorted(va), sorted(vb))
            boards += 1
            total_steps += len(trace)
            total_rips += cr.rips
        STATS["e2e"] = (boards, total_steps, total_rips)
        self.assertGreater(total_rips, 0, "no rip-up happened; the test boards are too easy")


if __name__ == "__main__":
    prog = unittest.main(exit=False, verbosity=2)
    if "ab" in STATS:
        print("A/B random: %d cases, all identical to a fresh router (%d where A changed the grids)"
              % STATS["ab"])
    if "ab_routed" in STATS:
        print("A/B routed: %d cases, %d identical" % STATS["ab_routed"])
    if "e2e" in STATS:
        print("end to end: %d boards, %d route_net calls, %d rip-ups, all identical to the oracle"
              % STATS["e2e"])
    raise SystemExit(0 if prog.result.wasSuccessful() else 1)

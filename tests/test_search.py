"""The C search against the pure-Python reference on random boards, in every mode.

Checks per search: the path cost (exact), the path itself, the new_via flag
and the number of expanded states.  Because the C heap orders entries by the
same (f, d, layer, idx, dir) key as the reference's heapq tuples, paths are
expected to be identical, not merely equal in cost; the test reports path
differences separately from cost differences so a tie-only difference would
be visible as such.

Run: python3 tests/test_search.py   (GRIDROUTER_TEST_SCALE=0.2 for a quick run)
"""
import time
import unittest

from common import (SCALE, apply, first_diff, grids, make_pair, random_ops, rng_for, ref,
                    bookkeeping)

STATS = {}


def random_search(rng, nx, ny, n_nets, pr, snapped):
    """Random search arguments: (net, sources, targets, wide, goal, max_cost, via_goal, relax)."""
    nc = nx * ny
    net = rng.randint(1, n_nets + 1)

    def cells(k):
        out = set()
        for _ in range(k):
            for _ in range(30):         # mostly cells that are free for this net
                c = (rng.randint(0, 1), rng.randrange(nc))
                if pr.T[c[0]][c[1]] in (0, net) or rng.random() < 0.1:
                    break
            out.add(c)
        return out
    own = list(pr.padcell.get(net, {}))         # this net's pad cells
    if own and rng.random() < 0.5:
        sources = set(rng.sample(own, min(len(own), rng.randint(1, 20))))
    else:
        sources = cells(rng.randint(1, 4))
    if own and rng.random() < 0.4:
        targets = set(rng.sample(own, min(len(own), rng.randint(1, 20))))
    else:
        targets = cells(rng.randint(1, 12))
    goal = None
    if rng.random() < 0.6:
        if rng.random() < 0.7:
            idx = next(iter(targets))[1]
            goal = (idx % nx, idx // nx)
        else:
            goal = (rng.randrange(nx), rng.randrange(ny))
    max_cost = None
    if rng.random() < 0.25:
        max_cost = rng.choice([0, 50, 300, 600, 1500, 600.5])
    via_goal = rng.random() < 0.25
    relax = snapped and rng.random() < 0.35
    return net, sources, targets, rng.random() < 0.4, goal, max_cost, via_goal, relax


class SearchVsOracle(unittest.TestCase):
    def run_config(self, strict, n_scenes, n_search, seed0):
        modes = {}
        mism_cost = mism_path = mism_other = 0
        n_cases = found = 0
        t_ref = t_c = 0.0
        for sc in range(n_scenes):
            rng = rng_for(seed0 + sc)
            nx, ny = rng.randint(25, 90), rng.randint(20, 70)
            pr, cr = make_pair(nx, ny, strict)
            n_nets = rng.randint(2, 8)
            ops = random_ops(rng, nx, ny, ref.DIRS, n_nets, post_snapshot=rng.random() < 0.8)
            apply(pr, ops)
            apply(cr, ops)
            d = first_diff(grids(pr), grids(cr), nx)
            self.assertIsNone(d, "scene %d grids differ: %s" % (sc, d))
            self.assertEqual(bookkeeping(pr), bookkeeping(cr), "scene %d bookkeeping" % sc)
            for k in range(0, nx * ny, 7):      # via_ok on a sample of cells, a few nets
                for net in (1, 2, n_nets):
                    self.assertEqual(pr.via_ok(k, net), cr.via_ok(k, net))
            for q in range(n_search):
                args = random_search(rng, nx, ny, n_nets, pr, True)
                net, src, tgt, wide, goal, mc, vg, rl = args
                t = time.perf_counter()
                p_ref, nv_ref = pr.search(net, src, tgt, wide, goal=goal, max_cost=mc,
                                          via_goal=vg, relax=rl)
                t_ref += time.perf_counter() - t
                t = time.perf_counter()
                p_c, nv_c = cr.search(net, src, tgt, wide, goal=goal, max_cost=mc,
                                      via_goal=vg, relax=rl)
                t_c += time.perf_counter() - t
                n_cases += 1
                key = ("wide" if wide else "thin", "goal" if goal else "nogoal",
                       "max" if mc is not None else "-", "via_goal" if vg else "-",
                       "relax" if rl else "-")
                modes[key] = modes.get(key, 0) + 1
                if p_ref is not None:
                    found += 1
                if (p_ref is None) != (p_c is None) or pr.last_cost != cr.last_cost:
                    mism_cost += 1
                    self.fail("scene %d search %d %r: cost %r vs %r" %
                              (sc, q, key, pr.last_cost, cr.last_cost))
                if p_ref != p_c:
                    mism_path += 1
                if nv_ref != nv_c or pr.last_expanded != cr.last_expanded:
                    mism_other += 1
        STATS["strict" if strict else "non-strict"] = dict(
            scenes=n_scenes, searches=n_cases, found=found, cost_mismatch=mism_cost,
            path_mismatch=mism_path, flag_or_expanded_mismatch=mism_other,
            modes=len(modes), t_ref=t_ref, t_c=t_c)
        self.assertEqual(mism_path, 0)
        self.assertEqual(mism_other, 0)
        # every combination of the five switches should have been exercised
        self.assertGreaterEqual(len(modes), 28, sorted(modes))

    def test_non_strict(self):
        self.run_config(False, max(4, int(250 * SCALE)), 20, 1000)

    def test_strict(self):
        self.run_config(True, max(4, int(250 * SCALE)), 20, 5000)


if __name__ == "__main__":
    prog = unittest.main(exit=False, verbosity=2)
    for k, v in STATS.items():
        print("%-10s %d scenes, %d searches (%d found a path): cost mismatches %d, "
              "path mismatches %d, flag/expanded mismatches %d, %d mode combinations; "
              "time oracle %.1f s, C %.3f s" %
              (k, v["scenes"], v["searches"], v["found"], v["cost_mismatch"],
               v["path_mismatch"], v["flag_or_expanded_mismatch"], v["modes"],
               v["t_ref"], v["t_c"]))
    raise SystemExit(0 if prog.result.wasSuccessful() else 1)

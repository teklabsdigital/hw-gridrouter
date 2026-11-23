"""API details and error handling of gridrouter.Router."""
import os
import unittest

from common import BLOCK, gridrouter, make_c


class Api(unittest.TestCase):
    def test_build_is_cached(self):
        lib = gridrouter.build()
        src = os.path.join(os.path.dirname(gridrouter.__file__), "gridrouter.c")
        self.assertGreaterEqual(os.path.getmtime(lib), os.path.getmtime(src))
        self.assertEqual(gridrouter.build(), lib)

    def test_views_and_direct_writes(self):
        r = make_c(40, 30, False)
        nx = r.NX
        self.assertEqual(r.T[0][0], BLOCK)                  # edge frame
        self.assertEqual(r.T[0][15 * nx + 20], 0)
        r.T[1][15 * nx + 20] = r.W[1][15 * nx + 20] = BLOCK  # as gen_pcb does before snapshot
        r.snapshot()
        r.commit(3, [(1, 10 * nx + 20), (1, 11 * nx + 20)], False, 0)
        self.assertEqual(r.T[1][10 * nx + 20], 3)
        r.rip(3)
        self.assertEqual(r.T[1][10 * nx + 20], 0)
        self.assertEqual(r.T[1][15 * nx + 20], BLOCK)       # kept: it was in the snapshot
        self.assertEqual(r.owned_count(3), 0)

    def test_block_cells_after_snapshot_is_permanent(self):
        r = make_c(40, 30, False)
        nx = r.NX
        r.snapshot()
        r.commit(2, [(0, 10 * nx + 10), (0, 10 * nx + 11)], False, 0)
        r.block_cells(0, 12, 12, 14, 14)
        r.rip(2)
        self.assertEqual(r.T[0][13 * nx + 13], BLOCK)
        self.assertEqual(r.W[0][13 * nx + 13], BLOCK)
        self.assertEqual(r.T[0][10 * nx + 10], 0)

    def test_relax_needs_snapshot(self):
        r = make_c(40, 30, False)
        with self.assertRaises(gridrouter.RouterError):
            r.search(1, {(0, 100)}, {(0, 200)}, False, relax=True)

    def test_bad_cell(self):
        r = make_c(40, 30, False)
        with self.assertRaises(gridrouter.RouterError):
            r.search(1, {(0, 10 ** 6)}, {(0, 200)}, False)

    def test_cost_overflow(self):
        r = make_c(40, 30, False, via_cost=2 ** 31 - 5)
        nx = r.NX
        with self.assertRaises(gridrouter.RouterError):
            r.search(1, {(0, 15 * nx + 10)}, {(1, 15 * nx + 30)}, False)

    def test_custom_tables(self):
        dirs = [(1, 0, 7), (-1, 0, 7), (0, 1, 7), (0, -1, 7)]
        r = gridrouter.Router(40, 30, 0.1, dirs=dirs, step=[[7, 7, 21, 21], [21, 21, 7, 7]],
                              turn=gridrouter.make_turn(dirs, 5, 9), heuristic=(7, 7, 0),
                              via_cost=100, edge_margin=None)
        nx = r.NX
        self.assertEqual(r.T[0][0], 0)                      # no edge frame
        p, _ = r.search(1, {(0, 5 * nx + 5)}, {(0, 5 * nx + 15)}, False, goal=(15, 5))
        self.assertEqual(len(p), 11)
        self.assertEqual(r.last_cost, 70)

    def test_for_board(self):
        r = gridrouter.Router.for_board(50.0, 36.0)
        self.assertEqual((r.NX, r.NY), (501, 361))

    def test_owners_and_routes(self):
        r = make_c(60, 40, False)
        nx = r.NX
        r.add_pad(5, (0,), 1.0, 2.0, 0.9, 1.9, 1.1, 2.1, 0)   # anchors the track's start (cell 10, 20)
        r.snapshot()
        path = [(0, 20 * nx + k) for k in range(10, 30)] + [(1, 20 * nx + 29)]
        r.commit(5, path, False, 0)
        self.assertEqual(r.owners(0, [(0, 20 * nx + 15)]), {5})
        self.assertEqual(r.owners(0, [(1, 20 * nx + 15)]), set())
        segs, vias = r.routes()
        self.assertEqual(len(segs), 1)
        self.assertEqual(segs[0][0], 0)
        self.assertAlmostEqual(segs[0][1], 1.0)
        self.assertAlmostEqual(segs[0][3], 2.9)
        self.assertEqual(len(vias), 1)
        self.assertEqual(vias[0][2], 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""gridrouter.kicad.emit on an in-memory pcbnew board.

Needs KiCad's Python (pcbnew); skipped elsewhere:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tests/test_kicad_emit.py
"""
import unittest

from common import gridrouter

try:
    import pcbnew
except ImportError:                     # not KiCad's Python
    pcbnew = None

from gridrouter.kicad import emit


@unittest.skipIf(pcbnew is None, "pcbnew not available (run with KiCad's Python)")
class Emit(unittest.TestCase):
    def test_emit(self):
        r = gridrouter.Router(120, 80)
        r.add_pad(1, (0,), 2.0, 2.0, 1.8, 1.7, 2.2, 2.3, 0)
        r.add_pad(1, (0,), 9.0, 6.0, 8.8, 5.7, 9.2, 6.3, 0)
        r.snapshot()
        pads = [[2.0, 2.0, (0,), list(r.padcell[1]), "A.1"]]
        pads.append([9.0, 6.0, (0,), [c for c in r.padcell[1] if c[1] % r.NX > 80], "B.1"])
        self.assertEqual(r.route_net(1, "SIG", pads, False, 0), [])
        self.assertEqual(r.failed, [])
        board = pcbnew.BOARD()
        ni = pcbnew.NETINFO_ITEM(board, "SIG")
        board.Add(ni)
        n_seg, n_via = emit(board, r, {"SIG": ni}, {1: "SIG"}, pcbnew, origin=(100.0, 80.0))
        segs, vias = r.routes()
        self.assertEqual((n_seg, n_via), (len(segs), len(vias)))
        tracks = [t for t in board.GetTracks()]
        self.assertEqual(len(tracks), n_seg + n_via)
        t = next(t for t in tracks if t.GetClass() == "PCB_TRACK")
        self.assertEqual(t.GetNetname(), "SIG")
        self.assertAlmostEqual(pcbnew.ToMM(t.GetWidth()), 0.20, places=6)
        x = pcbnew.ToMM(t.GetStart().x)
        self.assertTrue(100.0 <= x <= 112.0, x)


if __name__ == "__main__":
    unittest.main(verbosity=2)

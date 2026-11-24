# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital

"""gridrouter.kicad_trace.dump_board on an in-memory pcbnew board.

Needs KiCad's Python (pcbnew); the board tests are skipped elsewhere:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tests/test_kicad_trace.py

The arc approximation (no pcbnew needed) is tested in any Python.  If the viewer's
trace inspector viewer/build/rvdump exists, the dumped file is also read with it.
"""
import math
import os
import subprocess
import tempfile
import unittest

from common import HERE

try:
    import pcbnew
except ImportError:                     # not KiCad's Python
    pcbnew = None

from gridrouter import tracefile as tf
from gridrouter.kicad_trace import _arc_points, board_writer, dump_board

RVDUMP = os.path.join(os.path.dirname(HERE), "viewer", "build", "rvdump")
MM = 1000000


def area(contour):
    s = 0
    for (x0, y0), (x1, y1) in zip(contour, contour[1:] + contour[:1]):
        s += x0 * y1 - x1 * y0
    return abs(s) / 2.0


def poly_area(poly):
    """Even-odd area of one polygon (outline minus its holes, which do not overlap)."""
    return area(poly[0]) - sum(area(h) for h in poly[1:])


def inside(contour, x, y):
    n, c = len(contour), False
    for k in range(n):
        (x0, y0), (x1, y1) = contour[k], contour[(k + 1) % n]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


class ArcPoints(unittest.TestCase):
    def test_arc(self):
        r, err = 2 * MM, 5000
        pts = _arc_points((120 * MM, 90 * MM), (122 * MM, 92 * MM), (124 * MM, 90 * MM), err)
        self.assertEqual(pts[0], (120 * MM, 90 * MM))
        self.assertEqual(pts[-1], (124 * MM, 90 * MM))
        self.assertGreater(len(pts), 10)
        for x, y in pts:
            self.assertAlmostEqual(math.hypot(x - 122 * MM, y - 90 * MM), r, delta=2)
            self.assertGreaterEqual(y, 90 * MM - 1)       # through the mid point (y > 90), not the other way
        for a, b in zip(pts, pts[1:]):                    # chord error within the limit
            mx, my = (a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0
            self.assertLessEqual(r - math.hypot(mx - 122 * MM, my - 90 * MM), err + 2)

    def test_arc_other_way_and_straight(self):
        pts = _arc_points((120 * MM, 90 * MM), (122 * MM, 88 * MM), (124 * MM, 90 * MM), 5000)
        self.assertTrue(all(y <= 90 * MM + 1 for _, y in pts))
        self.assertEqual(_arc_points((0, 0), (5, 5), (10, 10), 5000), [(0, 0), (10, 10)])


def V(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def make_board():
    """A small 2-layer board with one of each kind of thing the dump writes."""
    b = pcbnew.BOARD()
    nets = {}
    for name in ("GND", "SIG"):
        ni = pcbnew.NETINFO_ITEM(b, name)
        b.Add(ni)
        nets[name] = ni
    edge = pcbnew.PCB_SHAPE(b)
    edge.SetShape(pcbnew.SHAPE_T_RECT)
    edge.SetStart(V(100, 80))
    edge.SetEnd(V(130, 100))
    edge.SetLayer(pcbnew.Edge_Cuts)
    edge.SetWidth(pcbnew.FromMM(0.1))
    b.Add(edge)

    # J1: a plated through-hole pad and an unplated slot rotated by 90 degrees
    j1 = pcbnew.FOOTPRINT(b)
    j1.SetReference("J1")
    j1.SetValue("CONN")
    j1.SetFPID(pcbnew.LIB_ID("TestLib", "THT_Slot"))
    j1.SetPosition(V(105, 85))
    b.Add(j1)
    p = pcbnew.PAD(j1)
    p.SetNumber("1")
    p.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    p.SetLayerSet(pcbnew.PAD.PTHMask())
    p.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(V(1.7, 1.7))
    p.SetDrillShape(pcbnew.PAD_DRILL_SHAPE_CIRCLE)
    p.SetDrillSize(V(1.0, 1.0))
    p.SetPosition(V(105, 85))
    p.SetNet(nets["SIG"])
    j1.Add(p)
    s = pcbnew.PAD(j1)
    s.SetNumber("")
    s.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    s.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    s.SetShape(pcbnew.PAD_SHAPE_OVAL)
    s.SetSize(V(1.0, 2.0))
    s.SetDrillShape(pcbnew.PAD_DRILL_SHAPE_OBLONG)
    s.SetDrillSize(V(1.0, 2.0))
    s.SetPosition(V(105, 90))
    s.SetOrientationDegrees(90)
    j1.Add(s)
    line = pcbnew.PCB_SHAPE(j1, pcbnew.SHAPE_T_SEGMENT)
    line.SetStart(V(103, 83))
    line.SetEnd(V(107, 83))
    line.SetLayer(pcbnew.F_SilkS)
    line.SetWidth(pcbnew.FromMM(0.12))
    j1.Add(line)

    # R1: two SMD pads, the second one rotated by 30 degrees
    r1 = pcbnew.FOOTPRINT(b)
    r1.SetReference("R1")
    r1.SetValue("10k")
    r1.SetFPID(pcbnew.LIB_ID("TestLib", "SMD_Pair"))
    r1.SetPosition(V(116.5, 85))
    b.Add(r1)
    for num, x, rot, net in (("1", 115, 0, "SIG"), ("2", 118, 30, "GND")):
        q = pcbnew.PAD(r1)
        q.SetNumber(num)
        q.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
        q.SetLayerSet(pcbnew.PAD.SMDMask())
        q.SetShape(pcbnew.PAD_SHAPE_RECT)
        q.SetSize(V(1.0, 0.5))
        q.SetPosition(V(x, 85))
        q.SetOrientationDegrees(rot)
        q.SetNet(nets[net])
        r1.Add(q)

    text = pcbnew.PCB_TEXT(b)
    text.SetText("TEST")
    text.SetPosition(V(110, 95))
    text.SetLayer(pcbnew.F_SilkS)
    text.SetTextSize(V(1.0, 1.0))
    text.SetTextThickness(pcbnew.FromMM(0.15))
    b.Add(text)
    hidden = pcbnew.PCB_TEXT(b)
    hidden.SetText("HIDDEN")
    hidden.SetPosition(V(110, 90))
    hidden.SetLayer(pcbnew.F_SilkS)
    hidden.SetVisible(False)
    b.Add(hidden)

    z = pcbnew.ZONE(b)
    z.SetLayer(pcbnew.F_Cu)
    z.SetNet(nets["GND"])
    o = z.Outline()
    o.NewOutline()
    for x, y in ((101, 81), (129, 81), (129, 99), (101, 99)):
        o.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    z.SetLocalClearance(pcbnew.FromMM(0.3))
    z.SetMinThickness(pcbnew.FromMM(0.25))
    b.Add(z)
    keep = pcbnew.ZONE(b)
    keep.SetIsRuleArea(True)
    keep.SetLayer(pcbnew.B_Cu)
    keep.SetDoNotAllowTracks(True)
    o = keep.Outline()
    o.NewOutline()
    for x, y in ((125, 82), (128, 82), (128, 86), (125, 86)):
        o.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    b.Add(keep)

    t = pcbnew.PCB_TRACK(b)
    t.SetStart(V(115, 85))
    t.SetEnd(V(120, 90))
    t.SetLayer(pcbnew.F_Cu)
    t.SetWidth(pcbnew.FromMM(0.25))
    t.SetNet(nets["SIG"])
    b.Add(t)
    a = pcbnew.PCB_ARC(b)
    a.SetStart(V(120, 90))
    a.SetMid(V(122, 92))
    a.SetEnd(V(124, 90))
    a.SetLayer(pcbnew.B_Cu)
    a.SetWidth(pcbnew.FromMM(0.25))
    a.SetNet(nets["SIG"])
    b.Add(a)
    v = pcbnew.PCB_VIA(b)
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetPosition(V(120, 90))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetWidth(pcbnew.F_Cu, pcbnew.FromMM(0.6))
    v.SetDrill(pcbnew.FromMM(0.3))
    v.SetNet(nets["SIG"])
    b.Add(v)
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    return b


STACKUP = [("F.Cu", 0.035), ("core", 1.51, 4.5), ("B.Cu", 0.035)]


@unittest.skipIf(pcbnew is None, "pcbnew not available (run with KiCad's Python)")
class DumpBoard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.tmp.name
        cls.board = b = make_board()
        def save(name):
            pcbnew.SaveBoard(os.path.join(d, name), b)
            with open(os.path.join(d, name), "rb") as f:
                return f.read()
        cls.saved = [save("a.kicad_pcb"), save("b.kicad_pcb")]       # saving twice: same bytes
        from gridrouter.recorder import Recorder
        cls.path = os.path.join(d, "board.grtrace")
        rec = Recorder(cls.path)
        cls.stats = dump_board(rec, b, pcbnew, "final", origin=(100.0, 80.0), stackup=STACKUP)
        rec.close()
        cls.saved.append(save("c.kicad_pcb"))
        cls.trace = tf.read_trace(cls.path)
        cls.brd = [c.data for c in cls.trace.chunks if c.tag == "BRD "][0]
        cls.li = {name: k for k, (_, _, name) in enumerate(cls.brd.layers)}
        cls.code = {name: code for code, name in cls.brd.nets}
        cls.fp = {f[4]: k for k, f in enumerate(cls.brd.footprints)}     # reference -> index

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def recs(self, kind, **kw):
        return [r for r in self.brd.records if r.kind == kind
                and all(getattr(r, k) == v for k, v in kw.items())]

    def test_board_unchanged(self):
        a, b, c = self.saved
        self.assertEqual(a, b, "saving the board twice gives different bytes; the check is void")
        self.assertEqual(a, c, "the dump changed the saved board")

    def test_file(self):
        self.assertEqual(self.trace.errors, [])
        self.assertTrue(self.trace.complete)
        self.assertEqual(tf.check(self.trace), [])
        self.assertEqual([c.tag for c in self.trace.chunks], ["BRD ", "END "])
        self.assertEqual(self.stats["bytes"], self.trace.chunks[0].length)
        print("\n    dump: %d records, %d bytes, %.3f s; skipped %r; approximated %r" % (
            self.stats["records"], self.stats["bytes"], self.stats["seconds"],
            self.stats["skipped"], self.stats["approximated"]))

    def test_header_layers_nets_stackup(self):
        d = self.brd
        self.assertEqual(d.label, "final")
        self.assertEqual((d.origin_x_nm, d.origin_y_nm), (100 * MM, 80 * MM))
        self.assertAlmostEqual(d.thickness_mm,
                               self.board.GetDesignSettings().GetBoardThickness() / 1e6, places=9)
        names = [n for _, _, n in d.layers]
        for n in ("F.Cu", "B.Cu", "F.Mask", "F.Paste", "F.Silkscreen", "Edge.Cuts"):
            self.assertIn(n, names)
        for kid, typ, name in d.layers:
            self.assertEqual(name, self.board.GetStandardLayerName(kid))
            self.assertEqual(typ, self.board.GetLayerType(kid) if pcbnew.IsCopperLayer(kid) else -1)
        self.assertEqual(sorted(self.code), ["GND", "SIG"])
        for code, name in d.nets:
            self.assertEqual(self.board.FindNet(name).GetNetCode(), code)
        self.assertEqual([s[2] for s in d.stackup], ["F.Cu", "core", "B.Cu"])
        self.assertTrue(math.isnan(d.stackup[0][1]))
        self.assertEqual(d.stackup[1][:2], (1.51, 4.5))

    def test_footprints_and_pads(self):
        fps = self.brd.footprints               # in board order (BOARD.Add puts a footprint first)
        self.assertEqual([f[4] for f in fps], [f.GetReference() for f in self.board.GetFootprints()])
        self.assertEqual(sorted((f[4], f[5], f[6]) for f in fps),
                         [("J1", "CONN", "TestLib:THT_Slot"), ("R1", "10k", "TestLib:SMD_Pair")])
        j1, r1 = self.fp["J1"], self.fp["R1"]
        self.assertEqual(fps[j1][1:4], (105 * MM, 85 * MM, 0))
        self.assertEqual(fps[r1][1:4], (116500000, 85 * MM, 0))
        info = sorted(self.recs(tf.PADINFO), key=lambda r: (fps[r.fp][4], r.y, r.x))
        self.assertEqual([(r.fp, r.number, r.x, r.y) for r in info],
                         [(j1, "1", 105 * MM, 85 * MM), (j1, "", 105 * MM, 90 * MM),
                          (r1, "1", 115 * MM, 85 * MM), (r1, "2", 118 * MM, 85 * MM)])
        self.assertEqual(info[0].net, self.code["SIG"])
        self.assertEqual(info[1].net, -1)
        self.assertEqual(info[0].attrib, pcbnew.PAD_ATTRIB_PTH)
        self.assertEqual(info[1].attrib, pcbnew.PAD_ATTRIB_NPTH)
        # the unrotated SMD pad, to the nanometre
        pad = self.recs(tf.POLYSET, role=tf.ROLE_PAD, fp=r1, layer_index=self.li["F.Cu"])
        pad.sort(key=lambda r: min(x for x, _ in r.polys[0][0]))
        self.assertEqual(len(pad), 2)
        self.assertEqual(pad[0].net, self.code["SIG"])
        self.assertEqual(sorted(pad[0].polys[0][0]),
                         sorted([(114500000, 84750000), (115500000, 84750000),
                                 (115500000, 85250000), (114500000, 85250000)]))
        self.assertAlmostEqual(poly_area(pad[1].polys[0]), 0.5e12, delta=0.5e12 * 1e-6)
        xs = [x for x, _ in pad[1].polys[0][0]]
        self.assertGreater(max(xs) - min(xs), 1.0 * MM)            # rotated: wider than 1 mm
        # mask and paste shapes of the SMD pads
        self.assertEqual(len(self.recs(tf.POLYSET, role=tf.ROLE_PAD, fp=r1,
                                       layer_index=self.li["F.Mask"])), 2)
        self.assertEqual(len(self.recs(tf.POLYSET, role=tf.ROLE_PAD, fp=r1,
                                       layer_index=self.li["F.Paste"])), 2)
        # the through-hole pad on both copper layers, round within the error
        tht = self.recs(tf.POLYSET, role=tf.ROLE_PAD, fp=j1, net=self.code["SIG"])
        self.assertEqual(sorted(r.layer_index for r in tht),
                         sorted([self.li["F.Cu"], self.li["B.Cu"], self.li["F.Mask"],
                                 self.li["B.Mask"]]))
        a = poly_area(tht[0].polys[0])
        self.assertAlmostEqual(a, math.pi * 0.85e6 ** 2, delta=math.pi * 0.85e6 ** 2 * 0.01)

    def test_holes(self):
        holes = self.recs(tf.HOLE)
        self.assertEqual(len(holes), 3)
        j1 = self.fp["J1"]
        h = [r for r in holes if r.fp == j1 and r.plated][0]
        self.assertEqual((h.x, h.y, h.w, h.h, h.shape, h.net), (105 * MM, 85 * MM, MM, MM, 0,
                                                                 self.code["SIG"]))
        s = [r for r in holes if r.fp == j1 and not r.plated][0]
        self.assertEqual((s.x, s.y, s.w, s.h, s.shape, s.net), (105 * MM, 90 * MM, MM, 2 * MM, 1, -1))
        self.assertAlmostEqual(s.rot_deg, 90.0)
        v = [r for r in holes if r.fp == -1][0]
        self.assertEqual((v.x, v.y, v.w, v.h, v.plated), (120 * MM, 90 * MM, 300000, 300000, 1))

    def test_tracks_and_vias(self):
        tracks = self.recs(tf.TRACK)
        straight = [t for t in tracks if t.layer_index == self.li["F.Cu"]]
        self.assertEqual([(t.x0, t.y0, t.x1, t.y1, t.w, t.net) for t in straight],
                         [(115 * MM, 85 * MM, 120 * MM, 90 * MM, 250000, self.code["SIG"])])
        arc = [t for t in tracks if t.layer_index == self.li["B.Cu"]]
        self.assertGreater(len(arc), 10)
        self.assertEqual((arc[0].x0, arc[0].y0), (120 * MM, 90 * MM))
        self.assertEqual((arc[-1].x1, arc[-1].y1), (124 * MM, 90 * MM))
        for t0, t1 in zip(arc, arc[1:]):
            self.assertEqual((t0.x1, t0.y1), (t1.x0, t1.y0))
        for t in arc:
            self.assertAlmostEqual(math.hypot(t.x0 - 122 * MM, t.y0 - 90 * MM), 2 * MM, delta=2)
        self.assertEqual(self.stats["approximated"].get("arc track as straight tracks"), 1)
        vias = self.recs(tf.VIA)
        self.assertEqual([(v.x, v.y, v.dia, v.drill, v.net) for v in vias],
                         [(120 * MM, 90 * MM, 600000, 300000, self.code["SIG"])])
        self.assertEqual((vias[0].top_layer_index, vias[0].bottom_layer_index),
                         (self.li["F.Cu"], self.li["B.Cu"]))

    def test_zones(self):
        gnd = self.code["GND"]
        outline = self.recs(tf.POLYSET, role=tf.ROLE_ZONE_OUTLINE)
        self.assertEqual(len(outline), 1)
        self.assertEqual((outline[0].layer_index, outline[0].net), (self.li["F.Cu"], gnd))
        self.assertEqual(outline[0].polys, [[[(101 * MM, 81 * MM), (129 * MM, 81 * MM),
                                              (129 * MM, 99 * MM), (101 * MM, 99 * MM)]]])
        fill = self.recs(tf.POLYSET, role=tf.ROLE_ZONE_FILL)
        self.assertEqual(len(fill), 1)
        self.assertEqual((fill[0].layer_index, fill[0].net, fill[0].sub), (self.li["F.Cu"], gnd, 0))
        z = [z for z in self.board.Zones() if not z.GetIsRuleArea()][0]
        want = z.GetFilledPolysList(pcbnew.F_Cu).Area()
        got = sum(poly_area(p) for p in fill[0].polys)
        self.assertAlmostEqual(got, want, delta=want * 1e-6)
        self.assertLess(got, 28e6 * 18e6)
        # unfractured: the via of the other net is a hole of the fill polygon
        holes = [h for p in fill[0].polys for h in p[1:]]
        self.assertTrue(holes)
        self.assertTrue(any(inside(h, 120 * MM, 90 * MM) for h in holes))
        self.assertFalse(any(inside(p[0], 120 * MM, 90 * MM) and not any(inside(h, 120 * MM, 90 * MM)
                                                                           for h in p[1:])
                             for p in fill[0].polys))
        rule = self.recs(tf.POLYSET, role=tf.ROLE_RULE_AREA)
        self.assertEqual([(r.layer_index, r.net) for r in rule], [(self.li["B.Cu"], -1)])

    def test_outline_graphics_text(self):
        out = self.recs(tf.POLYSET, role=tf.ROLE_OUTLINE)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].layer_index, self.li["Edge.Cuts"])
        self.assertEqual(sorted(out[0].polys[0][0]),
                         sorted([(100 * MM, 80 * MM), (130 * MM, 80 * MM), (130 * MM, 100 * MM),
                                 (100 * MM, 100 * MM)]))
        g = self.recs(tf.POLYSET, role=tf.ROLE_GRAPHIC, fp=self.fp["J1"],
                      layer_index=self.li["F.Silkscreen"])
        self.assertEqual([(r.sub, r.polys) for r in g],
                         [(120000, [[[(103 * MM, 83 * MM), (107 * MM, 83 * MM)]]])])
        edge = self.recs(tf.POLYSET, role=tf.ROLE_GRAPHIC, layer_index=self.li["Edge.Cuts"])
        self.assertEqual(len(edge), 1)
        self.assertEqual(edge[0].sub, 100000)
        self.assertEqual(edge[0].polys[0][0][0], edge[0].polys[0][0][-1])    # closed: first repeated
        texts = self.recs(tf.TEXT)
        board_text = [t for t in texts if t.fp == -1]
        self.assertEqual([(t.text, t.x, t.y, t.layer_index) for t in board_text],
                         [("TEST", 110 * MM, 95 * MM, self.li["F.Silkscreen"])])     # not "HIDDEN"
        j1 = [f for f in self.board.GetFootprints() if f.GetReference() == "J1"][0]
        self.assertEqual(sorted(t.text for t in texts if t.fp == self.fp["J1"]),
                         sorted(f.GetShownText(True) for f in j1.GetFields() if f.IsVisible()))
        self.assertIn(("J1", self.fp["J1"]), [(t.text, t.fp) for t in texts])    # visible field
        glyphs = self.recs(tf.POLYSET, role=tf.ROLE_TEXT, fp=-1)
        self.assertEqual(len(glyphs), 1)
        pts = [xy for p in glyphs[0].polys for c in p for xy in c]
        self.assertGreaterEqual(len(glyphs[0].polys), 4)      # T, E, S, T
        xs, ys = [x for x, _ in pts], [y for _, y in pts]
        self.assertTrue(min(xs) < 110 * MM < max(xs) and min(ys) < 95 * MM < max(ys))
        self.assertLess(max(xs) - min(xs), 5 * MM)
        self.assertGreater(sum(poly_area(p) for p in glyphs[0].polys), 0)
        self.assertEqual(self.stats["skipped"], {})

    def test_writer_without_recorder(self):
        bw, st = board_writer(self.board, pcbnew, "final", origin=(100.0, 80.0), stackup=STACKUP)
        with open(self.path, "rb") as f:
            data = f.read()
        start = tf.HEADER.size + tf.CHUNK.size            # the BRD payload is the first chunk's
        self.assertEqual(bw.payload(), data[start:start + self.stats["bytes"]])
        self.assertEqual(st["records"], self.stats["records"])

    def test_kiid_generator_not_advanced(self):
        # gen_pcb.py seeds KiCad's UUID generator for a repeatable board: the dump
        # must not create a board item (each one would draw a UUID from it)
        pcbnew.KIID.SeedGenerator(20261122)
        want = pcbnew.KIID().AsString()
        pcbnew.KIID.SeedGenerator(20261122)
        board_writer(self.board, pcbnew, "x").__getitem__(0).payload()
        self.assertEqual(pcbnew.KIID().AsString(), want)

    def test_rvdump(self):
        if not os.path.exists(RVDUMP):
            print("\n    NOTE: %s not built; the C++ reader check was not run" % RVDUMP)
            return
        res = subprocess.run([RVDUMP, self.path], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        issues = [ln for ln in res.stdout.splitlines() if ln.startswith("issues:")]
        self.assertEqual(len(issues), 1, res.stdout)
        self.assertRegex(issues[0], r"issues:\s+0 errors, 0 warnings")
        print("\n    rvdump: " + issues[0])


@unittest.skipIf(pcbnew is None, "pcbnew not available (run with KiCad's Python)")
class Unfilled(unittest.TestCase):
    def test_unfilled_zone_and_skipped_items(self):
        """Before zone filling (the "placed" dump): zone outlines, no fills.  Items the
        format has no record for are counted in stats["skipped"]."""
        b = pcbnew.BOARD()
        ni = pcbnew.NETINFO_ITEM(b, "GND")
        b.Add(ni)
        z = pcbnew.ZONE(b)
        z.SetLayer(pcbnew.B_Cu)
        z.SetNet(ni)
        o = z.Outline()
        o.NewOutline()
        for x, y in ((0, 0), (10, 0), (10, 5), (0, 5)):
            o.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
        b.Add(z)
        dim = pcbnew.PCB_DIM_ALIGNED(b)
        dim.SetLayer(pcbnew.Dwgs_User)
        b.Add(dim)
        bw, st = board_writer(b, pcbnew, "placed")
        roles = [r.role for r in bw.records if r.kind == tf.POLYSET]
        self.assertEqual(roles.count(tf.ROLE_ZONE_OUTLINE), 1)
        self.assertEqual(roles.count(tf.ROLE_ZONE_FILL), 0)
        self.assertEqual(st["skipped"], {"graphic item PCB_DIM_ALIGNED": 1,     # no record kind
                                         "board outline (GetBoardPolygonOutlines failed)": 1})
        self.assertEqual(roles.count(tf.ROLE_OUTLINE), 0)               # no Edge.Cuts outline
        self.assertEqual(st["footprints"], 0)
        self.assertEqual(tf.parse("BRD ", bw.payload()).label, "placed")


if __name__ == "__main__":
    unittest.main(verbosity=2)

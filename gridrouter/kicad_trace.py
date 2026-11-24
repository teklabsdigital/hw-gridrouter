# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital

"""Dump a pcbnew BOARD into a trace file as one ``BRD `` chunk (docs/TRACE_FORMAT.md 7.18).

    from gridrouter.recorder import Recorder
    from gridrouter.kicad_trace import dump_board

    stats = dump_board(rec, board, pcbnew, "placed", origin=(OX, OY), stackup=STACKUP)

The pcbnew module is passed in by the caller, as for gridrouter.kicad; gridrouter
never imports it.  The dump is strictly read-only: it only calls getters and the
const Transform...ToPolygon / ToPolySet methods (into polygon sets of its own),
copies zone fills before unfracturing them, and creates no board item (so the
seeded KIID generator a caller may rely on is not advanced either).

What the format cannot express is skipped and counted in the returned stats
("skipped"), and shapes written approximately are counted in "approximated".

Standard library only (plus the pcbnew passed in), Python 3.9 compatible.
"""
import math
import time

from gridrouter import tracefile as tf

__all__ = ["dump_board", "board_writer"]


def _llround(x):
    """Round half away from zero (spec section 9)."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def _chain(lc, close=False):
    """Points of a SHAPE_LINE_CHAIN as [(x, y)] (arcs come approximated by their points).
    The first point is not repeated at the end unless close is set."""
    pts = [(p.x, p.y) for p in lc.CPoints()]
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts.pop()
    if close and pts:
        pts.append(pts[0])
    return pts


def _polys(ps, close=False):
    """A SHAPE_POLY_SET as [polygon], polygon = [outline, hole, ...]."""
    out = []
    for i in range(ps.OutlineCount()):
        poly = [_chain(ps.COutline(i), close)]
        for h in range(ps.HoleCount(i)):
            poly.append(_chain(ps.CHole(i, h), close))
        poly = [c for c in poly if c]
        if poly:
            out.append(poly)
    return out


def _arc_points(s, m, e, max_err):
    """Polyline through an arc given by start, mid and end points (nm tuples), with a
    chord error of at most max_err.  Collinear points give the straight line."""
    (ax, ay), (bx, by), (cx, cy) = s, m, e
    d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-9:
        return [s, e]
    a2, b2, c2 = ax * ax + ay * ay, bx * bx + by * by, cx * cx + cy * cy
    ox = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    oy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    r = math.hypot(ax - ox, ay - oy)
    a0 = math.atan2(ay - oy, ax - ox)
    tau = 2.0 * math.pi
    d1 = (math.atan2(cy - oy, cx - ox) - a0) % tau
    dm = (math.atan2(by - oy, bx - ox) - a0) % tau
    sweep = d1 if dm <= d1 else d1 - tau
    if s == e:
        sweep = tau if dm >= 0 else -tau
    return [s] + _circle_points(ox, oy, r, a0, sweep, max_err)[1:-1] + [e]


def _circle_points(ox, oy, r, a0, sweep, max_err):
    """Points from angle a0 over sweep (radians) on a circle, chord error <= max_err."""
    if r <= max_err:
        n = 4
    else:
        step = 2.0 * math.acos(max(-1.0, 1.0 - max_err / r))
        n = max(2, int(math.ceil(abs(sweep) / step)))
    return [(_llround(ox + r * math.cos(a0 + sweep * k / n)),
             _llround(oy + r * math.sin(a0 + sweep * k / n))) for k in range(n + 1)]


def _xy(v):
    return (v.x, v.y)


class _Dump:
    def __init__(self, board, pcbnew, label, origin, stackup, max_error):
        self.b, self.pn = board, pcbnew
        bds = board.GetDesignSettings()
        self.max_err = int(max_error if max_error is not None else bds.m_MaxError) or 5000
        ox, oy = origin if origin is not None else (0.0, 0.0)
        self.bw = tf.BoardWriter(label, bds.GetBoardThickness() / 1e6,
                                 (_llround(ox * 1e6), _llround(oy * 1e6)))
        self.skipped, self.approx, self.counts = {}, {}, {}
        self.lidx = {}                      # kicad layer id -> layer_index
        self.copper = set()
        for lid in board.GetEnabledLayers().Seq():
            lid = int(lid)
            cu = pcbnew.IsCopperLayer(lid)
            typ = int(board.GetLayerType(lid)) if cu else -1
            self.lidx[lid] = self.bw.layer(board.GetStandardLayerName(lid), lid, typ)
            if cu:
                self.copper.add(lid)
        for item in stackup or ():
            name, th = item[0], item[1]
            er = item[2] if len(item) > 2 else None
            self.bw.stackup_item(name, th, er)
        nets = board.GetNetInfo().NetsByNetcode()
        for code in sorted(int(c) for c in nets.keys()):
            if code > 0:
                self.bw.net(code, nets[code].GetNetname())
        self.paste = {int(pcbnew.F_Paste), int(pcbnew.B_Paste)}
        self.mask = {int(pcbnew.F_Mask), int(pcbnew.B_Mask)}

    # -- bookkeeping ------------------------------------------------------------
    def skip(self, what, n=1):
        self.skipped[what] = self.skipped.get(what, 0) + n

    def approximate(self, what, n=1):
        self.approx[what] = self.approx.get(what, 0) + n

    def count(self, what, n=1):
        self.counts[what] = self.counts.get(what, 0) + n

    def layer(self, lid, what):
        li = self.lidx.get(int(lid))
        if li is None:
            self.skip("%s on a layer that is not enabled" % what)
        return li

    @staticmethod
    def net(item):
        code = item.GetNetCode()
        return code if code > 0 else -1

    def polyset(self, li, ps, net=-1, fp=-1, role=tf.ROLE_GRAPHIC, close=False, sub=0):
        polys = _polys(ps, close)
        if polys:
            self.bw.polyset(li, polys, net=net, fp=fp, role=role, sub=sub)
            self.count("polyset role %d" % role)
        return len(polys)

    # -- board -------------------------------------------------------------------
    def run(self):
        pn, b = self.pn, self.b
        ps = pn.SHAPE_POLY_SET()
        edge = self.lidx.get(int(pn.Edge_Cuts))
        if edge is not None and b.GetBoardPolygonOutlines(ps, False):
            self.polyset(edge, ps, role=tf.ROLE_OUTLINE)
        elif edge is not None:
            self.skip("board outline (GetBoardPolygonOutlines failed)")
        for item in b.GetDrawings():
            self.item(item, -1)
        for k, fp in enumerate(b.GetFootprints()):
            self.footprint(k, fp)
        for z in b.Zones():
            self.zone(z, -1)
        for t in b.GetTracks():
            self.track(t)
        return self.bw

    def item(self, item, fp):
        cls = item.GetClass()
        if cls == "PCB_SHAPE":
            self.shape(item, fp)
        elif cls in ("PCB_TEXT", "PCB_FIELD", "PCB_TEXTBOX"):
            if item.IsVisible():            # hidden text is not drawn by KiCad either
                self.text(item, fp)
            if cls == "PCB_TEXTBOX" and item.IsBorderEnabled():
                self.skip("text box border")
        else:
            self.skip("graphic item %s" % cls)

    def footprint(self, k, fp):
        pos = fp.GetPosition()
        idx = self.bw.footprint(fp.GetReference(), fp.GetValue(), fp.GetFPIDAsString(), pos.x, pos.y,
                                fp.GetOrientationDegrees(), fp.IsFlipped())
        assert idx == k
        for f in fp.GetFields():
            if f.IsVisible():
                self.text(f, idx)
        for item in fp.GraphicalItems():
            self.item(item, idx)
        for pad in fp.Pads():
            self.pad(pad, idx)
        for z in fp.Zones():
            self.zone(z, idx)

    # -- graphics ------------------------------------------------------------------
    def shape(self, s, fp):
        pn = self.pn
        li = self.layer(s.GetLayer(), "graphic")
        if li is None:
            return
        kind = s.GetShape()
        filled = s.IsAnyFill()
        if s.IsHatchedFill():
            self.approximate("hatched fill written solid")
        if filled:
            buf = pn.SHAPE_POLY_SET()
            s.TransformShapeToPolygon(buf, s.GetLayer(), 0, self.max_err, pn.ERROR_INSIDE)
            if buf.OutlineCount() > 1:
                buf.Simplify()
            if not self.polyset(li, buf, fp=fp, role=tf.ROLE_GRAPHIC):
                self.skip("empty filled shape")
            return
        w = max(1, s.GetWidth())
        if kind == pn.SHAPE_T_SEGMENT:
            lines = [[_xy(s.GetStart()), _xy(s.GetEnd())]]
        elif kind == pn.SHAPE_T_RECT:
            c = [_xy(p) for p in s.GetRectCorners()]
            lines = [c + c[:1]]
            if s.GetCornerRadius() > 0:
                self.approximate("rounded rectangle written with square corners")
        elif kind == pn.SHAPE_T_CIRCLE:
            c = s.GetCenter()
            lines = [_circle_points(c.x, c.y, s.GetRadius(), 0.0, 2.0 * math.pi, self.max_err)]
            self.approximate("circle as polyline")
        elif kind == pn.SHAPE_T_ARC:
            lines = [_arc_points(_xy(s.GetStart()), _xy(s.GetArcMid()), _xy(s.GetEnd()), self.max_err)]
            self.approximate("arc as polyline")
        elif kind == pn.SHAPE_T_POLY:
            lines = [c for poly in _polys(s.GetPolyShape(), close=True) for c in poly]
        elif kind == pn.SHAPE_T_BEZIER:
            lines = [[_xy(p) for p in s.GetBezierPoints()]]
            self.approximate("bezier as polyline")
        else:
            self.skip("shape of type %d" % kind)
            return
        lines = [ln for ln in lines if len(ln) >= 2]
        if not lines:
            self.skip("empty shape")
            return
        self.bw.polyset(li, [[ln] for ln in lines], fp=fp, role=tf.ROLE_GRAPHIC, sub=w)
        self.count("polyset role %d" % tf.ROLE_GRAPHIC)

    def text(self, t, fp):
        pn = self.pn
        li = self.layer(t.GetLayer(), "text")
        if li is None:
            return
        buf = pn.SHAPE_POLY_SET()
        t.TransformTextToPolySet(buf, 0, self.max_err, pn.ERROR_INSIDE)
        if buf.OutlineCount() > 1:
            buf.Simplify()
        if not self.polyset(li, buf, fp=fp, role=tf.ROLE_TEXT):
            self.skip("text with no glyph outline")
        p = t.GetTextPos()
        self.bw.text(li, fp, p.x, p.y, t.GetTextAngleDegrees(), t.GetShownText(True))
        self.count("text")

    # -- pads ------------------------------------------------------------------------
    def pad(self, pad, fp):
        pn = self.pn
        net = self.net(pad)
        pos = pad.GetPosition()
        self.bw.padinfo(fp, net, pos.x, pos.y, int(pad.GetAttribute()), pad.GetNumber())
        self.count("pad")
        for lid in pad.GetLayerSet().Seq():
            lid = int(lid)
            li = self.lidx.get(lid)
            if li is None:
                continue                    # e.g. inner layers of "*.Cu" on a 2-layer board
            clear = 0
            if lid in self.copper:
                if not pad.FlashLayer(lid):
                    continue                # no copper on this layer (unused-layer removal, NPTH)
            elif lid in self.mask:
                clear = pad.GetSolderMaskExpansion(lid)
            elif lid in self.paste:
                m = pad.GetSolderPasteMargin(lid)
                clear = min(m.x, m.y)
                if m.x != m.y:
                    self.approximate("paste margin x != y: smaller one used")
            buf = pn.SHAPE_POLY_SET()
            pad.TransformShapeToPolygon(buf, lid, clear, self.max_err, pn.ERROR_INSIDE)
            if buf.OutlineCount() > 1:
                buf.Simplify()
            if not self.polyset(li, buf, net=net, fp=fp, role=tf.ROLE_PAD):
                self.skip("pad layer with an empty shape")
        if pad.HasHole():
            d = pad.GetDrillSize()
            oblong = pad.GetDrillShape() == pn.PAD_DRILL_SHAPE_OBLONG and d.x != d.y
            self.bw.hole(pos.x, pos.y, d.x, d.y, pad.GetOrientationDegrees() if oblong else 0.0,
                         plated=pad.GetAttribute() != pn.PAD_ATTRIB_NPTH, shape=1 if oblong else 0,
                         fp=fp, net=net)
            self.count("hole")

    # -- zones ---------------------------------------------------------------------
    def zone(self, z, fp):
        rule = z.GetIsRuleArea()
        net = -1 if rule else self.net(z)
        for lid in z.GetLayerSet().Seq():
            li = self.lidx.get(int(lid))
            if li is None:
                continue                    # e.g. AllCuMask() on a 4-layer board: nothing there
            if rule:
                self.polyset(li, z.Outline(), fp=fp, role=tf.ROLE_RULE_AREA)
                continue
            self.polyset(li, z.Outline(), net=net, fp=fp, role=tf.ROLE_ZONE_OUTLINE)
            if z.HasFilledPolysForLayer(lid):
                fill = self.pn.SHAPE_POLY_SET(z.GetFilledPolysList(lid))     # a copy
                if fill.OutlineCount():
                    fill.Unfracture()
                    self.polyset(li, fill, net=net, fp=fp, role=tf.ROLE_ZONE_FILL)

    # -- tracks and vias -------------------------------------------------------------
    def track(self, t):
        cls = t.GetClass()
        if cls == "PCB_VIA":
            p = t.GetPosition()
            top, bot = self.lidx.get(int(t.TopLayer())), self.lidx.get(int(t.BottomLayer()))
            if top is None or bot is None:
                self.skip("via on a layer that is not enabled")
                return
            net = self.net(t)
            drill = t.GetDrillValue()
            self.bw.via(p.x, p.y, t.GetWidth(t.TopLayer()), drill, net, top, bot)
            self.bw.hole(p.x, p.y, drill, plated=True, fp=-1, net=net)
            self.count("via")
            return
        li = self.layer(t.GetLayer(), "track")
        if li is None:
            return
        net, w = self.net(t), t.GetWidth()
        if cls == "PCB_ARC":
            pts = _arc_points(_xy(t.GetStart()), _xy(t.GetMid()), _xy(t.GetEnd()), self.max_err)
            for a, c in zip(pts, pts[1:]):
                self.bw.track(li, net, a[0], a[1], c[0], c[1], w)
            self.approximate("arc track as straight tracks")
            self.count("track (from arcs)", len(pts) - 1)
            return
        if cls != "PCB_TRACK":
            self.skip("track item %s" % cls)
            return
        s, e = t.GetStart(), t.GetEnd()
        self.bw.track(li, net, s.x, s.y, e.x, e.y, w)
        self.count("track")


def board_writer(board, pcbnew, label, origin=None, stackup=None, max_error=None):
    """(tracefile.BoardWriter, stats) for board, without writing anything.  See dump_board."""
    t0 = time.perf_counter()
    d = _Dump(board, pcbnew, label, origin, stackup, max_error)
    bw = d.run()
    stats = dict(label=label, seconds=time.perf_counter() - t0, layers=len(bw.layers),
                 nets=len(bw.nets), footprints=len(bw.footprints), records=len(bw.records),
                 counts=dict(d.counts), skipped=dict(d.skipped), approximated=dict(d.approx))
    return bw, stats


def dump_board(recorder, board, pcbnew, label, origin=None, stackup=None, max_error=None):
    """Write one BRD chunk of a pcbnew BOARD through recorder (a gridrouter.recorder.Recorder).

    label: "placed", "final" or any text; origin: board position of router cell (0, 0)
    in mm, as given to Recorder.attach (copied into the chunk as nm); stackup: [(name,
    thickness_mm[, er])] top to bottom (pcbnew does not expose the stackup to Python);
    max_error: chord error in nm for arcs, circles, text and pad shapes (default: the
    board's own maximum error setting).

    Read-only on the board.  Returns a dict of statistics: counts of what was written,
    "skipped" (board content the format cannot express, by kind), "approximated",
    "seconds" (time to build the chunk) and "bytes" (payload size)."""
    bw, stats = board_writer(board, pcbnew, label, origin, stackup, max_error)
    payload = bw.payload()
    recorder.board(payload)
    stats["bytes"] = len(payload)
    return stats

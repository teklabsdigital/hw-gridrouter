"""gridrouter trace files (.grtrace), format version 1.0.

The format is specified in docs/TRACE_FORMAT.md; this module follows it and
nothing else.  It holds:

  * payload packers, one per chunk tag (``pack(tag, fields)``), and
    ``TraceWriter``, the low-level chunk writer (one write and one flush per
    chunk, so a reader can follow a growing file);
  * ``BoardWriter``, which builds a ``BRD `` payload from plain values (no
    pcbnew; gridrouter/kicad_trace.py or a synthetic board feeds it);
  * ``TraceReader`` / ``read_trace``, which parse every 1.0 chunk into plain
    objects (``types.SimpleNamespace`` with the field names of the spec),
    skip unknown tags, ignore trailing payload bytes, report corrupt chunks
    and stop quietly at a chunk that is not completely written yet;
  * ``replay`` (copper state, spec section 8) and ``check`` (id sequences,
    order guarantees and bounds, sections 3.4, 4.1, 6 and 10; errors and
    warnings at the levels of section 10);
  * a command line:

        python3 -m gridrouter.tracefile summary FILE
        python3 -m gridrouter.tracefile dump FILE
        python3 -m gridrouter.tracefile check FILE     # exit 1 on errors; warnings only: 0

Standard library only, Python 3.9 compatible.
"""
import array
import math
import os
import struct
import sys
import time
from types import SimpleNamespace

__all__ = ["MAGIC", "MAJOR", "MINOR", "NONE32", "BLOCK", "TAGS", "TraceWriter", "BoardWriter",
           "TraceReader", "read_trace", "pack", "parse", "replay", "check", "summary",
           "CorruptChunk"]

MAGIC = b"GRTRACE\0"
MAJOR, MINOR = 1, 0
NONE32 = 0xFFFFFFFF
BLOCK = 1 << 30

HEADER = struct.Struct("<8sHHId")           # 24 bytes
CHUNK = struct.Struct("<4sId")              # 16 bytes

TAGS = ("RUN ", "NETS", "PAD ", "RECT", "DISC", "VIA ", "SNAP", "GRDS", "RNB ", "RNE ",
        "SRCH", "CMIT", "RBLD", "RIP ", "DROP", "MARK", "ROUT", "BRD ", "END ")

# SRCH flags (7.11)
S_WIDE, S_HAS_GOAL, S_HAS_MAX, S_VIA_GOAL, S_RELAX, S_FOUND, S_NEW_VIA, S_HAS_COSTS, \
    S_ERROR, S_HAS_TRACE = (1 << k for k in range(10))
# RUN flags (7.1)
R_STRICT, R_SKIP_JOINED, R_EDGE_MARGIN = 1, 2, 4
# CMIT flags (7.12)
C_WIDE, C_END_VIA, C_SKIP_JOINED = 1, 2, 4

# BRD record kinds (7.18)
POLYSET, HOLE, PADINFO, TRACK, VIA, TEXT = 1, 2, 3, 4, 5, 6
ROLE_PAD, ROLE_GRAPHIC, ROLE_TEXT, ROLE_ZONE_FILL, ROLE_ZONE_OUTLINE, ROLE_OUTLINE, \
    ROLE_RULE_AREA = range(7)

_BIG = sys.byteorder == "big"

_RUN = struct.Struct("<17iI12d")
_PAD = struct.Struct("<iI10d")
_RECT = struct.Struct("<IiII6d")
_DISC = struct.Struct("<6i")
_VIA = struct.Struct("<IiiiiIII")
_RNB = struct.Struct("<IiIiII")
_RNB_PAD = struct.Struct("<ddII")
_RNE = struct.Struct("<IiiIII")
_SRCH = struct.Struct("<IIIiIiiiqqqdIIIIIIII")
_CMIT = struct.Struct("<IiIiIIIIIId")
_STUB = struct.Struct("<II5d")
_IJ = struct.Struct("<ii")
_RIP = struct.Struct("<iIII")
_DROP = struct.Struct("<iIq")
_ROUT_SEG = struct.Struct("<Ii5d")
_ROUT_VIA = struct.Struct("<ddiI")
_END = struct.Struct("<IIIiqqd")
_BRD = struct.Struct("<dii")
_FP = struct.Struct("<diiII")
_POLY = struct.Struct("<iiiIII")
_HOLE = struct.Struct("<iiiidIIii")
_PADINFO = struct.Struct("<iiiiI")
_7I = struct.Struct("<7i")
_TEXT = struct.Struct("<iiiid")

assert (_RUN.size, _PAD.size, _RECT.size, _DISC.size, _VIA.size, _SRCH.size, _CMIT.size,
        _STUB.size, _END.size, _HOLE.size, _FP.size) == (168, 88, 64, 24, 32, 96, 48, 48, 40,
                                                         40, 24)


class CorruptChunk(ValueError):
    """A complete chunk whose payload is too short for its fields (spec 3.3)."""


# -- scalar / array helpers -----------------------------------------------------
def _u32(v):
    return int(v) & 0xFFFFFFFF


def pack_str(s):
    """`str`: u32 byte count, UTF-8 bytes, zero padding to a multiple of 4."""
    if isinstance(s, (bytes, bytearray)):
        b = bytes(s)
    else:
        b = str(s).encode("utf-8", errors="replace")
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)


def le_array(typecode, values):
    """Little-endian bytes of values as a 4-byte (i / I) or 1-byte (B) array.

    values may be bytes (taken as they are), an array.array, a ctypes array or
    any iterable of ints.  An "i" array may be written where the spec says
    u32 (cells): the bytes are the same for every value >= 0."""
    if isinstance(values, (bytes, bytearray)):
        return bytes(values)
    if isinstance(values, array.array) and values.itemsize == array.array(typecode).itemsize \
            and values.typecode in ("i", "I", "l", "L", "b", "B"):
        a = values
    else:
        try:
            mv = memoryview(values)         # ctypes arrays: copy the native bytes
        except TypeError:
            mv = None
        if mv is not None and mv.itemsize == array.array(typecode).itemsize:
            a = array.array(typecode)
            a.frombytes(mv.cast("B"))
        else:
            a = array.array(typecode, values)
    if _BIG and a.itemsize > 1:
        a = array.array(a.typecode, a)
        a.byteswap()
    return a.tobytes()


def _from_le(typecode, b):
    a = array.array(typecode)
    a.frombytes(b)
    if _BIG and a.itemsize > 1:
        a.byteswap()
    return a


def _layer_mask(layers):
    m = 0
    for layer in layers:
        m |= 1 << int(layer)
    return m & 0xFFFFFFFF


def _pad8(b):
    return b + b"\0" * (-len(b) % 8)


def _align8(b):
    """bytearray b grows by 4 zero bytes if its length is not a multiple of 8."""
    if len(b) & 7:
        b += b"\0" * 4


def _g(obj, name, default=None):
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


# -- payload packers ------------------------------------------------------------
def pack(tag, f):
    """Payload bytes of chunk `tag` from the fields in f (dict or namespace) with
    the field names of the spec; counts (nd, npads, nsrc, ...) are derived from
    the lists.  The inverse of parse()."""
    fn = _PACKERS.get(tag)
    if fn is None:
        raise ValueError("no packer for tag %r" % tag)
    return fn(f)


def _pack_run(f):
    dirs = [tuple(d) for d in _g(f, "dirs")]
    nd = len(dirs)
    rt, rw, rv = _g(f, "r_track"), _g(f, "r_wide"), _g(f, "r_via")
    pc, em = _g(f, "pad_clear"), _g(f, "edge_margin")
    head = _RUN.pack(
        _g(f, "nx"), _g(f, "ny"), nd, _g(f, "block", BLOCK), _g(f, "via_cost"), _g(f, "rip_cost"),
        _g(f, "max_rips"), _g(f, "h_max"), _g(f, "h_min"), _g(f, "h_off"),
        rt[0], rt[1], rw[0], rw[1], rv[0], rv[1], _g(f, "r_via_space"), _u32(_g(f, "flags", 0)),
        _g(f, "cell"), _g(f, "origin_x", 0.0), _g(f, "origin_y", 0.0), _g(f, "track"),
        _g(f, "track_wide"), pc[0], pc[1], _g(f, "pad_novia"), em[0], em[1],
        _g(f, "via_dia", 0.60), _g(f, "via_drill", 0.30))
    step = _g(f, "step")
    turn = _g(f, "turn")
    flat_dirs = []
    for d in dirs:
        flat_dirs += [int(d[0]), int(d[1]), int(d[2]) if len(d) > 2 else 0]
    flat_step = [int(v) for v in step[0]] + [int(v) for v in step[1]]
    flat_turn = [-1 if t is None else int(t) for row in turn for t in row]
    if len(flat_step) != 2 * nd or len(flat_turn) != (nd + 1) * nd:
        raise ValueError("RUN: step / turn do not match dirs")
    names = _g(f, "layer_name", ("F.Cu", "B.Cu"))
    return (head + le_array("i", flat_dirs) + le_array("i", flat_step) + le_array("i", flat_turn)
            + pack_str(_g(f, "producer", "")) + pack_str(names[0]) + pack_str(names[1]))


def _pack_nets(f):
    entries = list(_g(f, "entries"))
    out = bytearray(struct.pack("<I", len(entries)))
    for code, name in entries:
        out += struct.pack("<i", code) + pack_str(name)
    return bytes(out)


def _pack_pad(f):
    return _PAD.pack(*[_g(f, k) for k in ("net", "layers", "power", "cx", "cy", "x0", "y0", "x1",
                                          "y1", "d_t", "d_w", "d_novia")])


def _pack_rect(f):
    return _RECT.pack(*[_g(f, k) for k in ("op", "net", "layers", "levels", "x0", "y0", "x1", "y1",
                                           "d_t", "d_w")])


def _pack_disc(f):
    return _DISC.pack(*[_g(f, k) for k in ("net", "layer", "i", "j", "r_t", "r_w")])


def _pack_via(f):
    return _VIA.pack(_u32(_g(f, "copper_id")), _g(f, "net"), _g(f, "i"), _g(f, "j"), _g(f, "power"),
                     _g(f, "added"), _g(f, "ctx"), _u32(_g(f, "route_id")))


def _pack_snap(f):
    return struct.pack("<I", 0)


def _pack_grds(f):
    nc = _g(f, "nc")
    parts = [struct.pack("<II", _g(f, "which"), nc), pack_str(_g(f, "label", ""))]
    for k in ("T0", "T1", "W0", "W1"):
        b = le_array("i", _g(f, k))
        if len(b) != 4 * nc:
            raise ValueError("GRDS: %s has %d bytes, expected %d" % (k, len(b), 4 * nc))
        parts.append(b)
    nv = le_array("B", _g(f, "novia"))
    if len(nv) != nc:
        raise ValueError("GRDS: novia has %d bytes, expected %d" % (len(nv), nc))
    parts.append(nv)
    return b"".join(parts)


def _pack_rnb(f):
    pads = list(_g(f, "pads"))
    out = bytearray(_RNB.pack(_u32(_g(f, "route_id")), _g(f, "net"), _g(f, "wide"), _g(f, "power"),
                              len(pads), 0))
    for cx, cy, layers, ncells in pads:
        out += _RNB_PAD.pack(cx, cy, layers, _u32(ncells))
    out += pack_str(_g(f, "name"))
    labels = list(_g(f, "labels"))
    if len(labels) != len(pads):
        raise ValueError("RNB: one label per pad")
    for s in labels:
        out += pack_str(s)
    return bytes(out)


def _pack_rne(f):
    ripped, failed = list(_g(f, "ripped")), list(_g(f, "failed"))
    out = bytearray(_RNE.pack(_u32(_g(f, "route_id")), _g(f, "net"), _g(f, "rips_after"),
                              len(ripped), len(failed), _g(f, "status")))
    for s in ripped + failed:
        out += pack_str(s)
    return bytes(out)


def _pack_srch(f):
    src = le_array("I", _g(f, "src", b""))
    tgt = le_array("I", _g(f, "tgt", b""))
    path = le_array("I", _g(f, "path", b""))
    enc = _g(f, "enc", 0)
    flags = _u32(_g(f, "flags", 0))
    if enc == 0:
        tc = le_array("I", _g(f, "trace_cell", b""))
        td = le_array("i", _g(f, "trace_d", b"")) if flags & S_HAS_COSTS else b""
        ntrace = len(tc) // 4
        if td and len(td) != len(tc):
            raise ValueError("SRCH: trace_d and trace_cell differ in length")
        tail = tc + td
    else:
        ntrace = _g(f, "ntrace", 0)
        tail = bytes(_g(f, "trace_raw", b""))
    head = _SRCH.pack(
        _u32(_g(f, "search_id")), _u32(_g(f, "route_id", NONE32)), _g(f, "ctx", 0), _g(f, "net"),
        flags, _g(f, "gi", 0), _g(f, "gj", 0), _g(f, "rips_at_call", 0), _g(f, "max_cost", -1),
        _g(f, "cost", -1), _g(f, "expanded", -1), _g(f, "duration_s", 0.0),
        len(src) // 4, len(tgt) // 4, len(path) // 4, ntrace, _u32(_g(f, "ntrace_total", ntrace)),
        _u32(_g(f, "repeat_of", NONE32)), enc, 0)
    return b"".join((head, src, tgt, path, tail))


def _pack_cmit(f):
    stubs, vias = list(_g(f, "stubs")), list(_g(f, "vias"))
    path = le_array("I", _g(f, "path"))
    out = bytearray(_CMIT.pack(_u32(_g(f, "copper_id")), _g(f, "net"), _u32(_g(f, "flags")),
                               _g(f, "power"), _g(f, "ctx"), _u32(_g(f, "route_id")),
                               len(path) // 4, len(stubs), len(vias), 0, _g(f, "width_mm")))
    for layer, x0, y0, x1, y1, w in stubs:
        out += _STUB.pack(layer, 0, x0, y0, x1, y1, w)
    for i, j in vias:
        out += _IJ.pack(i, j)
    out += path
    return bytes(out)


def _pack_rbld(f):
    nets, names = list(_g(f, "nets")), list(_g(f, "names"))
    if len(nets) != len(names):
        raise ValueError("RBLD: one net per name")
    out = bytearray(struct.pack("<II", _u32(_g(f, "route_id")), len(names)))
    out += le_array("i", nets)
    for s in names:
        out += pack_str(s)
    return bytes(out)


def _pack_rip(f):
    removed = list(_g(f, "removed"))
    return (_RIP.pack(_g(f, "net"), _g(f, "ctx"), _u32(_g(f, "route_id")), len(removed))
            + le_array("I", removed))


def _pack_drop(f):
    return (_DROP.pack(_g(f, "net"), _g(f, "ok"), _g(f, "max_cost")) + pack_str(_g(f, "name"))
            + pack_str(_g(f, "pad_label")))


def _pack_mark(f):
    return struct.pack("<I", _g(f, "level")) + pack_str(_g(f, "text"))


def _pack_rout(f):
    segs, vias = list(_g(f, "segs")), list(_g(f, "vias"))
    out = bytearray(struct.pack("<II", len(segs), len(vias)))
    for layer, net, x0, y0, x1, y1, w in segs:
        out += _ROUT_SEG.pack(layer, net, x0, y0, x1, y1, w)
    for x, y, net in vias:
        out += _ROUT_VIA.pack(x, y, net, 0)
    return bytes(out)


def _pack_end(f):
    failed = list(_g(f, "failed"))
    out = bytearray(_END.pack(_g(f, "searches"), _g(f, "routes"), len(failed), _g(f, "rips"),
                              _g(f, "expanded_total"), _g(f, "trace_entries"), _g(f, "wall_s")))
    for name, label in failed:
        out += pack_str(name) + pack_str(label)
    return bytes(out)


def _pack_brd(f):
    if isinstance(f, BoardWriter):
        return f.payload()
    return BoardWriter.from_fields(f).payload()


_PACKERS = {"RUN ": _pack_run, "NETS": _pack_nets, "PAD ": _pack_pad, "RECT": _pack_rect,
            "DISC": _pack_disc, "VIA ": _pack_via, "SNAP": _pack_snap, "GRDS": _pack_grds,
            "RNB ": _pack_rnb, "RNE ": _pack_rne, "SRCH": _pack_srch, "CMIT": _pack_cmit,
            "RBLD": _pack_rbld, "RIP ": _pack_rip, "DROP": _pack_drop, "MARK": _pack_mark,
            "ROUT": _pack_rout, "BRD ": _pack_brd, "END ": _pack_end}


# -- writer ---------------------------------------------------------------------
class TraceWriter:
    """File header plus chunks.  Each chunk is built in memory (header, payload,
    padding) and written with one write() and one flush() (spec 3.3)."""

    def __init__(self, path, open_time=None):
        self.path = path
        self.f = open(path, "wb")
        self.open_time = time.time() if open_time is None else float(open_time)
        self._t0 = time.perf_counter()
        self._last_t = 0.0
        self.f.write(HEADER.pack(MAGIC, MAJOR, MINOR, 0, self.open_time))
        self.f.flush()
        self.offset = HEADER.size           # file offset of the next chunk

    def now(self):
        return time.perf_counter() - self._t0

    def chunk(self, tag, payload, t=None):
        """Write one chunk; returns its file offset.  t defaults to now and is
        never smaller than the previous chunk's t."""
        if isinstance(tag, str):
            tag = tag.encode("ascii")
        if len(tag) != 4 or any(b < 0x20 or b > 0x7E for b in tag):
            raise ValueError("bad chunk tag %r" % tag)
        n = len(payload)
        if n > 0xFFFFFFFF:
            raise ValueError("chunk payload too large (%d bytes)" % n)
        t = self.now() if t is None else float(t)
        t = max(t, self._last_t)
        self._last_t = t
        data = b"".join((CHUNK.pack(tag, n, t), payload, b"\0" * (-n % 8)))
        off = self.offset
        self.f.write(data)
        self.f.flush()
        self.offset += len(data)
        return off

    def close(self):
        if self.f is not None:
            self.f.close()
            self.f = None


# -- BRD ----------------------------------------------------------------------
class BoardWriter:
    """Builds a ``BRD `` payload (spec 7.18).  All coordinates are board nm
    (ints), angles degrees.  Layers are referred to by the index returned by
    layer(), footprints by the index returned by footprint().

        bw = BoardWriter("placed", thickness_mm=1.6, origin_nm=(ox, oy))
        f_cu = bw.layer("F.Cu", 0, 0)
        bw.net(1, "GND")
        fp = bw.footprint("U1", "MCU", "Lib:QFN", x, y)
        bw.polyset(f_cu, [[[(x0, y0), (x1, y0), (x1, y1), (x0, y1)]]], net=1, fp=fp, role=0)
        rec.board(bw)       # or tracefile.pack("BRD ", bw)
    """

    def __init__(self, label, thickness_mm=1.6, origin_nm=(0, 0)):
        self.label = label
        self.thickness_mm = float(thickness_mm)
        self.origin_x_nm, self.origin_y_nm = int(origin_nm[0]), int(origin_nm[1])
        self.layers = []            # (kicad_id, type, name)
        self.stackup = []           # (thickness_mm, er or NaN, name)
        self.nets = []              # (code, name)
        self.footprints = []        # (rot_deg, x, y, flipped, ref, value, fpid)
        self.records = []           # SimpleNamespace(kind=..., fields of the kind)

    @classmethod
    def from_fields(cls, f):
        bw = cls(_g(f, "label"), _g(f, "thickness_mm"), (_g(f, "origin_x_nm"), _g(f, "origin_y_nm")))
        bw.layers = [tuple(x) for x in _g(f, "layers")]
        bw.stackup = [tuple(x) for x in _g(f, "stackup")]
        bw.nets = [tuple(x) for x in _g(f, "nets")]
        bw.footprints = [tuple(x) for x in _g(f, "footprints")]
        bw.records = list(_g(f, "records"))
        return bw

    def layer(self, name, kicad_id=-1, type=-1):
        self.layers.append((int(kicad_id), int(type), name))
        return len(self.layers) - 1

    def layer_index(self, name):
        for k, (_, _, n) in enumerate(self.layers):
            if n == name:
                return k
        raise KeyError(name)

    def stackup_item(self, name, thickness_mm, er=None):
        self.stackup.append((float(thickness_mm), float("nan") if er is None else float(er), name))

    def net(self, code, name):
        self.nets.append((int(code), name))

    def footprint(self, ref, value, fpid, x, y, rot_deg=0.0, flipped=False):
        self.footprints.append((float(rot_deg), int(x), int(y), 1 if flipped else 0, ref, value, fpid))
        return len(self.footprints) - 1

    def polyset(self, layer_index, polys, net=-1, fp=-1, role=ROLE_GRAPHIC, sub=0):
        """polys: [polygon], polygon = [contour], contour = [(x, y)] (first contour the
        outline, the rest holes).  sub = 0 filled (even-odd), > 0 stroked with that width."""
        polys = [[[(int(x), int(y)) for x, y in c] for c in poly] for poly in polys]
        self.records.append(SimpleNamespace(kind=POLYSET, layer_index=int(layer_index), net=int(net),
                                            fp=int(fp), role=int(role), sub=int(sub), polys=polys))

    def hole(self, x, y, w, h=None, rot_deg=0.0, plated=True, shape=0, fp=-1, net=-1):
        self.records.append(SimpleNamespace(kind=HOLE, x=int(x), y=int(y), w=int(w),
                                            h=int(w if h is None else h), rot_deg=float(rot_deg),
                                            plated=1 if plated else 0, shape=int(shape), fp=int(fp),
                                            net=int(net)))

    def padinfo(self, fp, net, x, y, attrib, number):
        self.records.append(SimpleNamespace(kind=PADINFO, fp=int(fp), net=int(net), x=int(x),
                                            y=int(y), attrib=int(attrib), number=number))

    def track(self, layer_index, net, x0, y0, x1, y1, w):
        self.records.append(SimpleNamespace(kind=TRACK, layer_index=int(layer_index), net=int(net),
                                            x0=int(x0), y0=int(y0), x1=int(x1), y1=int(y1), w=int(w)))

    def via(self, x, y, dia, drill, net, top_layer_index, bottom_layer_index):
        self.records.append(SimpleNamespace(kind=VIA, x=int(x), y=int(y), dia=int(dia),
                                            drill=int(drill), net=int(net),
                                            top_layer_index=int(top_layer_index),
                                            bottom_layer_index=int(bottom_layer_index)))

    def text(self, layer_index, fp, x, y, angle_deg, text):
        self.records.append(SimpleNamespace(kind=TEXT, layer_index=int(layer_index), fp=int(fp),
                                            x=int(x), y=int(y), angle_deg=float(angle_deg), text=text))

    def raw_record(self, kind, body):
        """A record of a kind this version does not know (for tests of skipping).  The body
        is stored zero-padded to a multiple of 8, as a reader gives it back."""
        self.records.append(SimpleNamespace(kind=int(kind), raw=_pad8(bytes(body))))

    @staticmethod
    def _body(r):
        k = r.kind
        if k == POLYSET:
            b = bytearray(_POLY.pack(r.layer_index, r.net, r.fp, r.role, r.sub, len(r.polys)))
            for poly in r.polys:
                b += struct.pack("<I", len(poly))
                for c in poly:
                    b += struct.pack("<I", len(c))
                    b += le_array("i", [v for xy in c for v in xy])
            return bytes(b)
        if k == HOLE:
            return _HOLE.pack(r.x, r.y, r.w, r.h, r.rot_deg, r.plated, r.shape, r.fp, r.net)
        if k == PADINFO:
            return _PADINFO.pack(r.fp, r.net, r.x, r.y, r.attrib) + pack_str(r.number)
        if k == TRACK:
            return _7I.pack(r.layer_index, r.net, r.x0, r.y0, r.x1, r.y1, r.w)
        if k == VIA:
            return _7I.pack(r.x, r.y, r.dia, r.drill, r.net, r.top_layer_index, r.bottom_layer_index)
        if k == TEXT:
            return _TEXT.pack(r.layer_index, r.fp, r.x, r.y, r.angle_deg) + pack_str(r.text)
        return r.raw

    def payload(self):
        b = bytearray(_BRD.pack(self.thickness_mm, self.origin_x_nm, self.origin_y_nm))
        b += pack_str(self.label)
        b += struct.pack("<I", len(self.layers))
        for kid, typ, name in self.layers:
            b += struct.pack("<ii", kid, typ) + pack_str(name)
        b += struct.pack("<I", len(self.stackup))
        for th, er, name in self.stackup:
            _align8(b)
            b += struct.pack("<dd", th, er) + pack_str(name)
        b += struct.pack("<I", len(self.nets))
        for code, name in self.nets:
            b += struct.pack("<i", code) + pack_str(name)
        b += struct.pack("<I", len(self.footprints))
        for rot, x, y, flipped, ref, value, fpid in self.footprints:
            _align8(b)
            b += _FP.pack(rot, x, y, flipped, 0) + pack_str(ref) + pack_str(value) + pack_str(fpid)
        b += struct.pack("<I", len(self.records))
        _align8(b)
        for r in self.records:
            body = _pad8(self._body(r))
            b += struct.pack("<II", r.kind, len(body)) + body
        return bytes(b)


# -- reader ---------------------------------------------------------------------
class _Cur:
    """Bounds-checked little-endian cursor over one payload."""
    __slots__ = ("b", "o", "n")

    def __init__(self, b):
        self.b, self.o, self.n = b, 0, len(b)

    def need(self, k):
        if k < 0 or self.o + k > self.n:
            raise CorruptChunk("payload too short: need %d bytes at offset %d, length %d"
                               % (k, self.o, self.n))

    def st(self, s):
        self.need(s.size)
        v = s.unpack_from(self.b, self.o)
        self.o += s.size
        return v

    def u32(self):
        self.need(4)
        v = struct.unpack_from("<I", self.b, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        self.need(4)
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def raw(self, k):
        self.need(k)
        v = bytes(self.b[self.o:self.o + k])
        self.o += k
        return v

    def arr(self, typecode, count):
        size = array.array(typecode).itemsize
        return _from_le(typecode, self.raw(count * size))

    def str(self):
        n = self.u32()
        b = self.raw((n + 3) & ~3)
        return b[:n].decode("utf-8", errors="replace")

    def align8(self):
        if self.o & 7:
            self.o += 4


def _parse_run(c, ctx):
    v = c.st(_RUN)
    f = SimpleNamespace(
        nx=v[0], ny=v[1], nd=v[2], block=v[3], via_cost=v[4], rip_cost=v[5], max_rips=v[6],
        h_max=v[7], h_min=v[8], h_off=v[9], r_track=[v[10], v[11]], r_wide=[v[12], v[13]],
        r_via=[v[14], v[15]], r_via_space=v[16], flags=v[17], cell=v[18], origin_x=v[19],
        origin_y=v[20], track=v[21], track_wide=v[22], pad_clear=[v[23], v[24]], pad_novia=v[25],
        edge_margin=[v[26], v[27]], via_dia=v[28], via_drill=v[29])
    nd = f.nd
    if nd < 0 or nd > 4096:
        raise CorruptChunk("RUN: nd = %d" % nd)
    d = c.arr("i", 3 * nd)
    f.dirs = [(d[3 * k], d[3 * k + 1], d[3 * k + 2]) for k in range(nd)]
    s = c.arr("i", 2 * nd)
    f.step = [list(s[:nd]), list(s[nd:])]
    t = c.arr("i", (nd + 1) * nd)
    f.turn = [list(t[k * nd:(k + 1) * nd]) for k in range(nd + 1)]
    f.producer = c.str()
    f.layer_name = [c.str(), c.str()]
    return f


def _parse_nets(c, ctx):
    n = c.u32()
    entries = []
    for _ in range(n):
        code = c.i32()
        entries.append((code, c.str()))
    return SimpleNamespace(entries=entries)


def _parse_pad(c, ctx):
    v = c.st(_PAD)
    return SimpleNamespace(**dict(zip(("net", "layers", "power", "cx", "cy", "x0", "y0", "x1", "y1",
                                       "d_t", "d_w", "d_novia"), v)))


def _parse_rect(c, ctx):
    v = c.st(_RECT)
    return SimpleNamespace(**dict(zip(("op", "net", "layers", "levels", "x0", "y0", "x1", "y1",
                                       "d_t", "d_w"), v)))


def _parse_disc(c, ctx):
    v = c.st(_DISC)
    return SimpleNamespace(**dict(zip(("net", "layer", "i", "j", "r_t", "r_w"), v)))


def _parse_via(c, ctx):
    v = c.st(_VIA)
    return SimpleNamespace(**dict(zip(("copper_id", "net", "i", "j", "power", "added", "ctx",
                                       "route_id"), v)))


def _parse_snap(c, ctx):
    c.need(4)
    return SimpleNamespace()


def _parse_grds(c, ctx):
    which, nc = c.u32(), c.u32()
    label = c.str()
    if ctx.get("nc") is not None and nc != ctx["nc"]:
        raise CorruptChunk("GRDS: nc = %d, current RUN has %d" % (nc, ctx["nc"]))
    f = SimpleNamespace(which=which, nc=nc, label=label)
    for k in ("T0", "T1", "W0", "W1"):
        setattr(f, k, c.arr("i", nc))
    f.novia = c.raw(nc)
    return f


def _parse_rnb(c, ctx):
    rid, net, wide, power, npads, _ = c.st(_RNB)
    c.need(npads * _RNB_PAD.size)
    pads = [c.st(_RNB_PAD) for _ in range(npads)]
    name = c.str()
    labels = [c.str() for _ in range(npads)]
    return SimpleNamespace(route_id=rid, net=net, wide=wide, power=power, pads=pads, name=name,
                           labels=labels)


def _parse_rne(c, ctx):
    rid, net, rips_after, nripped, nfailed, status = c.st(_RNE)
    c.need(4 * (nripped + nfailed))
    ripped = [c.str() for _ in range(nripped)]
    failed = [c.str() for _ in range(nfailed)]
    return SimpleNamespace(route_id=rid, net=net, rips_after=rips_after, status=status,
                           ripped=ripped, failed=failed)


_SRCH_NAMES = ("search_id", "route_id", "ctx", "net", "flags", "gi", "gj", "rips_at_call",
               "max_cost", "cost", "expanded", "duration_s", "nsrc", "ntgt", "npath", "ntrace",
               "ntrace_total", "repeat_of", "enc", "reserved")


def _parse_srch(c, ctx):
    f = SimpleNamespace(**dict(zip(_SRCH_NAMES, c.st(_SRCH))))
    del f.reserved
    f.src = c.arr("I", f.nsrc)
    f.tgt = c.arr("I", f.ntgt)
    f.path = c.arr("I", f.npath)
    if f.enc == 0:
        f.trace_cell = c.arr("I", f.ntrace)
        f.trace_d = c.arr("i", f.ntrace) if f.flags & S_HAS_COSTS else array.array("i")
        f.trace_raw = None
    else:                                   # unknown encoding: no wavefront (7.11)
        f.trace_cell, f.trace_d = array.array("I"), array.array("i")
        f.trace_raw = bytes(c.b[c.o:])
    return f


def _parse_cmit(c, ctx):
    v = c.st(_CMIT)
    f = SimpleNamespace(copper_id=v[0], net=v[1], flags=v[2], power=v[3], ctx=v[4], route_id=v[5],
                        width_mm=v[10])
    npath, nstub, nvia = v[6], v[7], v[8]
    c.need(nstub * _STUB.size + nvia * 8 + npath * 4)
    f.stubs = []
    for _ in range(nstub):
        layer, _, x0, y0, x1, y1, w = c.st(_STUB)
        f.stubs.append((layer, x0, y0, x1, y1, w))
    f.vias = [c.st(_IJ) for _ in range(nvia)]
    f.path = c.arr("I", npath)
    return f


def _parse_rbld(c, ctx):
    rid, n = c.u32(), c.u32()
    c.need(8 * n)
    nets = list(c.arr("i", n))
    names = [c.str() for _ in range(n)]
    return SimpleNamespace(route_id=rid, nets=nets, names=names)


def _parse_rip(c, ctx):
    net, cx, rid, n = c.st(_RIP)
    removed = list(c.arr("I", n))
    return SimpleNamespace(net=net, ctx=cx, route_id=rid, removed=removed)


def _parse_drop(c, ctx):
    net, ok, mc = c.st(_DROP)
    return SimpleNamespace(net=net, ok=ok, max_cost=mc, name=c.str(), pad_label=c.str())


def _parse_mark(c, ctx):
    level = c.u32()
    return SimpleNamespace(level=level, text=c.str())


def _parse_rout(c, ctx):
    nseg, nvia = c.u32(), c.u32()
    c.need(nseg * _ROUT_SEG.size + nvia * _ROUT_VIA.size)
    segs = [c.st(_ROUT_SEG) for _ in range(nseg)]
    vias = [c.st(_ROUT_VIA)[:3] for _ in range(nvia)]
    return SimpleNamespace(segs=segs, vias=vias)


def _parse_end(c, ctx):
    v = c.st(_END)
    f = SimpleNamespace(searches=v[0], routes=v[1], nfailed=v[2], rips=v[3], expanded_total=v[4],
                        trace_entries=v[5], wall_s=v[6])
    c.need(8 * f.nfailed)
    f.failed = []
    for _ in range(f.nfailed):
        name = c.str()
        f.failed.append((name, c.str()))
    return f


def _parse_record(kind, body):
    c = _Cur(body)
    if kind == POLYSET:
        li, net, fp, role, sub, npoly = c.st(_POLY)
        c.need(4 * npoly)
        polys = []
        for _ in range(npoly):
            ncont = c.u32()
            c.need(4 * ncont)
            poly = []
            for _ in range(ncont):
                npts = c.u32()
                xy = c.arr("i", 2 * npts)
                poly.append([(xy[2 * k], xy[2 * k + 1]) for k in range(npts)])
            polys.append(poly)
        return SimpleNamespace(kind=kind, layer_index=li, net=net, fp=fp, role=role, sub=sub,
                               polys=polys)
    if kind == HOLE:
        v = c.st(_HOLE)
        return SimpleNamespace(kind=kind, **dict(zip(("x", "y", "w", "h", "rot_deg", "plated",
                                                      "shape", "fp", "net"), v)))
    if kind == PADINFO:
        v = c.st(_PADINFO)
        return SimpleNamespace(kind=kind, fp=v[0], net=v[1], x=v[2], y=v[3], attrib=v[4],
                               number=c.str())
    if kind == TRACK:
        v = c.st(_7I)
        return SimpleNamespace(kind=kind, **dict(zip(("layer_index", "net", "x0", "y0", "x1", "y1",
                                                      "w"), v)))
    if kind == VIA:
        v = c.st(_7I)
        return SimpleNamespace(kind=kind, **dict(zip(("x", "y", "dia", "drill", "net",
                                                      "top_layer_index", "bottom_layer_index"), v)))
    if kind == TEXT:
        v = c.st(_TEXT)
        return SimpleNamespace(kind=kind, layer_index=v[0], fp=v[1], x=v[2], y=v[3], angle_deg=v[4],
                               text=c.str())
    return SimpleNamespace(kind=kind, raw=bytes(body))


def _parse_brd(c, ctx):
    th, ox, oy = c.st(_BRD)
    f = SimpleNamespace(thickness_mm=th, origin_x_nm=ox, origin_y_nm=oy, label=c.str())
    n = c.u32()
    c.need(12 * n)
    f.layers = []
    for _ in range(n):
        kid, typ = c.i32(), c.i32()
        f.layers.append((kid, typ, c.str()))
    n = c.u32()
    c.need(20 * n)
    f.stackup = []
    for _ in range(n):
        c.align8()
        th_k, er = c.st(struct.Struct("<dd"))
        f.stackup.append((th_k, er, c.str()))
    n = c.u32()
    c.need(8 * n)
    f.nets = []
    for _ in range(n):
        code = c.i32()
        f.nets.append((code, c.str()))
    n = c.u32()
    c.need(36 * n)
    f.footprints = []
    for _ in range(n):
        c.align8()
        rot, x, y, flipped, _ = c.st(_FP)
        ref, value, fpid = c.str(), c.str(), c.str()
        f.footprints.append((rot, x, y, flipped, ref, value, fpid))
    n = c.u32()
    c.align8()
    c.need(8 * n)
    f.records = []
    for _ in range(n):
        kind, nbytes = c.u32(), c.u32()
        body = c.raw(nbytes)
        f.records.append(_parse_record(kind, body))
    return f


_PARSERS = {"RUN ": _parse_run, "NETS": _parse_nets, "PAD ": _parse_pad, "RECT": _parse_rect,
            "DISC": _parse_disc, "VIA ": _parse_via, "SNAP": _parse_snap, "GRDS": _parse_grds,
            "RNB ": _parse_rnb, "RNE ": _parse_rne, "SRCH": _parse_srch, "CMIT": _parse_cmit,
            "RBLD": _parse_rbld, "RIP ": _parse_rip, "DROP": _parse_drop, "MARK": _parse_mark,
            "ROUT": _parse_rout, "BRD ": _parse_brd, "END ": _parse_end}


def parse(tag, payload, nc=None):
    """Fields of one chunk payload (bytes) as a SimpleNamespace, or None for an
    unknown tag.  Raises CorruptChunk if the payload is too short.  nc: nx * ny
    of the current RUN, checked against GRDS."""
    fn = _PARSERS.get(tag)
    if fn is None:
        return None
    return fn(_Cur(payload), {"nc": nc})


class TraceReader:
    """Reads a trace file, or follows one while it is written (refresh()).

    Attributes: header (namespace or None while fewer than 24 bytes exist),
    chunks (list of namespaces: tag, t, offset, length, data, error),
    complete (an END was read), errors (corruption reports), off (file offset
    of the first chunk not read yet).  Unknown tags give data = None; a corrupt
    chunk gives data = None and error = the reason (it is skipped, spec 3.3).
    A complete chunk header with a non-printable tag stops reading for good."""

    def __init__(self, path=None, data=None):
        if (path is None) == (data is None):
            raise ValueError("give a path or data")
        self.path, self._data = path, data
        self.header = None
        self.chunks = []
        self.complete = False
        self.stopped = False
        self.errors = []
        self.off = 0
        self.size = 0
        self._nc = None
        self.refresh()

    def _tail(self):
        if self._data is not None:
            self.size = len(self._data)
            return memoryview(self._data)[self.off:]
        with open(self.path, "rb") as f:
            f.seek(0, os.SEEK_END)
            self.size = f.tell()
            f.seek(self.off)
            return memoryview(f.read())

    def refresh(self):
        """Read chunks appended since the last call; returns how many were added."""
        if self.complete or self.stopped:
            return 0
        tail = self._tail()
        pos = 0
        if self.header is None:
            if len(tail) < HEADER.size:
                return 0                    # not written yet
            magic, major, minor, flags, open_time = HEADER.unpack_from(tail, 0)
            if magic != MAGIC:
                raise ValueError("not a gridrouter trace file (magic %r)" % bytes(magic))
            if major != MAJOR:
                raise ValueError("unsupported trace format version %d.%d" % (major, minor))
            self.header = SimpleNamespace(major=major, minor=minor, flags=flags,
                                          open_time=open_time)
            pos = HEADER.size
        added = 0
        n = len(tail)
        while True:
            if n - pos < CHUNK.size:
                break                       # tail not written yet
            tag_b, ln, t = CHUNK.unpack_from(tail, pos)
            if any(b < 0x20 or b > 0x7E for b in tag_b):
                self.errors.append("offset %d: non-printable chunk tag %r; file corrupt from here"
                                   % (self.off + pos, bytes(tag_b)))
                self.stopped = True
                break
            nxt = pos + CHUNK.size + ln + (-ln % 8)
            if nxt > n:
                break                       # chunk not completely written yet
            tag = tag_b.decode("ascii")
            ch = SimpleNamespace(tag=tag, t=t, offset=self.off + pos, length=ln, data=None,
                                 error=None)
            payload = bytes(tail[pos + CHUNK.size:pos + CHUNK.size + ln])
            try:
                ch.data = parse(tag, payload, self._nc)
            except CorruptChunk as e:
                ch.error = str(e)
                self.errors.append("offset %d: %s chunk corrupt: %s" % (ch.offset, tag, e))
            if tag == "RUN ":
                # a corrupt RUN starts a session with no known grid (spec 6 rule 2)
                self._nc = None if ch.data is None else ch.data.nx * ch.data.ny
            self.chunks.append(ch)
            added += 1
            pos = nxt
            if tag == "END ":
                self.complete = True
                break
        self.off += pos
        return added


def read_trace(source):
    """TraceReader of a path, or of bytes."""
    if isinstance(source, (bytes, bytearray, memoryview)):
        return TraceReader(data=bytes(source))
    return TraceReader(path=source)


# -- replay and checks -------------------------------------------------------------
def replay(trace, upto=None):
    """Copper state (spec section 8) just after chunk index `upto` (default: the last).

    Returns a namespace: run (the latest RUN's fields, or None if there is no RUN or
    the latest one is corrupt), alive {copper_id: chunk}, edges {net: {layer: set of
    ((i,j),(i,j))}}, ewidth {(net, layer, edge): width}, vias {net: set of (i, j)},
    stubs {net: [(layer, x0, y0, x1, y1, w)]}.  edges / ewidth / stubs / vias are
    the union of the live copper items, applied in file order (a later CMIT's width
    wins on a shared edge).  Every RUN, also a corrupt one, starts a new session
    with no copper; router chunks before the first RUN or after a corrupt RUN
    belong to no usable session and are ignored (spec 6 rules 2 and 3)."""
    chunks = trace.chunks if upto is None else trace.chunks[:upto + 1]
    run, alive = None, {}
    for ch in chunks:
        if ch.tag == "RUN ":
            run, alive = ch.data, {}
            continue
        d = ch.data
        if d is None or run is None:
            continue
        if ch.tag == "CMIT":
            alive[d.copper_id] = ch
        elif ch.tag == "VIA " and d.added:
            alive[d.copper_id] = ch
        elif ch.tag == "RIP ":
            for cid in d.removed:
                alive.pop(cid, None)
    out = SimpleNamespace(run=run, alive=alive, edges={}, ewidth={}, vias={}, stubs={})
    if run is None:
        return out
    nx, nc = run.nx, run.nx * run.ny
    for cid in sorted(alive):
        ch = alive[cid]
        d = ch.data
        if ch.tag == "VIA ":
            out.vias.setdefault(d.net, set()).add((d.i, d.j))
            continue
        edges = out.edges.setdefault(d.net, {0: set(), 1: set()})
        path = [divmod(c, nc) for c in d.path]
        for k in range(1, len(path)):
            (pl, pidx), (layer, idx) = path[k - 1], path[k]
            if pl == layer:
                a, b = (pidx % nx, pidx // nx), (idx % nx, idx // nx)
                e = (min(a, b), max(a, b))
                edges.setdefault(layer, set()).add(e)
                out.ewidth[(d.net, layer, e)] = d.width_mm
        for ij in d.vias:
            out.vias.setdefault(d.net, set()).add(tuple(ij))
        if d.stubs:
            out.stubs.setdefault(d.net, []).extend(d.stubs)
    return out


_ROUTER_TAGS = frozenset(("PAD ", "RECT", "DISC", "VIA ", "SNAP", "GRDS", "RNB ", "RNE ", "SRCH",
                          "CMIT", "RBLD", "RIP ", "DROP", "ROUT"))


def check(trace, levels=False):
    """Checks a reader SHOULD make (spec 4.1, 6, 10), at the levels of spec 10.

    Errors: corrupt chunks, id mismatches, router chunks before the first RUN or
    in a session whose RUN is corrupt, cells out of range (3.4), a CMIT stub
    layer > 1, a GRDS whose nc is not the session's, a route_id that names no
    earlier RNB, a VIA with added = 0 and a copper id, and a RIP that lists a
    copper id that does not exist, belongs to an earlier session or was already
    removed.

    Warnings (inconsistencies that make no chunk corrupt): t decreasing or NaN,
    END not last, END counts and totals, SNAP not followed by GRDS which = 1,
    RNB / RNE pairing, route_id and ctx not matching the open route_net, other
    chunks inside a route_net, RBLD not followed by its RIPs, DROP not preceded
    by its SRCH / CMIT, repeat_of and other trace-field inconsistencies, a via
    cell outside the grid (VIA, CMIT vias; the via still takes its copper id),
    and a RIP removed list that is not strictly ascending or not exactly the
    copper of spec 7.14 (a listed copper id of another net of the session is
    still removed, as replay() does).

    Ids (4.1): a corrupt SRCH, RNB or CMIT takes the next id; after a mismatch
    the next expected id is the stored id plus one, so one gap is reported once.

    Returns a list of strings in file order, each warning prefixed with
    "warning: " (empty for a clean file).  With levels=True: a pair (errors,
    warnings) of lists of strings without the prefix."""
    out = []                                # (is_warning, text), file order

    def err(where, text):
        out.append((False, "%s: %s" % (where, text)))

    def warn(where, text):
        out.append((True, "%s: %s" % (where, text)))

    def route_ref(where, route_id):
        # a route_id names the open route_net, or NONE32 outside one (4.2, 6 rule 6)
        if route_id != NONE32 and route_id >= rid:
            err(where, "route_id %d names no earlier RNB" % route_id)
        elif route_id != (NONE32 if open_route is None else open_route):
            warn(where, "route_id %s, but the open route_net is %s" % (
                "-" if route_id == NONE32 else route_id, "-" if open_route is None else open_route))

    sid = rid = cid = 0                     # next expected ids (4.1)
    session = -1                            # index of the current session (each RUN starts one)
    run = None                              # its RUN; None before the first RUN or if corrupt
    nx = ny = nc = 0
    last_t = -math.inf
    open_route = None
    rnb_net = {}                            # route_id -> net of its RNB
    rne_seen = set()
    srch = {}                               # search_id -> data (this session)
    copper = {}                             # copper_id -> [session, net or None, removed]
    since_snap = {}                         # net -> its unremoved copper ids since SNAP / RUN
    snapped = False
    n_srch = n_rnb = 0
    exp_total = tr_total = 0
    chunks = trace.chunks
    for k, ch in enumerate(chunks):
        tag, d = ch.tag, ch.data
        where = "chunk %d (%s at %d)" % (k, tag, ch.offset)
        if math.isnan(ch.t):
            warn(where, "t is NaN")
        else:
            if ch.t < last_t:
                warn(where, "t decreases")
            last_t = ch.t
        if ch.error:
            err(where, ch.error)

        # -- ids: file-wide, never reset; a corrupt SRCH / RNB / CMIT takes its id too
        if tag == "SRCH":
            n_srch += 1
            if d is None:
                sid += 1
            else:
                if d.search_id != sid:
                    err(where, "search_id %d, expected %d" % (d.search_id, sid))
                sid = d.search_id + 1
        elif tag == "RNB ":
            n_rnb += 1
            if d is None:
                rid += 1
            else:
                if d.route_id != rid:
                    err(where, "route_id %d, expected %d" % (d.route_id, rid))
                rid = d.route_id + 1
        elif tag == "CMIT" or (tag == "VIA " and d is not None and d.added):
            this = cid if d is None else d.copper_id
            if this != cid:
                err(where, "copper_id %d, expected %d" % (this, cid))
            cid = this + 1
            copper[this] = [session, None if d is None else d.net, False]
        elif tag == "VIA " and d is not None and d.copper_id != NONE32:
            err(where, "VIA added = 0 with a copper id")

        # -- sessions (6 rules 2 and 3): every RUN starts one, also a corrupt RUN
        if tag == "RUN ":
            session += 1
            run = d
            nx, ny = (d.nx, d.ny) if d is not None else (0, 0)
            nc = nx * ny
            srch, since_snap, snapped = {}, {}, False
            if open_route is not None:
                warn(where, "RUN while route_net %d has no RNE" % open_route)
                open_route = None
            continue
        if d is None:
            continue
        if tag in _ROUTER_TAGS:
            if session < 0:
                err(where, "before any RUN")
                continue
            if run is None:
                err(where, "in a router session whose RUN is corrupt")
                continue
        if open_route is not None and tag not in ("SRCH", "RBLD", "RIP ", "CMIT", "RNE "):
            warn(where, "inside route_net %d" % open_route)

        if tag == "SRCH":
            if d.expanded >= 0:
                exp_total += d.expanded
            tr_total += d.ntrace
            # src / tgt may hold any value when the search raised (error, 3.4)
            names = ("path", "trace_cell") if d.flags & S_ERROR else \
                ("src", "tgt", "path", "trace_cell")
            for name in names:
                a = getattr(d, name)
                if a and max(a) >= 2 * nc:
                    err(where, "%s cell out of range" % name)
            if d.trace_cell and len(set(d.trace_cell)) != len(d.trace_cell):
                warn(where, "a trace cell appears twice")
            route_ref(where, d.route_id)
            if (d.ctx == 1) != (open_route is not None):
                warn(where, "ctx %d %s a route_net" % (d.ctx, "inside" if open_route is not None
                                                       else "outside"))
            if d.repeat_of != NONE32:
                t = srch.get(d.repeat_of)
                if t is None or t.repeat_of != NONE32:
                    warn(where, "repeat_of %d is not an earlier non-repeat search of the session"
                         % d.repeat_of)
                elif d.ntrace != 0 or d.ntrace_total != t.ntrace_total or \
                        (d.flags ^ t.flags) & (S_HAS_TRACE | S_HAS_COSTS):
                    warn(where, "repeat fields differ from search %d" % d.repeat_of)
            if d.ntrace > d.ntrace_total:
                warn(where, "ntrace > ntrace_total")
            srch[d.search_id] = d
        elif tag in ("CMIT", "VIA "):
            if tag == "CMIT":
                if d.path and max(d.path) >= 2 * nc:
                    err(where, "path cell out of range")
                if any(s[0] > 1 for s in d.stubs):
                    err(where, "stub layer > 1")
                out_of_grid = [v for v in d.vias if not (0 <= v[0] < nx and 0 <= v[1] < ny)]
                if out_of_grid:
                    warn(where, "via cell %r outside the grid" % (tuple(out_of_grid[0]),))
                if (d.ctx == 1) != (open_route is not None):
                    warn(where, "ctx %d %s a route_net" % (d.ctx, "inside" if open_route is not None
                                                           else "outside"))
            elif not (0 <= d.i < nx and 0 <= d.j < ny):
                warn(where, "via cell (%d, %d) outside the grid" % (d.i, d.j))
            if tag == "CMIT" or d.added:
                since_snap.setdefault(d.net, set()).add(d.copper_id)
            route_ref(where, d.route_id)
        elif tag == "RNB ":
            if open_route is not None:
                warn(where, "route_net inside route_net %d" % open_route)
            open_route = d.route_id
            rnb_net[d.route_id] = d.net
        elif tag == "RNE ":
            if d.route_id >= rid:
                err(where, "route_id %d names no earlier RNB" % d.route_id)
            else:
                if open_route != d.route_id:
                    warn(where, "RNE without its RNB")
                if d.route_id in rne_seen:
                    warn(where, "second RNE for route_id %d" % d.route_id)
                if rnb_net.get(d.route_id, d.net) != d.net:
                    warn(where, "RNE net %d differs from its RNB's" % d.net)
                rne_seen.add(d.route_id)
            open_route = None
        elif tag == "SNAP":
            nxt = chunks[k + 1] if k + 1 < len(chunks) else None
            if nxt is not None and (nxt.tag != "GRDS" or nxt.data is None or nxt.data.which != 1):
                warn(where, "not followed by GRDS which = 1")
            since_snap, snapped = {}, True
        elif tag == "GRDS":
            if d.nc != nc:
                err(where, "nc %d != %d" % (d.nc, nc))
        elif tag == "RBLD":
            want = [n for n in d.nets if n != -1]
            got = []
            for c in chunks[k + 1:k + 1 + len(want)]:
                if c.tag != "RIP " or c.data is None or c.data.ctx != 1 or \
                        c.data.route_id != d.route_id:
                    break
                got.append(c.data.net)
            if got != want:
                warn(where, "followed by RIPs of %r, expected %r" % (got, want))
            route_ref(where, d.route_id)
        elif tag == "RIP ":
            removed = list(d.removed)
            if any(b <= a for a, b in zip(removed, removed[1:])):
                warn(where, "removed not strictly ascending")
            route_ref(where, d.route_id)
            # 7.14: exactly the net's unremoved copper of the session since its last
            # SNAP (since RUN if none); before the session's first SNAP an empty
            # list is also right (the router was never snapshotted).
            live = sorted(since_snap.get(d.net, ()))
            for x in removed:
                c = copper.get(x)
                if c is None:
                    err(where, "removes copper id %d, which does not exist" % x)
                elif c[0] != session:
                    err(where, "removes copper id %d of an earlier router session" % x)
                elif c[2]:
                    err(where, "removes copper id %d, already removed" % x)
                else:
                    if c[1] is not None and c[1] != d.net:
                        warn(where, "RIP of net %d removes copper id %d of net %d"
                             % (d.net, x, c[1]))
                    c[2] = True
                    if c[1] is not None and c[1] in since_snap:
                        since_snap[c[1]].discard(x)
            if removed or snapped:
                if live != removed:
                    warn(where, "removed %r, expected %r" % (removed, live))
                since_snap.pop(d.net, None)
        elif tag == "DROP":
            j = k - 1
            seen = []
            while j >= 0 and chunks[j].tag in ("SRCH", "CMIT") and chunks[j].data is not None \
                    and chunks[j].data.ctx == 2:
                seen.append(chunks[j].tag)
                j -= 1
            seen.reverse()
            ok = seen in (["SRCH", "CMIT"], ["SRCH", "SRCH", "CMIT"]) if d.ok == 1 else \
                (len(seen) in (0, 1, 2) and "CMIT" not in seen)
            if not ok:
                warn(where, "preceded by %r" % seen)
        elif tag == "END ":
            if k != len(chunks) - 1:
                warn(where, "END is not the last chunk")
            if d.searches != n_srch or d.routes != n_rnb:
                warn(where, "END counts %d / %d, file has %d / %d"
                     % (d.searches, d.routes, n_srch, n_rnb))
            if d.expanded_total != exp_total or d.trace_entries != tr_total:
                warn(where, "END totals differ from the SRCH chunks")
    if levels:
        return [t for w, t in out if not w], [t for w, t in out if w]
    return ["warning: " + t if w else t for w, t in out]


# -- summary / dump -----------------------------------------------------------------
def summary(trace):
    """A dict of counts for the command line."""
    counts = {}
    s = SimpleNamespace(searches=0, found=0, errors=0, repeats=0, expanded=0, trace_entries=0,
                        trace_total=0, nets=set(), routed=set(), rnb=0, failed_pads=0, rips=0,
                        drop_failed=0, copper=0, runs=0)
    end = None
    for ch in trace.chunks:
        counts[ch.tag] = counts.get(ch.tag, 0) + 1
        d = ch.data
        if d is None:
            continue
        if ch.tag == "SRCH":
            s.searches += 1
            s.found += bool(d.flags & S_FOUND)
            s.errors += bool(d.flags & S_ERROR)
            s.repeats += d.repeat_of != NONE32
            s.expanded += max(d.expanded, 0)
            s.trace_entries += d.ntrace
            s.trace_total += d.ntrace_total
        elif ch.tag == "NETS":
            s.nets.update(code for code, _ in d.entries)
        elif ch.tag == "RNB ":
            s.rnb += 1
            s.routed.add(d.net)
        elif ch.tag == "RNE ":
            s.failed_pads += len(d.failed)
        elif ch.tag == "RIP ":
            s.rips += 1
        elif ch.tag == "DROP":
            s.drop_failed += d.ok != 1
        elif ch.tag in ("CMIT", "VIA ") and (ch.tag == "CMIT" or d.added):
            s.copper += 1
        elif ch.tag == "RUN ":
            s.runs += 1
        elif ch.tag == "END ":
            end = d
    return dict(size=trace.size, complete=trace.complete, chunks=len(trace.chunks), counts=counts,
                runs=s.runs, searches=s.searches, found=s.found, search_errors=s.errors,
                repeats=s.repeats, expanded=s.expanded, trace_entries=s.trace_entries,
                trace_total=s.trace_total, named_nets=len(s.nets), route_net_calls=s.rnb,
                routed_nets=len(s.routed), failed_pads=s.failed_pads, drop_failed=s.drop_failed,
                rip_chunks=s.rips, copper_items=s.copper,
                end_failed=None if end is None else end.nfailed,
                end_rips=None if end is None else end.rips, errors=list(trace.errors))


def _describe(ch):
    d = ch.data
    if d is None:
        return ch.error or "(unknown tag, skipped)"
    tag = ch.tag
    if tag == "RUN ":
        return "%dx%d nd=%d cell=%g via_cost=%d max_rips=%d producer=%r" % (
            d.nx, d.ny, d.nd, d.cell, d.via_cost, d.max_rips, d.producer)
    if tag == "NETS":
        return "%d names" % len(d.entries)
    if tag == "PAD ":
        return "net %d layers %d (%g, %g)" % (d.net, d.layers, d.cx, d.cy)
    if tag == "RECT":
        return "op %d net %d layers %d (%g, %g)-(%g, %g)" % (d.op, d.net, d.layers, d.x0, d.y0,
                                                              d.x1, d.y1)
    if tag == "DISC":
        return "net %d layer %d (%d, %d) r %d/%d" % (d.net, d.layer, d.i, d.j, d.r_t, d.r_w)
    if tag == "VIA ":
        return "copper %s net %d (%d, %d) added %d" % (
            "-" if d.copper_id == NONE32 else d.copper_id, d.net, d.i, d.j, d.added)
    if tag == "GRDS":
        return "which %d nc %d label %r" % (d.which, d.nc, d.label)
    if tag == "RNB ":
        return "route %d net %d %r %d pads%s" % (d.route_id, d.net, d.name, len(d.pads),
                                                  " wide" if d.wide else "")
    if tag == "RNE ":
        return "route %d net %d status %d ripped %r failed %r" % (d.route_id, d.net, d.status,
                                                                 d.ripped, d.failed)
    if tag == "SRCH":
        names = ("wide", "goal", "max", "via_goal", "relax", "found", "new_via", "costs", "ERROR",
                 "trace")
        fl = ",".join(n for k, n in enumerate(names) if d.flags >> k & 1)
        rep = "" if d.repeat_of == NONE32 else " repeat_of %d" % d.repeat_of
        return "search %d net %d ctx %d [%s] cost %d expanded %d path %d trace %d/%d%s" % (
            d.search_id, d.net, d.ctx, fl, d.cost, d.expanded, d.npath, d.ntrace, d.ntrace_total,
            rep)
    if tag == "CMIT":
        return "copper %d net %d ctx %d path %d stubs %d vias %d w %g" % (
            d.copper_id, d.net, d.ctx, len(d.path), len(d.stubs), len(d.vias), d.width_mm)
    if tag == "RBLD":
        return "route %s drop %r nets %r" % ("-" if d.route_id == NONE32 else d.route_id, d.names,
                                             d.nets)
    if tag == "RIP ":
        return "net %d ctx %d removed %r" % (d.net, d.ctx, d.removed)
    if tag == "DROP":
        return "net %d %r pad %r ok %d" % (d.net, d.name, d.pad_label, d.ok)
    if tag == "MARK":
        return "%s %r" % ("phase" if d.level == 0 else "note", d.text)
    if tag == "ROUT":
        return "%d segments, %d vias" % (len(d.segs), len(d.vias))
    if tag == "BRD ":
        return "label %r: %d layers, %d nets, %d footprints, %d records" % (
            d.label, len(d.layers), len(d.nets), len(d.footprints), len(d.records))
    if tag == "END ":
        return "searches %d routes %d failed %d rips %d" % (d.searches, d.routes, d.nfailed, d.rips)
    return ""


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="python3 -m gridrouter.tracefile",
                                 description="Inspect a gridrouter trace file (.grtrace).")
    ap.add_argument("command", choices=("summary", "dump", "check"))
    ap.add_argument("file")
    a = ap.parse_args(argv)
    tr = read_trace(a.file)
    if a.command == "dump":
        if tr.header is not None:
            print("header: version %d.%d, opened %s" % (
                tr.header.major, tr.header.minor,
                time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(tr.header.open_time))))
        for ch in tr.chunks:
            print("%10d  %10.4f  %s %8d  %s" % (ch.offset, ch.t, ch.tag, ch.length, _describe(ch)))
        if not tr.complete:
            print("(no END: incomplete run; %d bytes after the last complete chunk)"
                  % (tr.size - tr.off))
        for e in tr.errors:
            print("ERROR:", e)
        return 1 if tr.errors else 0
    if a.command == "check":
        errs, warns = check(tr, levels=True)
        probs = list(tr.errors) + errs
        for p in probs:
            print(p)
        for w in warns:
            print("warning:", w)
        print("%s: %d chunks, %s, %d problems%s" % (
            a.file, len(tr.chunks), "complete" if tr.complete else "incomplete", len(probs),
            ", %d warnings" % len(warns) if warns else ""))
        return 1 if probs else 0
    s = summary(tr)
    print("file:            %s (%d bytes, %.2f MB)" % (a.file, s["size"], s["size"] / 1e6))
    print("complete:        %s" % ("yes (END present)" if s["complete"] else "no"))
    print("chunks:          %d" % s["chunks"])
    for tag in sorted(s["counts"]):
        print("    %-6r %d" % (tag, s["counts"][tag]))
    print("router sessions: %d" % s["runs"])
    print("searches:        %d (%d found a path, %d raised, %d repeats)" % (
        s["searches"], s["found"], s["search_errors"], s["repeats"]))
    print("expanded states: %d" % s["expanded"])
    print("trace entries:   %d stored, %d counted" % (s["trace_entries"], s["trace_total"]))
    print("nets:            %d named, %d routed in %d route_net calls" % (
        s["named_nets"], s["routed_nets"], s["route_net_calls"]))
    print("copper items:    %d" % s["copper_items"])
    print("rip chunks:      %d" % s["rip_chunks"])
    print("failures:        %d failed route_net pads, %d failed plane drops; END: %s failed, "
          "%s rips" % (s["failed_pads"], s["drop_failed"], s["end_failed"], s["end_rips"]))
    for e in s["errors"]:
        print("ERROR:", e)
    return 1 if s["errors"] else 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))

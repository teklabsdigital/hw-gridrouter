"""Pure-Python oracle: the Router of obd2_dongle/scripts/gen_pcb.py (copied 2026-11-21).

The class body is the reference code unchanged except for:
  * the module constants (NX, NY, G, DIRS, STEP, TURN, ...) are set by
    configure() instead of from the board and the environment;
  * search() records self.last_cost (d of the popped goal state) and
    self.last_expanded (states popped after the stale check) for the tests;
  * emit() is replaced by routes(), the same algorithm returning plain data
    instead of creating pcbnew objects (and pruning a copy of the edges).
Do not "fix" anything here: it is the specification the C router is tested against.
"""
import copy
import heapq

G = 0.1
NX = NY = NC = 0
BLOCK = 1 << 30
STRICT = False
DIRS = STEP = TURN = None
START = 0
WRONG_WAY = DIAGONAL = None
VIA_COST = 250
RIP_COST, MAX_RIPS = 150, 400
BEND_45, BEND_90 = 12, 40
RULES = {"edge": 0.3}
TRACK, TRACK_PWR = 0.20, 0.50
VIA_DIA = 0.60


def _step(along_x):
    """Cost of each direction on a layer that runs along x (F.Cu) or along y (B.Cu)."""
    out = []
    for dx, dy, c in DIRS:
        f = DIAGONAL if dx and dy else 1.0 if (dx if along_x else dy) else WRONG_WAY
        out.append(int(round(c * f)))
    return out


def _turns():
    rows = []
    for ax, ay, _ in DIRS:
        row = []
        for bx, by, _ in DIRS:
            dot = (ax * bx + ay * by) / ((ax * ax + ay * ay) ** 0.5 * (bx * bx + by * by) ** 0.5)
            row.append(0 if dot > 0.9 else BEND_45 if dot > 0.5 else BEND_90 if dot > -0.1 else None)
        rows.append(row)
    rows.append([0] * len(DIRS))    # index START: start of a path or just after a via
    return rows


def configure(nx, ny, strict=False, cell=0.1, via_cost=250, rip_cost=150, max_rips=400,
              bend_45=12, bend_90=40):
    """Set the module constants the way gen_pcb.py computes them."""
    global G, NX, NY, NC, STRICT, DIRS, WRONG_WAY, DIAGONAL, START, STEP, TURN
    global VIA_COST, RIP_COST, MAX_RIPS, BEND_45, BEND_90
    G, NX, NY, NC = cell, nx, ny, nx * ny
    STRICT = strict
    if STRICT:
        DIRS = [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10)]
        WRONG_WAY, DIAGONAL = 3.0, 1.0
    else:
        DIRS = [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10),
                (1, 1, 14), (1, -1, 14), (-1, 1, 14), (-1, -1, 14)]
        WRONG_WAY, DIAGONAL = 1.6, 1.2
    START = len(DIRS)
    VIA_COST, RIP_COST, MAX_RIPS = via_cost, rip_cost, max_rips
    BEND_45, BEND_90 = bend_45, bend_90
    STEP = [_step(True), _step(False)]
    TURN = _turns()


_disc = {}


def disc(r):
    if r not in _disc:
        _disc[r] = [(dx, dy) for dx in range(-r, r + 1) for dy in range(-r, r + 1)
                    if dx * dx + dy * dy < (r + 0.5) ** 2]
    return _disc[r]


class Router:
    def __init__(self):
        self.T = [[0] * NC, [0] * NC]
        self.W = [[0] * NC, [0] * NC]
        self.novia = bytearray(NC)          # via centre not allowed (pads, other vias)
        self.edges = {}                     # net -> layer -> set of ((i,j),(i,j))
        self.stubs = {}                     # net -> [(layer, x0, y0, x1, y1)]
        self.vias = {}                      # net -> set of (i, j)
        self.padcell = {}                   # net -> {(layer, idx): (x, y) pad centre}
        self.width = {}
        self.failed = []
        self.own = None                     # (level, layer, idx) -> nets whose tracks claim it
        self.ewidth = {}
        for lev, margin in ((self.T, RULES["edge"] + 0.15), (self.W, RULES["edge"] + 0.35)):
            m = int(margin / G + 0.999)
            for g in lev:
                for j in range(NY):
                    for i in range(NX):
                        if i < m or j < m or i > NX - 1 - m or j > NY - 1 - m:
                            g[j * NX + i] = BLOCK

    # -- occupancy ---------------------------------------------------------
    @staticmethod
    def _claim(g, idx, net):
        v = g[idx]
        if v == 0:
            g[idx] = net
        elif v != net:
            g[idx] = BLOCK

    def claim_rect(self, net, layers, x0, y0, x1, y1, d_t, d_w):
        for lev, d in ((self.T, d_t), (self.W, d_w)):
            i0 = max(0, int((x0 - d) / G - 1e-6) + (1 if x0 - d > 0 else 0))
            j0 = max(0, int((y0 - d) / G - 1e-6) + (1 if y0 - d > 0 else 0))
            i1 = min(NX - 1, int((x1 + d) / G + 1e-6))
            j1 = min(NY - 1, int((y1 + d) / G + 1e-6))
            for layer in layers:
                g = lev[layer]
                for j in range(j0, j1 + 1):
                    base = j * NX
                    for i in range(i0, i1 + 1):
                        self._claim(g, base + i, net)

    def claim_disc(self, net, layer, i, j, r_t, r_w):
        own = self.own
        for li, (lev, r) in enumerate(((self.T, r_t), (self.W, r_w))):
            g = lev[layer]
            for dx, dy in disc(r):
                a, b = i + dx, j + dy
                if 0 <= a < NX and 0 <= b < NY:
                    idx = b * NX + a
                    self._claim(g, idx, net)
                    if own is not None:
                        own.setdefault((li, layer, idx), set()).add(net)

    # -- rip-up support ------------------------------------------------------
    def snapshot(self):
        """Everything committed so far (pads, plane stubs, escapes) becomes permanent."""
        self.snap = copy.deepcopy((self.T, self.W, self.novia, self.edges, self.stubs,
                                   self.vias, self.ewidth))
        self.log = {}                       # net name -> [(net, path, wide, power)]
        self.own = {}
        self.rips = 0

    def rebuild(self, drop):
        """Remove the nets in drop and replay every other routed net."""
        (self.T, self.W, self.novia, self.edges, self.stubs,
         self.vias, self.ewidth) = copy.deepcopy(self.snap)
        for n in drop:
            self.log.pop(n, None)
        self.own = {}
        for items in self.log.values():
            for net, path, wide, power in items:
                self.commit(net, path, wide, power)

    def block_rect(self, x0, y0, x1, y1):
        self.claim_rect(BLOCK, (0, 1), x0, y0, x1, y1, 0.0, 0.0)
        for j in range(max(0, int(y0 / G)), min(NY - 1, int(y1 / G) + 1) + 1):
            for i in range(max(0, int(x0 / G)), min(NX - 1, int(x1 / G) + 1) + 1):
                self.novia[j * NX + i] = 1

    def add_pad(self, net, layers, cx, cy, x0, y0, x1, y1, power):
        self.claim_rect(net, layers, x0, y0, x1, y1,
                        0.30 + 0.1 * power, 0.50 + 0.1 * power)
        d = VIA_DIA / 2 + 0.1
        for j in range(max(0, int((y0 - d) / G)), min(NY - 1, int((y1 + d) / G) + 1) + 1):
            for i in range(max(0, int((x0 - d) / G)), min(NX - 1, int((x1 + d) / G) + 1) + 1):
                self.novia[j * NX + i] = 1
        cells = self.padcell.setdefault(net, {})
        for j in range(int(y0 / G) + 1, int(y1 / G) + 1):
            for i in range(int(x0 / G) + 1, int(x1 / G) + 1):
                if x0 < i * G < x1 and y0 < j * G < y1:
                    for layer in layers:
                        cells[(layer, j * NX + i)] = (cx, cy)

    def via_ok(self, idx, net):
        if self.novia[idx]:
            return False
        a, b = self.W[0][idx], self.W[1][idx]
        return (a == 0 or a == net) and (b == 0 or b == net)

    # -- search ------------------------------------------------------------
    def search(self, net, sources, targets, wide, goal=None, max_cost=None, via_goal=False,
               relax=False):
        """A* from a set of (layer, idx) to a set of (layer, idx).

        via_goal=True instead stops at the first cell where a via fits
        (used to drop GND / +3V3 pads into their plane).
        """
        self.last_cost, self.last_expanded = None, 0                    # [test instrumentation]
        lev = self.W if wide else self.T
        hard = self.snap[1 if wide else 0] if relax else None
        dist, prev, heap = {}, {}, []
        if goal:
            gi, gj = goal

            def h(layer, idx):
                dx, dy = abs(idx % NX - gi), abs(idx // NX - gj)
                return max(0, (10 * (dx + dy) if STRICT else 10 * max(dx, dy) + 4 * min(dx, dy)) - 30)
        else:
            def h(layer, idx):
                return 0
        for layer, idx in sources:
            v = lev[layer][idx]
            if v == 0 or v == net or (hard and hard[layer][idx] in (0, net)):
                dist[(layer, idx, START)] = 0
                heapq.heappush(heap, (h(layer, idx), 0, layer, idx, START))
        while heap:
            f, d, layer, idx, dr = heapq.heappop(heap)
            k = (layer, idx, dr)
            if dist.get(k, 1 << 60) < d:
                continue
            self.last_expanded += 1                                     # [test instrumentation]
            s = (layer, idx)
            if via_goal:
                if s in targets or self.via_ok(idx, net):
                    self.last_cost = d                                  # [test instrumentation]
                    return self._trace(prev, k), s not in targets
            elif s in targets:
                self.last_cost = d                                      # [test instrumentation]
                return self._trace(prev, k), False
            if max_cost is not None and d > max_cost:
                return None, False
            i, j = idx % NX, idx // NX
            g = lev[layer]
            turn = TURN[dr]
            step = STEP[layer]
            for nd_i, (dx, dy, _) in enumerate(DIRS):
                c = step[nd_i]
                t = turn[nd_i]
                if t is None:
                    continue
                a, b = i + dx, j + dy
                if not (0 <= a < NX and 0 <= b < NY):
                    continue
                n = b * NX + a
                v = g[n]
                if v != 0 and v != net:
                    if hard is None or hard[layer][n] not in (0, net):
                        continue
                    t += RIP_COST
                nd = d + c + t
                key = (layer, n, nd_i)
                if nd < dist.get(key, 1 << 60):
                    dist[key] = nd
                    prev[key] = k
                    heapq.heappush(heap, (nd + h(layer, n), nd, layer, n, nd_i))
            if not via_goal and self.via_ok(idx, net):
                other = 1 - layer
                v = lev[other][idx]
                if v == 0 or v == net or (hard and hard[other][idx] in (0, net)):
                    nd = d + VIA_COST + (0 if v == 0 or v == net else RIP_COST)
                    key = (other, idx, START)
                    if nd < dist.get(key, 1 << 60):
                        dist[key] = nd
                        prev[key] = k
                        heapq.heappush(heap, (nd + h(other, idx), nd, other, idx, START))
        return None, False

    @staticmethod
    def _trace(prev, s):
        path = [s[:2]]
        while s in prev:
            s = prev[s]
            path.append(s[:2])
        return path[::-1]

    # -- committing --------------------------------------------------------
    def commit(self, net, path, wide, power, end_via=False):
        self.width[net] = TRACK_PWR if wide else self.width.get(net, TRACK)
        r_t, r_w = (5, 7) if wide else (3, 5)
        r_t, r_w = r_t + int(power), r_w + int(power)     # power: extra clearance in grid cells
        edges = self.edges.setdefault(net, {0: set(), 1: set()})
        cells = self.padcell.get(net, {})
        for k, (layer, idx) in enumerate(path):
            i, j = idx % NX, idx // NX
            self.claim_disc(net, layer, i, j, r_t, r_w)
            if k:
                pl, pidx = path[k - 1]
                if pl == layer:
                    a = (pidx % NX, pidx // NX)
                    e = (min(a, (i, j)), max(a, (i, j)))
                    edges[layer].add(e)
                    self.ewidth[(net, layer, e)] = TRACK_PWR if wide else TRACK
                else:
                    self.add_via(net, i, j, power)
        for layer, idx in (path[0], path[-1]):
            c = cells.get((layer, idx))
            if c:
                x, y = (idx % NX) * G, (idx // NX) * G
                st = self.stubs.setdefault(net, [])
                if abs(x - c[0]) > 1e-6:
                    st.append((layer, c[0], c[1], x, c[1]))
                if abs(y - c[1]) > 1e-6:
                    st.append((layer, x, c[1], x, y))
        if end_via:
            idx = path[-1][1]
            self.add_via(net, idx % NX, idx // NX, power)

    def add_via(self, net, i, j, power):
        if (i, j) in self.vias.setdefault(net, set()):
            return
        self.vias[net].add((i, j))
        r_t, r_w = 5 + int(power), 7 + int(power)
        for layer in (0, 1):
            self.claim_disc(net, layer, i, j, r_t, r_w)
        for dx, dy in disc(8):                 # 0.8 mm centre spacing between any two vias
            a, b = i + dx, j + dy
            if 0 <= a < NX and 0 <= b < NY:
                self.novia[b * NX + a] = 1

    # -- nets --------------------------------------------------------------
    def route_net(self, net, name, pads, wide, power):
        """pads: [(cx, cy, layers, cells, label, bbox)].  Prim-style: grow one tree.

        If a pad cannot be reached, the path is searched again through other
        nets' tracks; the nets in the way are ripped up and returned so that
        the caller can route them again.
        """
        self.failed = [f for f in self.failed if f[0] != name]
        todo = list(pads[1:])
        tree = set(pads[0][3])
        done = [(pads[0][0], pads[0][1])]
        ripped = []
        while todo:
            todo.sort(key=lambda p: min(abs(p[0] - x) + abs(p[1] - y) for x, y in done))
            p = todo.pop(0)
            targets = set(p[3])
            goal = (int(round(p[0] / G)), int(round(p[1] / G)))
            path = None
            for w, relax in ((wide, False), (False, False), (wide, True), (False, True)):
                if relax and self.rips >= MAX_RIPS:
                    break
                path, _ = self.search(net, tree, targets, w, goal=goal, relax=relax)
                if path is None:
                    continue
                if relax:
                    li = 1 if w else 0
                    nets = set()
                    for layer, idx in path:
                        nets |= self.own.get((li, layer, idx), set())
                    nets.discard(net)
                    victims = [nm for nm, items in self.log.items() if items[0][0] in nets]
                    self.rips += 1
                    self.rebuild(victims)
                    ripped += victims
                    lev = self.W if w else self.T
                    if any(lev[layer][idx] not in (0, net) for layer, idx in path):
                        path = None
                        continue
                break
            if path is None:
                self.failed.append((name, p[4]))
                continue
            self.log.setdefault(name, []).append((net, path, w, power))
            self.commit(net, path, w, power)
            tree.update(path)
            tree.update(targets)
            done.append((p[0], p[1]))
        return ripped

    def drop_to_plane(self, net, name, pad, joined):
        """Short stub from an SMD pad to a via into the plane (or to an existing stub)."""
        for wide in (True, False):
            path, new_via = self.search(net, set(pad[3]), joined, wide,
                                        max_cost=600, via_goal=True)
            if path is not None:
                self.commit(net, path, wide, False, end_via=new_via)
                if not wide:
                    self.width[net] = min(self.width.get(net, TRACK), TRACK)
                joined.update(path)
                return True
        self.failed.append((name, pad[4]))
        return False

    # -- output (emit() with pcbnew calls replaced by plain tuples) ---------
    def routes(self):
        segs = []
        for net, layers in self.edges.items():
            for layer, edges in layers.items():
                edges = set(edges)                                      # [copy: do not prune self.edges]
                anchor = {(idx % NX, idx // NX) for (l, idx) in self.padcell.get(net, {}) if l == layer}
                anchor |= self.vias.get(net, set())
                while True:                 # drop spurs: unused ends of pin escapes
                    deg = {}
                    for a, b in edges:
                        deg[a] = deg.get(a, 0) + 1
                        deg[b] = deg.get(b, 0) + 1
                    spur = {e for e in edges
                            if any(deg[c] == 1 and c not in anchor for c in e)}
                    if not spur:
                        break
                    edges -= spur
                adj = {}
                for a, b in edges:
                    adj.setdefault(a, []).append(b)
                    adj.setdefault(b, []).append(a)
                seen = set()
                for a, b in sorted(edges):
                    if (a, b) in seen:
                        continue
                    d = (b[0] - a[0], b[1] - a[1])
                    w = self.ewidth[(net, layer, (a, b))]
                    start, end = a, b
                    seen.add((a, b))
                    while True:             # extend forward
                        nxt = (end[0] + d[0], end[1] + d[1])
                        e = (min(end, nxt), max(end, nxt))
                        if len(adj[end]) == 2 and e in edges and e not in seen \
                                and self.ewidth[(net, layer, e)] == w:
                            seen.add(e)
                            end = nxt
                        else:
                            break
                    while True:             # extend backward
                        nxt = (start[0] - d[0], start[1] - d[1])
                        e = (min(start, nxt), max(start, nxt))
                        if len(adj[start]) == 2 and e in edges and e not in seen \
                                and self.ewidth[(net, layer, e)] == w:
                            seen.add(e)
                            start = nxt
                        else:
                            break
                    segs.append((layer, start[0] * G, start[1] * G, end[0] * G, end[1] * G, w, net))
        for net, stubs in self.stubs.items():
            for layer, x0, y0, x1, y1 in stubs:
                segs.append((layer, x0, y0, x1, y1, TRACK, net))
        vias = [(i * G, j * G, net) for net, vs in self.vias.items() for i, j in vs]
        return segs, vias

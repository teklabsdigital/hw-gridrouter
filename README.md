# gridrouter

A grid router for printed circuit boards, rewritten from Python to C. Routing a
real 4-layer KiCad board went from 16 to 38 minutes to about 11 to 17 seconds.
On the same inputs it gives the same routes as the Python version. In the benchmark it is about 60x faster: the whole test
board takes 1.27 s instead of 84.9 s, with an identical result (see the
[benchmark table](#speed-testsbenchpy) below).

It is a two-layer grid (maze) router for Python scripts that generate KiCad
boards. It re-implements the pure-Python router that was first written inside
the board generator of the [Bluetooth OBD-II dongle](https://github.com/teklabsdigital/hw-obd2-dongle) project.
The behaviour is the same: on the same inputs it returns the same paths, costs,
rip-ups and failures, as checked by the tests. It runs about 60x faster. The
dongle's board generator now uses this library to route its real board.

The A* search, the occupancy grids, claiming and rip-up are in C
(`gridrouter/gridrouter.c`, loaded with `ctypes`). Bookkeeping and the per-net
driver logic stay in Python.

- No dependencies: standard-library Python 3.9+ and a C compiler (`cc` on macOS
  and Linux; on Windows MSVC `cl`, MinGW-w64 `gcc` or LLVM `clang`).
- Does not import `pcbnew`. The router returns plain lists of segments and vias.
  An optional helper (`gridrouter.kicad.emit`) turns them into KiCad objects.
- The shared library is compiled automatically on first import, and again
  whenever `gridrouter.c` / `gridrouter.h` is newer than it. On macOS it is a
  universal (arm64 + x86_64) dylib, so it loads in the system Python and in
  KiCad's bundled Python 3.9, native or under Rosetta.

## Layout

```
router/
  gridrouter/__init__.py    Router class, build-on-first-use, cost-table helpers
  gridrouter/gridrouter.c   C core: grids, claims, ownership / rip-up, A* search
  gridrouter/gridrouter.h   C API
  gridrouter/kicad.py       optional: write routes() into a pcbnew BOARD (pcbnew passed in)
  tests/ref_router.py       the oracle: the original pure-Python Router, copied unchanged (constants configurable)
  tests/common.py           random-scene helpers shared by the tests
  tests/test_search.py      C search vs oracle on random boards, every mode, both direction tables
  tests/test_ripup.py       incremental rip-up: A/B vs fresh router; full route-with-rip-up vs oracle
  tests/test_api.py         API details and error handling
  tests/test_kicad_emit.py  gridrouter.kicad.emit on an in-memory board (KiCad's Python only)
  tests/run_all.py          runs all of the above
  tests/bench.py            timing on a 501 x 361 grid, C vs oracle
  Makefile                  convenience targets (build, test, bench)
```

## Build and test

Nothing to install. Clone the repository into a folder named `router`, the name the dongle expects next to it. Then run any of these commands from the `router` folder:

```sh
git clone https://github.com/teklabsdigital/hw-gridrouter router
cd router

python3 -c "import gridrouter"          # compiles the library if needed (macOS: gridrouter/libgridrouter.dylib)
make                                    # same, via make
python3 tests/run_all.py                # all tests (~1-2 min); GRIDROUTER_TEST_SCALE=0.2 for a quick run
make test-kicad                         # the same tests in KiCad's Python (includes the pcbnew emit test)
python3 tests/bench.py                  # benchmark; add --full-oracle to also route the whole board in Python
python3 tests/bench.py --strict         # the same with the 4-direction (GEN_PCB_STRICT=1) tables
```

Commands written as `VAR=value command` are for macOS and Linux shells. In Windows PowerShell, set the variable first, for example:

```powershell
$env:GRIDROUTER_TEST_SCALE="0.2"; python tests\run_all.py
```

The PowerShell form has not been tried.

On macOS the build command is

```sh
cc -O2 -std=c99 -Wall -Wextra -dynamiclib -arch arm64 -arch x86_64 -o gridrouter/libgridrouter.dylib gridrouter/gridrouter.c
```

The build can be changed with environment variables:

- `CC`: the compiler.
- `GRIDROUTER_ARCHS`: the architectures, default `"arm64 x86_64"`.
- `GRIDROUTER_CFLAGS`: extra flags.
- `GRIDROUTER_LIB`: where the library goes. Set this if the router folder is
  read-only, or to use a library you built yourself.

The library is written to a temporary name and then renamed, so a running
process keeps its old copy. On Linux the same code builds a `.so`
(`-shared -fPIC`). See [Platforms](#platforms) for Windows and for what has
actually been run.

## Model (unchanged from the reference)

- **Grid.** `nx` by `ny` cells with pitch `cell` mm (0.1 mm in gen_pcb.py). A
  cell is `idx = j * nx + i`, at (i * cell, j * cell) mm. There are two signal
  layers: 0 = F.Cu, 1 = B.Cu.
- **Occupancy.** Each layer has two levels. `T` marks where the centre of a
  thin track may not go. `W` does the same for a wide track or a via. A cell
  holds 0 (free), a net number (only that net may use it) or `BLOCK` = 2^30.
  When a second net claims a cell, it becomes `BLOCK`. `novia` marks cells
  where a via centre is not allowed.
- **Search.** A* from a set of `(layer, idx)` sources to a set of targets. The
  state is (layer, cell, incoming direction). Costs:
  - each step costs `STEP[layer][dir]`;
  - a bend adds `TURN[incoming][outgoing]`, and reversing is forbidden;
  - a layer change adds `via_cost`, and only where `via_ok`;
  - an optional goal heuristic `max(0, a*max(dx,dy) + b*min(dx,dy) - c)`.

  Modes:
  - `max_cost`;
  - `via_goal`: stop at the first cell where a via fits, or at a target;
  - `relax`: the path may pass cells claimed by other nets since `snapshot()`,
    at `rip_cost` per cell, but never cells claimed before the snapshot.
- **commit.** Claims discs along a path. The radii depend on the track width
  plus `power` extra cells. It records grid edges, pad stubs and vias.
  `add_via` claims both layers and marks `novia` around the via.
- **Rip-up.** `route_net` grows a tree pad by pad. If a pad cannot be reached,
  it searches again with `relax=True`, rips up the nets that own the contested
  cells, and returns their names so the caller can queue them again.

## API

```python
import gridrouter
r = gridrouter.Router(nx, ny, cell=0.1, strict=False, ...)
```

### Constructor parameters

| Parameter | Default | Meaning (reference name) |
|---|---|---|
| `nx`, `ny` | required | grid size in cells (`NX`, `NY`) |
| `cell` | 0.1 | cell pitch in mm (`G`) |
| `strict` | False | True: 4 directions, wrong-way 3.0, diagonal 1.0, heuristic 10*(dx+dy). False: 8 directions, wrong-way 1.6, diagonal 1.2, octile heuristic (`STRICT`) |
| `dirs` | from `strict` | `[(dx, dy, base cost)]` (`DIRS`) |
| `wrong_way`, `diagonal` | from `strict` | multipliers used to build `step` (`WRONG_WAY`, `DIAGONAL`) |
| `step` | built | `[[cost per dir] for layer 0, layer 1]`; layer 0 prefers x, layer 1 prefers y (`STEP`) |
| `bend_45`, `bend_90` | 12, 40 | used to build `turn` (`BEND_45`, `BEND_90`) |
| `turn` | built | `(len(dirs)+1) x len(dirs)` table, `None` = forbidden; the last row is a path start or the cell after a via (`TURN`) |
| `via_cost` | 250 | (`VIA_COST`) |
| `rip_cost`, `max_rips` | 150, 400 | (`RIP_COST`, `MAX_RIPS`) |
| `heuristic` | from `strict` | `(a, b, c)`: h = max(0, a*max(dx,dy) + b*min(dx,dy) - c); strict (10, 10, 30), else (10, 4, 30) |
| `track`, `track_wide` | 0.20, 0.50 | widths written to segments (`TRACK`, `TRACK_PWR`) |
| `r_track`, `r_wide` | (3, 5), (5, 7) | T / W claim radii in cells for a thin / wide track (hard-coded in `commit`) |
| `r_via`, `r_via_space` | (5, 7), 8 | via claim radii; radius of the `novia` disc (hard-coded in `add_via`) |
| `pad_clear` | (0.30, 0.50) | T / W pad clearance in mm, plus `cell * power` (hard-coded in `add_pad`) |
| `pad_novia` | 0.6/2 + 0.1 | `novia` margin around pads in mm (`VIA_DIA / 2 + 0.1`) |
| `edge_margin` | (0.3+0.15, 0.3+0.35) | T / W keep-out from the grid border in mm (`RULES["edge"] + ...`); `None` disables it |

`Router.for_board(width_mm, height_mm, cell=0.1, **kw)` sizes the grid the way
gen_pcb.py does: `round(w / cell) + 1` cells.

### Attributes

- `NX`, `NY`, `NC`, `G`, `BLOCK`, `START`, `DIRS`, `STEP`, `TURN`, `VIA_COST`,
  `RIP_COST`, `MAX_RIPS`, `TRACK`, `TRACK_PWR`: the constants in use.
- `T[layer][idx]`, `W[layer][idx]`, `novia[idx]`: live views of the C grids
  (ctypes arrays). They can be read anywhere. Writing is supported before
  `snapshot()`, as gen_pcb.py does for `BUCK_AREA`.
- `hardT`, `hardW`, `hard_novia`: the grids as of the last `snapshot()`.
- `edges`, `stubs`, `vias`, `padcell`, `width`, `failed`, `log`, `rips`: same
  meaning and structure as in the reference.
- `ewidth`: a property returning a copy of the reference's
  `{(net, layer, edge): width}` dict.
- `last_cost`, `last_expanded`: cost and expanded-state count of the last search.
- `skip_joined_stubs` (**new**, default `False`): when `True`, `commit` adds no
  pad stub at a path end that lands on a grid edge this net already owns on
  that layer. Such a stub would only double the connection to the pad centre.
  The dongle sets it to `True`. It changes the output, so it is off by default
  to keep the oracle results. **It has no test.**

### Methods (same names and arguments as the reference unless marked)

| Method | Notes |
|---|---|
| `claim_rect(net, layers, x0, y0, x1, y1, d_t, d_w)` | permanent claim of a rectangle in mm grown by d_t / d_w |
| `claim_disc(net, layer, i, j, r_t, r_w)` | |
| `block_rect(x0, y0, x1, y1)` | BLOCK on both layers and `novia` |
| `block_cells(layer, i0, j0, i1, j1, levels=(0, 1))` | **new**: BLOCK a cell rectangle (inclusive), same as writing `T`/`W` directly but also correct after `snapshot()` |
| `add_pad(net, layers, cx, cy, x0, y0, x1, y1, power)` | |
| `via_ok(idx, net)` | |
| `snapshot()` | everything so far becomes permanent; clears `log`, `rips` |
| `rebuild(drop)` | removes the nets named in `drop` (keys of `log`); incremental, see below |
| `rip(net)` | **new**: remove one net number's post-snapshot claims and bookkeeping |
| `owners(level, path)` | **new**, replaces the reference's `own` dict: set of nets whose post-snapshot claims cover the path cells on level 0 (T) / 1 (W) |
| `path_free(level, path, net)` | **new**: every path cell on that level is 0 or `net` |
| `search(net, sources, targets, wide, goal=None, max_cost=None, via_goal=False, relax=False)` | returns `(path or None, new_via)`; sources / targets may also be `array.array("i")` from `encode()` |
| `encode(cells)` | **new**: `[(layer, idx)]` to an encoded array (`layer * NC + idx`) |
| `commit(net, path, wide, power, end_via=False)` | |
| `add_via(net, i, j, power)` | |
| `route_net(net, name, pads, wide, power)` | returns the ripped net names |
| `drop_to_plane(net, name, pad, joined, max_cost=600)` | `max_cost` is **new** (was the literal 600) |
| `routes()` | **new**, replaces `emit`: `(segments, vias)`. `segments = [(layer, x0, y0, x1, y1, width, net)]` and `vias = [(x, y, net)]`, in mm from cell (0, 0). Collinear edges are merged and spurs dropped, as `emit` did |
| `owned_count(net)`, `search_stats()` | diagnostics |

Module-level names:

- `BLOCK`, `RouterError` (raised on bad cells, cost overflow, or relax before
  snapshot).
- `build(force=False)`.
- `make_dirs(strict)`, `make_step(dirs, wrong_way, diagonal)`,
  `make_turn(dirs, bend_45, bend_90)`, `disc(r)`: the reference's table code.
- `gridrouter.kicad.emit(board, router, netinfo, names, pcbnew, origin=(0, 0), via_dia=0.6, via_drill=0.3)`:
  adds `PCB_TRACK` / `PCB_VIA` objects exactly as the reference `emit` did.
  It returns `(n_segments, n_vias)`.

C functions that are not part of the Python API but worth knowing:

- `rt_novia_rect(r, i0, j0, i1, j1)` marks every cell of an inclusive cell
  rectangle as "no via" permanently (clipped to the grid). `add_pad` and
  `block_rect` use it for their `novia` margins, instead of a Python loop over
  cells. It has no test of its own. It is exercised only indirectly: the
  oracle tests call `add_pad` and `block_rect` and compare the `novia` grids
  with the reference.

## How it works, and how it differs from the reference

### Same results

The C heap orders entries by `(f, d, layer, idx, dir)`, the same key the
reference pushes into `heapq`. Distances only ever strictly decrease, so no two
entries are equal. Any correct priority queue therefore pops exactly the same
sequence as the reference, which means the same paths, not just the same
costs. Ties are broken the same way and inconsistent heuristics behave the
same way. The tests check the path, cost, `new_via` and expanded-state count of
every search against the oracle.

### Speed

- **Search.** Flat arrays, with `dist` as int32 and a 1-byte predecessor code
  per state. Only touched states are reset after a search.
- **Priority queue.** One binary min-heap per f value, plus a cursor at the
  lowest non-empty f. A* pops f in nearly increasing order and many entries
  share an f, so each heap stays shallow. Pushes below the cursor are handled,
  so the order is exact for any costs. Keys with f >= 2^20 go to a single
  overflow binary heap.
- **Profiling notes.** Pop latency was most of the run time. Neither
  branch-free child selection nor a 4-ary heap helped measurably; the per-f
  buckets gave about 1.3x.

### Incremental rip-up

After `snapshot()`, every disc, path or via claim by a net is also recorded in
a per-cell linked list of owning nets and in a per-net list of cells.
`rip(net)` walks that net's list, unlinks it, and recomputes each cell from
the snapshot value and the remaining owners. This uses the same claim rule, so
the result is identical to the reference's "restore snapshot, replay every
other net". There is no copying and no replay. The tests check this against a
fresh router and against the oracle's `rebuild()`.

### Deviations

1. **Constants.** All constants are constructor parameters. Nothing is read
   from module globals or from `GEN_PCB_STRICT`; pass `strict=` instead.
2. **Output.** `emit(board, netinfo, names)` is replaced by `routes()` (plain
   data) and `gridrouter.kicad.emit(...)` (takes `pcbnew` as an argument).
   `routes()` does not modify the router. The reference `emit` pruned spurs
   from `self.edges` in place.
3. **No `snap` or `own`.** There is no `snap` attribute and no `own` dict. Use
   `owners()`, `hardT`, `hardW` and `hard_novia`. `rebuild(drop)` keeps its
   signature and result.
4. **Unlogged commits after `snapshot()`.** A `commit` / `add_via` made outside
   `route_net` after `snapshot()` belongs to its net. It is removed only when
   that net is ripped. In the reference such claims, and the `end_via` of any
   post-snapshot commit, silently vanished at the next rip-up of any net,
   because `rebuild` replays only `log` and the log has no `end_via` field.
   This looks unintended. When this library was written, it could not happen
   in the dongle's `route_all`, which called `drop_to_plane`, the fan-out
   commits and `EXTRA_VIAS` only before `snapshot()`. The dongle's code has
   changed since, so this has not been re-checked.
5. **Rectangle claims after `snapshot()`.** `claim_rect` / `add_pad` /
   `block_rect` / `block_cells` after `snapshot()` are permanent. In the
   reference they were lost at the next rip-up.
6. **Direct grid writes after `snapshot()`.** Direct writes to `T` / `W` /
   `novia` after `snapshot()` are not tracked. A later rip-up of a net that
   covers the cell recomputes it from the snapshot, so use `block_cells`
   instead. Before `snapshot()`, direct writes behave exactly as in the
   reference.
7. **`route_net` before `snapshot()`.** It skips the relaxed (rip-up)
   attempts. The reference raised `AttributeError`.
8. **`ewidth` is a copy.** It is stored per net internally; the property
   returns a fresh flat dict.
9. **Limits.**
   - At most 8192 cells per side, 16 directions, disc radius 255.
   - Path costs must stay below 2^31. A search that overflows raises
     `RouterError`; the reference used unbounded ints.
   - Cells passed in must lie inside the grid. The reference would silently
     wrap a negative index; this raises `RouterError`.
   - Costs must be non-negative, which the reference assumed too.

### Behaviour of the reference that is kept but worth knowing

- **The heuristic can be inadmissible.** It aims at the pad centre with only
  a 3-cell (30) allowance. For a target cell more than 3 cells from the
  centre (a long pad), it can overestimate, and A* may return a slightly
  costlier path than the optimum. This is reproduced exactly.
- **Ties favour smaller `d`.** Among equal f, `heapq` pops the smaller `d`,
  the state nearest the source. A* then explores a whole f-contour before
  reaching the goal. Preferring larger `d` would explore far fewer states,
  but it would change paths, so it was not done.
- **No vias through contested claims in `relax` mode.** `via_ok` still looks
  at the live W grid, so a via can never be placed through another net's
  rippable claims; only tracks can.
- **Contested sources are free.** A source cell claimed by another net after
  the snapshot is accepted in `relax` mode without `rip_cost`.
- **`width` is not restored on rip-up**, as in the reference. It only affects
  the per-net width record, which `route_all` does not use for output.

## Test and benchmark results

Measured on 2026-11-21 on an Apple Silicon Mac, Apple clang 21, `-O2`.

### Correctness

`tests/run_all.py` passes in both Python 3.13 (arm64) and KiCad's
Python 3.9.13 (arm64). A smaller run also passes on the x86_64 slice, under
`arch -x86_64`.

**Search vs oracle** (`test_search.py`). 250 random boards per direction
table. Each board is 25-90 x 20-70 cells with:

- pads of random size, layer and power;
- keep-outs and direct BLOCK writes;
- committed tracks and vias before and after the snapshot.

Each board gets 20 random searches covering all 32 combinations of
wide / goal / max_cost / via_goal / relax.

| Direction table | Searches | Found a path | Cost mismatches | Path mismatches | `new_via` / expanded-count mismatches |
|---|---|---|---|---|---|
| 8 directions | 5000 | 2704 | 0 | 0 | 0 |
| STRICT (4 directions) | 5000 | 2754 | 0 | 0 | 0 |

Every board also checks that all grids, edges, stubs, vias and widths, and
`via_ok` on sampled cells, are identical between C and oracle.

**Rip-up** (`test_ripup.py`):

- **A/B, 800 random cases.** Nets A and B are committed interleaved, with
  vias, end vias and overlapping claims; A is sometimes a net that also owns
  pads. Ripping A leaves the grids and bookkeeping identical to a fresh router
  that only committed B. Ripping B as well restores the snapshot. In 1 case in
  8 the oracle's own `rebuild()` agrees too.
- **A/B with real routing, 36 cases.** `route_net` routes both nets, then A is
  ripped. All 36 are identical to a fresh router.
- **End to end, 16 congested boards.** Each board has 6-14 nets plus a plane
  net dropped with `drop_to_plane`, routed with the `route_all` queue loop.
  That is 435 `route_net` calls and 202 rip-ups. After every call, the grids,
  bookkeeping, log order, ripped lists, failures, rip count and final
  segments / vias are identical to the oracle.

### Speed (`tests/bench.py`)

The benchmark uses a 501 x 361 grid (a 50 x 36 mm board, like the OBD-II dongle)
with 249 pads, 76 nets and an antenna keep-out. Half of the nets are routed
first as obstacles; the search timings use the first connection of each of
the other 38 nets.

| Test | Oracle (Python 3.13) | C | Speed-up |
|---|---|---|---|
| 38 thin searches | 12.88 s (232 k states/s) | 0.194 s (15.4 M states/s) | 66x |
| 38 wide searches | 11.66 s | 0.173 s | 67x |
| 38 relaxed searches | 2.62 s | 0.044 s | 60x |
| commit 77 paths | 0.051 s | 0.0011 s | 46x |
| rip-up of one net (`rebuild`) | 0.11 s | 0.00003 s | about 3400x |
| **whole board, `route_all` loop** | **84.9 s** | **1.27 s** | **67x**, identical result |
| whole board, `--strict` | 17.2 s | 0.36 s | 48x, identical result |

Strict searches are short, so fixed per-call overhead weighs more.

The reference's `rebuild` costs about 0.1 s here, and more with more nets
routed. On rip-up-heavy runs the overall gain should be higher than the 67x
of this lightly ripped benchmark.

### On the real board

On the OBD-II dongle's board, the old pure-Python router took 16 to 38 minutes
per run and often finished with 1 to 10 nets unrouted. With this library the
whole board routes in about 11 to 17 seconds, including rip-ups and the
dongle's clean-up passes. One run during development reported 779 segments, 138 vias
and 16 rip-ups in 16 s.

These numbers come from the development session logs. They are not a
controlled comparison: the board, its placement and the routing code all
changed between the two. Several of the nets that were left open by the old
router were caused by placement problems, which were found and fixed once
runs became fast.

## How the OBD-II dongle uses it

The dongle's board generator, `scripts/gen_pcb.py` in the dongle repository,
puts this folder on the import path and builds a router with its own grid,
spacings and costs. It looks for the router in a folder named `router` beside
the dongle repository, or in the folder named by the environment variable
`GRIDROUTER_PATH`. So clone this repository as `router` beside the dongle, or
set that variable (`REPO` is the dongle repository's root):

```python
sys.path.insert(0, os.environ.get("GRIDROUTER_PATH", os.path.join(REPO, "..", "router")))
import gridrouter
from gridrouter.kicad import emit as emit_routes

def make_router():
    return gridrouter.Router(
        NX, NY, G, strict=STRICT,
        wrong_way=WRONG_WAY, diagonal=DIAGONAL,
        via_cost=VIA_COST, rip_cost=150, max_rips=MAX_RIPS, bend_45=12, bend_90=40,
        track=TRACK, track_wide=TRACK_PWR,
        r_track=(3, 5), r_wide=(5, 7), r_via=(5, 7), r_via_space=8,
        pad_clear=(0.30, 0.50), pad_novia=VIA_DIA / 2 + 0.1,
        edge_margin=(RULES["edge"] + 0.15, RULES["edge"] + 0.35))
```

The dongle's defaults differ from this library's: wrong-way cost 2.0,
diagonal 1.5, via cost 400 and up to 600 rip-ups. Environment variables in
`gen_pcb.py` can change them. It also sets `skip_joined_stubs = True`.

Before `snapshot()`, `gen_pcb.py` writes the live `novia` grid directly to
route the CAN bus wires without vias. It saves the grid, fills it with the
value 2 using `ctypes.memset`, routes the two wires, then puts back every cell
that still holds 2. This relies on the rule above: direct grid writes are
exact before `snapshot()`.

Read the dongle's `scripts/gen_pcb.py` (`make_router` and `route_all`) for the current
details. This README does not track its line numbers.

## Platforms

- **macOS (Apple Silicon):** built and tested. Tests pass in Python 3.13 and
  in KiCad 10's bundled Python 3.9. A smaller run also passed on the x86_64
  slice under Rosetta.
- **Linux (untested):** the module builds `gridrouter/libgridrouter.so` with
  `$CC` (default `cc`) and `-shared -fPIC`. The Makefile does the same. This
  has never been run.
- **Windows (untested):** the module builds `gridrouter\gridrouter.dll`. It
  uses `$CC` if that is set, otherwise the first of `cl`, `gcc` and `clang`
  found on `PATH`. For `cl` (MSVC), run Python from an "x64 Native Tools
  Command Prompt" so the compiler is on `PATH`. The compiler must target the
  same architecture as your Python (64-bit for 64-bit). MSVC has no 128-bit
  integer type, so the C code then uses a two-part key that sorts in the same
  order. That code has been tested on macOS: the full test suite passes with
  it forced on (`GRIDROUTER_CFLAGS="-DRT_NO_INT128 -Werror"`). MSVC itself has
  not been tried. The DLL is copied into place, not renamed, so close other Python
  processes that have it loaded before a rebuild. You can also build the DLL
  yourself and set `GRIDROUTER_LIB` to its path. The Windows build itself
  has never been run.
  The Makefile is for macOS and Linux only.

## Where this came from

An AI assistant (Claude Code) wrote this library as part of an experiment in
AI-driven hardware design: the [Bluetooth OBD-II dongle](https://github.com/teklabsdigital/hw-obd2-dongle).
The dongle's first router was written in pure Python and took 16 to 38 minutes
per run. The human engineer asked for something faster. A separate AI agent
then built this library, with the Python router as its specification. The
tests compare every result with that original code, which is kept unchanged in
`tests/ref_router.py`.

The routing ideas are textbook ones: Lee / A* maze routing on a grid, rip-up
and reroute, and a preferred direction per layer. The code is original to this
project.

## Licence

MIT. See [LICENSE](LICENSE).

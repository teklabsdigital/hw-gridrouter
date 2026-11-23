# CLAUDE.md: instructions for AI assistants

## What this is

`gridrouter` is a two-layer PCB grid (maze) router.

- The core is in C: `gridrouter/gridrouter.c` and `gridrouter.h`. It holds the A* search, the occupancy grids, claims and rip-up.
- The wrapper is in Python: `gridrouter/__init__.py`, loaded with `ctypes`. It holds the `Router` class, the bookkeeping and the per-net driver.
- `gridrouter/kicad.py` turns the routes into KiCad objects. The caller passes in `pcbnew`.
- It uses only the Python standard library and a C compiler. The library is built automatically on first import, and again whenever the C source is newer.

It was written by an AI assistant to speed up the board generator of the Bluetooth OBD-II dongle (https://github.com/teklabsdigital/hw-obd2-dongle). That generator is its main user. See `README.md` for the full API and the model.

## Build and test

```sh
python3 -c "import gridrouter"     # builds gridrouter/libgridrouter.dylib (.so on Linux, gridrouter.dll on Windows) if needed
python3 tests/run_all.py           # all tests, about 1 to 2 minutes
GRIDROUTER_TEST_SCALE=0.2 python3 tests/run_all.py    # quick run (macOS / Linux shell)
python3 tests/bench.py             # timing, C against the Python reference
make test-kicad                    # tests in KiCad's Python, including the pcbnew emit test (macOS path in the Makefile)
```

- `VAR=value command` is the macOS / Linux shell form. In Windows PowerShell (untested): `$env:GRIDROUTER_TEST_SCALE="0.2"; python tests\run_all.py`.
- `GRIDROUTER_LIB` sets where the built library goes. Use it if the folder is read-only, or to keep build output out of the source tree.
- `test_kicad_emit.py` is skipped outside KiCad's Python.

## How it is tested: equivalence with the oracle

`tests/ref_router.py` is the original pure-Python router, copied unchanged. Only its constants can be set from outside. It is the specification.

**The C router must give identical results to it:**

- the same paths, not just the same costs;
- the same `new_via` flags and expanded-state counts;
- the same grids, edges, stubs, vias and widths;
- the same rip-up sequence.

The tests check all of this on random boards (`tests/common.py`). The C priority queue pops in exactly the same order as Python's `heapq`. That is why the paths match, not only the costs.

Do not "fix" the oracle to match a change in C. If a change is meant to alter behaviour, make it optional and off by default, so the oracle tests still hold. `skip_joined_stubs` is an example. Then add a test for the new option.

## Rules for changes

1. Run `python3 tests/run_all.py` after every change. Run the full scale before you say the work is done, not only the quick run.
2. Keep the C and the Python wrapper in step. A change to a C function signature must also change:
   - its `ctypes` signature table in `_load()` in `__init__.py`;
   - the declaration in `gridrouter.h`.
3. Keep `ref_router.py` unchanged.
4. Every new option or function needs a test. Two parts have no test today:
   - `skip_joined_stubs` has no test at all;
   - `rt_novia_rect` is tested only indirectly, through `add_pad` and `block_rect`.
5. Document deviations from the reference in the README's "Deviations" list.
6. Do not add dependencies. The library must stay standard-library Python plus a C99 compiler.
7. The owner's standing policy for any tool or package added to this project: ask the user first, pin an exact version, and do not upgrade by reflex.

## How the dongle uses it

The dongle's board generator, `scripts/gen_pcb.py` in the dongle repository:

- imports the router from a folder named `router` beside the dongle repository, or from `GRIDROUTER_PATH` if that is set;
- builds it with `make_router()`, using its own costs: wrong-way 2.0, diagonal 1.5, via cost 400, up to 600 rip-ups;
- sets `skip_joined_stubs = True`;
- writes the live `novia` grid directly before `snapshot()` to route the CAN wires without vias;
- writes the result with `gridrouter.kicad.emit`.

A change here can change the dongle's board. After any change that affects behaviour, regenerate the dongle board and check it: DRC 0 violations and 0 unconnected, its layout checks pass, and the via count and track length are about the same as before.

## What is untested

- **Linux:** the `.so` build has not been run.
- **Windows:** the build path in `_build_windows()` has not been run. It uses `$CC`, or else the first of `cl`, `gcc` and `clang` on `PATH`, and writes `gridrouter.dll`. Without a 128-bit integer type (MSVC) the C code uses a two-part heap key. That branch has been tested on macOS: the full test suite passes with `GRIDROUTER_CFLAGS="-DRT_NO_INT128 -Werror"`. Re-run that after any change to the heap code. MSVC itself has not been tried.
- Only macOS on Apple Silicon has been used, in Python 3.13 and in KiCad 10's Python 3.9. A smaller test run also passed under Rosetta (x86_64).
- `skip_joined_stubs`, as above.
- Limits: at most 8192 cells per side, 16 directions and disc radius 255. Path costs must stay below 2^31.

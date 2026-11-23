# CLAUDE.md: instructions for AI assistants

## What this is

`gridrouter` is a two-layer PCB grid (maze) router.

- The core is in C: `gridrouter/gridrouter.c` and `gridrouter.h`. It holds the A* search, the occupancy grids, claims and rip-up.
- The wrapper is in Python: `gridrouter/__init__.py`, loaded with `ctypes`. It holds the `Router` class, the bookkeeping and the per-net driver.
- `gridrouter/kicad.py` turns the routes into KiCad objects. The caller passes in `pcbnew`.
- Tracing (optional, off unless asked for; README "Tracing"):
  - `gridrouter/recorder.py`: `load_traced()` builds a second library with `-DRT_TRACE`; `TracedRouter` (a `Router` subclass on that library) and `Recorder` write a trace file.
  - `gridrouter/tracefile.py`: the trace file writer, reader, `check`, `replay` and the `python3 -m gridrouter.tracefile` command line.
  - `gridrouter/kicad_trace.py`: `dump_board` writes a `pcbnew` board into a trace. The caller passes in `pcbnew`.
  - `docs/TRACE_FORMAT.md`: the file format, the only contract between the recorder and the viewer.
  - `examples/record_bench.py`: records the benchmark board, or a small board, without KiCad.
  - `viewer/`: Visual Gridrouter, the native app that replays traces. It has its own rules: read `viewer/CLAUDE.md` before changing anything under `viewer/`.
- It uses only the Python standard library and a C compiler. The library is built automatically on first import, and again whenever the C source is newer.

It was written by an AI assistant to speed up the board generator of the Bluetooth OBD-II dongle (https://github.com/teklabsdigital/hw-obd2-dongle). That generator is its main user. See `README.md` for the full API and the model.

## Build and test

```sh
python3 -c "import gridrouter"     # builds gridrouter/libgridrouter.dylib (.so on Linux, gridrouter.dll on Windows) if needed
python3 tests/run_all.py           # all tests, about 1 to 2 minutes
GRIDROUTER_TEST_SCALE=0.2 python3 tests/run_all.py    # quick run (macOS / Linux shell)
python3 tests/bench.py             # timing, C against the Python reference
make test-kicad                    # tests in KiCad's Python, including the pcbnew emit test and test_kicad_trace.py (macOS path in the Makefile)
python3 tests/run_traced.py        # the suite again with TracedRouter(check=True) and a recorder; a separate, slower gate
```

- `VAR=value command` is the macOS / Linux shell form. In Windows PowerShell (untested): `$env:GRIDROUTER_TEST_SCALE="0.2"; python tests\run_all.py`.
- `GRIDROUTER_LIB` sets where the built library goes. Use it if the folder is read-only, or to keep build output out of the source tree.
- `test_kicad_emit.py` and the board tests of `test_kicad_trace.py` are skipped outside KiCad's Python.
- `run_all.py` includes `test_trace.py` (trace hook against the oracle, `Router` against `TracedRouter`, file format, reader checks). `run_traced.py` is not part of `run_all.py`.

### Full matrix for trace work

`build()` does not rebuild the default library when only `GRIDROUTER_CFLAGS` changes; it compares file times. So give each flag set a fresh `GRIDROUTER_LIB` path, in a directory that already exists (`build()` does not create it). The traced library goes beside it, under a name that includes a hash of the flags. After any change to the C core, `recorder.py` or `tracefile.py`, run at full scale:

```sh
python3 tests/run_all.py
python3 tests/run_traced.py
mkdir -p /tmp/gr_noint128
GRIDROUTER_LIB=/tmp/gr_noint128/libgridrouter.dylib GRIDROUTER_CFLAGS="-DRT_NO_INT128 -Werror" python3 tests/run_all.py
GRIDROUTER_LIB=/tmp/gr_noint128/libgridrouter.dylib GRIDROUTER_CFLAGS="-DRT_NO_INT128 -Werror" python3 tests/run_traced.py
make test-kicad
```

Use a new directory instead of `/tmp/gr_noint128` if it may hold a library built from older flags. On Linux the library name ends in `.so`.

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

   The trace hook follows the same rule with its own places:
   - all trace code in `gridrouter.c` and `gridrouter.h` stays inside `#ifdef RT_TRACE`, so the default build does not change;
   - the `ctypes` signatures of `rt_trace` and `rt_trace_count` are in `load_traced()` in `gridrouter/recorder.py`, not in `_load()`;
   - a change to the trace hook's behaviour or to what the recorder writes must follow `docs/TRACE_FORMAT.md`; a change to the format is a change to that document first.

   `recorder.py` reuses code from `__init__.py` instead of copying it, so a change there can break tracing without failing `run_all.py`'s other tests:
   - `load_traced()` runs `build()`, `_build_windows()` and `_load()` with their module globals replaced (a copy of the module's globals with `os` replaced by a copy whose `environ` has `-DRT_TRACE` added to `GRIDROUTER_CFLAGS`, `_lib_path` returning the traced path, and `_lib` a private cache). These functions must keep reading the environment through `os.environ`, the path through `_lib_path()`, and call `build()` and each other by their global names.
   - `TracedRouter.__init__` runs `Router.__init__` with `_load` replaced by `load_traced`. `Router.__init__` must keep getting its library by calling the global `_load()`. `TracedRouter.__init__` also binds the constructor's arguments by name to read `edge_margin`.
   - `TracedRouter` overrides `search`, `route_net`, `drop_to_plane`, `commit`, `add_via`, `rip`, `rebuild`, `snapshot`, `add_pad`, `claim_rect`, `block_rect`, `block_cells` and `claim_disc`, with the same signatures (`test_trace.py` checks this), and relies on which of them call which (spec section 5).

   After any change to `build()`, `_build_windows()`, `_load()`, `_lib_path()`, `Router.__init__` or those methods, run `tests/test_trace.py` and `tests/run_traced.py`.
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
- writes the result with `gridrouter.kicad.emit`;
- records a trace only when `GEN_PCB_TRACE_FILE` is set to an output path (on the dongle's `visual-router` branch): `make_router()` then builds a `TracedRouter`, a `Recorder` is attached, phases are marked and its direct grid writes are noted with `mark`, and `kicad_trace.dump_board` writes the board after placement ("placed") and after the final DRC ("final"). Unset, none of the trace code runs.

A change here can change the dongle's board. After any change that affects behaviour, regenerate the dongle board and check it: DRC 0 violations and 0 unconnected, its layout checks pass, and the via count and track length are about the same as before.

## What is untested

- **Linux:** the `.so` build has not been run.
- **Windows:** the build path in `_build_windows()` has not been run. It uses `$CC`, or else the first of `cl`, `gcc` and `clang` on `PATH`, and writes `gridrouter.dll`. Without a 128-bit integer type (MSVC) the C code uses a two-part heap key. That branch has been tested on macOS: the full test suite passes with `GRIDROUTER_CFLAGS="-DRT_NO_INT128 -Werror"`. Re-run that after any change to the heap code. MSVC itself has not been tried.
- Only macOS on Apple Silicon has been used, in Python 3.13 and in KiCad 10's Python 3.9. A smaller test run also passed under Rosetta (x86_64).
- `skip_joined_stubs`, as above.
- Limits: at most 8192 cells per side, 16 directions and disc radius 255. Path costs must stay below 2^31.
- **Tracing on Linux and Windows:** the traced library (`load_traced()`) has been built and run only on macOS (Apple Silicon), in Python 3.13 and KiCad 10's Python 3.9.

# Visual Gridrouter

A native desktop app that replays a `gridrouter` routing run from a trace
file (`.grtrace`): the board as KiCad draws it, the A* wavefront of each
search, paths committing, rip-ups, retries and failures. It only watches; it
never steers the router.

Status: **milestone M2**, the static board. The core reads `.grtrace` files
(M1) and turns a trace's `BRD` board into a display list in KiCad's default
colours and draw order; it also owns the camera. The app opens trace files
as read-only documents and draws the board with pan, zoom and an Appearance
panel. `rvrender` draws the same board to a PNG without a window. Router
copper, the wavefront and playback come in later milestones.

## Layout

```
viewer/
  core/                 shared core: portable C++17, standard library only
    include/rv.h        the C99 interface every client uses
    src/                implementation (api.cpp is the C boundary, trace.cpp the
                        reader, file.cpp the only platform-specific file I/O;
                        theme, display, scene and camera are the M2 board)
    tests/              self-contained tests (no framework), a C99 smoke test,
                        synth.cpp, a test-only trace writer built from the spec,
                        and synth_board.cpp, the synthetic stress board
  tools/                rvdump (trace inspector) and rvsynth (writes the synthetic traces)
  mac/                  macOS client: Swift 6 + AppKit, programmatic UI
    Sources/RVKit/      Swift wrapper over rv.h and the Core Graphics display-list renderer
    Sources/RouteViewer the app (TraceDocument, BoardView, AppearancePanel)
    Sources/rvrender/   headless renderer: board to PNG, PNG comparison, timing
    Tests/golden.sh     golden-image tests; Tests/golden/*.png are the expected images
    Info.plist          bundle metadata, .grtrace document type and NSDocumentClass
    bundle.sh           wraps the binary into a signed .app
  Package.swift         SwiftPM targets RVCore, RVKit, RouteViewer, rvrender
  Makefile              test, asan, dump, render, golden, app, selftest, run, clean
  check_additive.sh     checks the branch changed nothing outside the plan
```

The core owns everything that is not pixels or widgets, so the later Win32
and Qt6 clients reuse it unchanged. Clients are thin: they call `rv.h` and
draw what it returns.

## Build, run, test

From this folder, or from the repository root with `make -C viewer <target>`:

```sh
make test       # core tests + C99 header smoke test (clang++ -std=c++17 -Werror), then make golden
make asan       # core tests and smoke test under AddressSanitizer and UndefinedBehaviorSanitizer
make dump       # build/rvdump and build/rvsynth
make render     # rvrender; the stress board and the recorder sample as PNGs in build/render/
make golden     # render the golden cases, compare with mac/Tests/golden/*.png
make golden-update  # replace the golden PNGs (look at them before committing)
make app        # swift build -c release, then build/Visual Gridrouter.app (ad-hoc signed)
make selftest   # builds the app, starts it three times (no file, the sample, the stress board), exits
make run        # opens the app
make clean      # removes build/ and .build/
sh check_additive.sh   # read-only: fails if anything outside viewer/ changed beyond the allow-list
```

`make selftest` opens one window per run for a fraction of a second and exits
on its own (0 on success, 1 on a failed check, 2 if it does not finish in
10 s). With a file (`RouteViewer --selftest FILE`) it opens the trace as a
document without adding it to Open Recent, checks the window title, the
Appearance panel and the display list, renders the board offscreen, checks
that something besides the background was drawn, and checks that the view's
own drawing matches that render pixel for pixel (so the image is the right
way up).

## The app

- **Opening:** File > Open (⌘O), Open Recent, double-click in Finder, or drop
  a `.grtrace` file on the Dock icon or on any window. Each file is a
  read-only document (`TraceDocument`); the window title is the file name.
  With no trace open, the welcome window shows the board background and an
  Open button.
- **Board:** the latest `BRD` board of the trace by default; the board menu
  at the top of the Appearance panel picks another.
- **Pan:** drag, two-finger scroll, arrow keys.
- **Zoom:** pinch; scroll with ⌘, ⌥ or ⌃; a mouse wheel (zooms about the
  cursor, as in KiCad); `+` / `-` keys and View > Zoom In / Zoom Out
  (⌘= / ⌘-); `0` or ⌘0 zooms to fit.
- **Appearance panel:** layers in KiCad's order, each with its colour swatch
  and a visibility checkbox; object classes (zones, tracks, pads, vias, holes,
  text, footprint graphics, board graphics, rule areas, board outline).
  Masks, paste and courtyards start hidden, as in KiCad.
- **Status line:** cursor position in board millimetres and the zoom (100 % is
  zoom to fit).

## Drawing the board

The core builds a **display list** from a `BRD` chunk (`rv_get_display_list`):
batches of five primitives (filled polygons with holes, polylines, line
segments, discs, rings) in board millimetres, y down, each with a colour, a
layer, an object class and a draw-order key, sorted bottom to top. Clients
draw the batches in order; they do not decide colours or order. The list is
rebuilt only when the board, a visibility or the theme changes, never when
the camera moves; a visibility change only refilters. The camera (fit, zoom
about a point, pan, limits) is in the core too, so every client behaves the
same.

Filled polygons are oriented by the core (outline counter-clockwise on
screen, holes clockwise), so a client may fill a whole batch with the
non-zero rule and get KiCad's even-odd result per polygon.

Draw order, bottom to top: back technical layers (courtyard, fab, silkscreen,
paste, adhesive, mask), B.Cu, inner copper from the last to In1, F.Cu, front
technical layers (mask, adhesive, paste, silkscreen, fab, courtyard), user
layers, holes, Edge.Cuts. Within a copper layer: zones, rule areas,
graphics, text, tracks, pads, vias. KiCad itself draws the active copper
layer above everything; here silkscreen stays above copper so that it can
always be read.

### Theme

Colours are KiCad 10's built-in "KiCad Default" theme, read from KiCad
10.0.8's own `COLOR_SETTINGS` through its Python (`pcbnew`) on this machine:
every copper layer (F.Cu, In1–In30, B.Cu), every technical layer, Edge.Cuts,
Margin, the User layers, the background (0, 16, 35), via holes, via hole
walls, plated holes and non-plated holes. The zone fill opacity (0.6) is
KiCad's default from `.kicad_prl`. Not verified against KiCad, chosen to
match how KiCad looks: tracks, pads, vias and zones take their copper layer's
colour (KiCad 10's theme leaves those entries unset); pad hole walls are not
drawn; outlines of zones, rule areas and Edge.Cuts with no width use a
0.05 mm hairline; rule areas are hatched all over at 0.5 mm. There is no
theme file loader yet; the theme is built into the core (`core/src/theme.cpp`).

### Headless rendering and golden images

```sh
.build/release/rvrender FILE OUT.png [--size WxH] [--board N] [--layers a,b,c | --all-layers]
                                     [--hide-classes i,j] [--fit | --window x0,y0,x1,y1]
.build/release/rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
.build/release/rvrender --bench FILE [--size WxH] [--frames N]
```

`rvrender` uses the app's renderer (`RVKit.DisplayListRenderer`) at backing
scale 1 and writes an opaque sRGB PNG. `--board` counts from 0; `--window`
is a board rectangle in mm; `--hide-classes` takes `rv_object_class` numbers.

`make golden` renders six cases from synthetic traces only (`rvsynth` output,
never the recorder's `small.grtrace`, which may be regenerated) and compares
them with `mac/Tests/golden/*.png`: a pixel differs when a channel is off by
more than 48, and at most 0.2 % may differ. To regenerate after an intended
change: `make golden-update`, look at every PNG in `mac/Tests/golden/`, and
commit them with the change that caused them. The cases are listed in
`mac/Tests/golden.sh`.

### Performance

Core Graphics' cost grows with the pixel area each fill covers, so one
thread cannot redraw a full board at Retina size quickly (about 55 ms for
the stress board at 3200 × 2000 pixels). `DisplayListRenderer.renderImage`
splits the image into horizontal bands and draws them at the same time, one
`CGContext` per band over one shared pixel buffer. On an 18-core M5 Pro the
stress board (`rvsynth --stress`, 120 footprints, two zones with 400 holes
each, 220 routes; 43 batches and 85,074 points with default visibility)
redraws in about 5.4 ms median at 3200 × 2000 pixels, and about 6.0 ms with
the copy into a `CGImage`. Measure with
`.build/release/rvrender --bench build/render/stress.grtrace --size 3200x2000`.

`rvdump` reads a trace through `rv.h`, like a client:

```sh
build/rvdump FILE              # summary: version, complete or not, chunk counts, runs, nets, searches, totals
build/rvdump --chunks FILE     # one line per chunk
build/rvdump --search N FILE   # one search, with its first wavefront entries
build/rvsynth OUT              # write the synthetic test trace (--no-end, --truncate N)
build/rvsynth --stress OUT     # write the synthetic stress board ("placed" and "final" BRD)
build/rvdump --board N FILE    # a board's layers and display-list batches (N or "latest")
```

It exits 0 for a readable trace without errors, including a file that is
still being written ("incomplete"), 1 for an unreadable, foreign or corrupt
file, and 2 for bad usage.

The reader in brief: the chunk index and every small chunk are read when the
file is opened (or refreshed); `SRCH` wavefronts, `GRDS` grids and `BRD`
geometry are read on demand through small LRU caches. A corrupt chunk becomes
an error issue and is skipped; a short last chunk is "not written yet".

## Toolchain

Only the Apple Command Line Tools are needed. Xcode is not required and was
not used. Versions this was built and tested with:

| Tool | Version |
|---|---|
| Command Line Tools | 26.6 |
| Swift / SwiftPM | 6.3.3 (Swift 6 language mode, tools version 6.0) |
| Apple clang | 21.0.0 |
| GNU Make | 3.81 |
| macOS SDK | 26.5; deployment target macOS 14 |

Only Apple Silicon (arm64) has been built.

## Zero third-party dependencies

No packages, no SwiftPM dependencies, no Homebrew, no CMake. The core uses
the C++ standard library only; the Mac client uses system frameworks only.
Any new tool or dependency needs the owner's approval first (see
`CLAUDE.md`).

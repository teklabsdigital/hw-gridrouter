# Visual Gridrouter

A native desktop app that replays a `gridrouter` routing run from a trace
file (`.grtrace`): the board as KiCad draws it, the A* wavefront of each
search, paths committing, rip-ups, retries and failures. It only watches; it
never steers the router.

Status: **milestone M5**, inspect. The core reads `.grtrace` files (M1),
turns a trace's `BRD` board into a display list in KiCad's default colours
and draw order and owns the camera (M2), cuts the run into events on a
timeline, keeps the playback clock, and builds an overlay of the router's
copper, rip-up ghosts, failed pads and the current event's highlights at any
position (M3), draws each search's A* wavefront as images and paces the run
in five ways (M4), and now keeps a nets table, a selected net with a
KiCad-style highlight, hit testing and each net's attempt history (M5). The
app opens trace files as read-only documents and plays them: inspector
(nets table and history), board, caption, timeline strip, transport bar,
Playback menu, Appearance panel. `rvrender` draws any position to a PNG
without a window, with a net highlighted if asked.

## Layout

```
viewer/
  core/                 shared core: portable C++17, standard library only
    include/rv.h        the C99 interface every client uses
    src/                implementation (api.cpp is the C boundary, trace.cpp the
                        reader, file.cpp the only platform-specific file I/O;
                        theme, display, scene and camera are the M2 board;
                        timeline, routed, overlay, playback and session are M3;
                        wavefront is M4; inspect and hittest are M5)
    tests/              self-contained tests (no framework), a C99 smoke test,
                        synth.cpp, a test-only trace writer built from the spec,
                        synth_board.cpp, the synthetic stress board, and
                        synth_timeline.cpp, the synthetic routing run
  tools/                rvdump (trace inspector) and rvsynth (writes the synthetic traces)
  mac/                  macOS client: Swift 6 + AppKit, programmatic UI
    Sources/RVKit/      Swift wrapper over rv.h, the Core Graphics display-list
                        renderer and LayeredRenderer (cached planes for playback)
    Sources/RouteViewer the app (TraceDocument, BoardView, TimelineView, AppearancePanel,
                        InspectorPanel)
    Sources/rvrender/   headless renderer: any position to PNG, frames, PNG comparison,
                        timing, playback self-test
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
make render     # rvrender; the stress board, 12 frames of the synthetic run, the recorder sample, in build/render/
make golden     # render the golden cases, compare with mac/Tests/golden/*.png
make golden-update  # replace the golden PNGs (look at them before committing)
make app        # swift build -c release, then build/Visual Gridrouter.app (ad-hoc signed)
make selftest   # the app four times (no file, the sample, the stress board, the synthetic run),
                # then rvrender --selftest on the synthetic run and the sample
make run        # opens the app
make clean      # removes build/ and .build/
sh check_additive.sh   # read-only: fails if anything outside viewer/ changed beyond the allow-list
```

`make selftest` opens one window per run for a few seconds and exits on its
own (0 on success, 1 on a failed check, 2 if it does not finish in 10 s;
`RV_SELFTEST_TIMEOUT=SECONDS` allows longer, which the dongle trace needs,
about 11 s). With a file (`RouteViewer --selftest FILE`) it opens the trace
as a document without adding it to Open Recent, checks the window title, the
Appearance panel (12 object classes) and the display list, redraws every
plane, checks that something besides the background was drawn, and checks
that the layer tree as Core Animation itself draws it (`LayerSnapshot`: the
view's real layers rendered offscreen with `CARenderer`) matches a
single-pass render of every batch: no pixel may be off by more than 48 (so
the planes add up, in the right places, the right way up). With routing
events it then plays the run from the start through the window's own clock
path (large frames instead of waiting), checks that the display link runs
while playing and stops at the end, that the overlay changed, the playhead
moved and the caption is set, compares the layers again mid-run, steps one
event, and compares them in a search (part revealed, then settled). M5: it
selects a net through the nets table (the net of that search), checks the
table's selected row, the history, the timeline marks, the dim and highlight
slots, that "one net" pacing follows the selection, and compares the layers
again. Then the **gesture check**, with that net highlighted: 41 pans and
zooms through the view's own mouse, scroll and zoom handlers (small and
fractional drags, 17 of them vertical, zooms about off-centre points far in
and far out, zoom to fit), at backing scales 2 and 1, comparing the layer
tree with a fresh single-pass render after every step. Then clicks through
the view's own mouse handlers: a press with a 1-point wobble on a net
selects it (and its row in the table), a click on empty space clears, a
20-point drag that starts on a net pans by exactly the drag and selects
nothing, and the hover text names what is under the pointer. Last it plays
the run to the end in every pacing mode.

The comparisons rasterise in one band. (M4 believed Core Graphics drawing on
several threads was not repeatable; M5 found it is: see "Bands" under
Performance. The probe still prints how much the same picture drawn twice
differs: 0 px everywhere it has been run.)
In a wavefront image, a pixel whose centre falls exactly on a cell edge may
go to either cell (Core Animation and Core Graphics break the tie
differently); such pixels are counted apart and printed, everything else
must match.

`rvrender --selftest FILE` checks playback without a window: it plays the
run from start to end through the core clock in about 200 steps, seeks to
twelve positions in both directions (the same position reached from either
side must give the same picture), compares the stacked planes with a
single-pass render at 28 positions (nine of them in searches: part revealed,
just before settling, settled), checks the router vias drawn at the event
that applies the `ROUT` chunk against `ROUT`'s via count, plays the run
to the end in every pacing mode, and (M5) compares highlighted pictures
mid-search, at a rip-up and at the end, checks that clearing the selection
restores the picture exactly, and clicks a grid of 77 points (the topmost
hit with a net decides the selection, nothing clears it).

## The app

- **Opening:** File > Open (⌘O), Open Recent, double-click in Finder, or drop
  a `.grtrace` file on the Dock icon or on any window. Each file is a
  read-only document (`TraceDocument`); the window title is the file name.
  With no trace open, the welcome window shows the board background and an
  Open button.
- **Board:** by default the latest `BRD` board at or before the playback
  position ("Board at the playhead"), or before any board the router's
  outline (grid, pads and keep-outs of the router session); the board menu
  at the top of the Appearance panel picks a fixed one.
- **Playback:** a trace opens at its end, paused. The transport bar under
  the timeline: go to start, step back, play/pause, step forward, go to end,
  what a step moves to (event, search, commit, net, rip-up, failure, phase),
  slower/faster (0.25× to 64×) and the position ("event k of N · m:ss.s /
  3:00.0"). The caption above the timeline says the phase, the route_net in
  progress, what the event does, the running counts and the router's clock.
  Keys: space plays or pauses; ← / → step by the chosen kind; ⇧← / ⇧→ step
  by net; Home / End; ⌘[ / ⌘] slower / faster. The Playback menu has the
  same, plus Play Backward and Step By.
- **Pacing** (M4): the popup on the transport bar and Playback > Pacing:
  Fit length, Nets only, One net (the selected router net, M5; with none
  selected, the net of the event at the playhead), Skip failed, Real time.
  In One net, selecting another router net paces that net. Changing it keeps the event and the fraction of
  it; the timeline is redrawn with the new times.
- **Timeline:** phase bands with their labels on top, one bar per search
  (height grows with the states it expanded, log scale; red when it found
  nothing), rip-up ticks (orange) and failure ticks (red) at the bottom, and
  the playhead. Click or drag to scrub; playing pauses while dragging. The
  selected net's events (M5) are marked in light blue under the phase bands,
  and its search bars are drawn in that colour.
- **Inspector** (M5, left): a filter field and the **nets table**, one row
  per router session and net (click a column title to sort, again to
  reverse; selecting a row selects the net), the selection line, and the
  selected net's **history** (route_net calls with their searches, commits
  and returns, rip-ups and who caused them, failures); double-click a line to
  go to its event. While playing, the table follows the position four times
  a second.
- **Selecting on the board** (M5): a click selects the net of the topmost
  thing under the pointer (router copper, a pad, track, via or zone fill, a
  failed-pad marker, a wavefront cell); a click on nothing clears. A press
  that moves 3 points or more is a drag: it pans and selects nothing.
- **Pan:** drag, two-finger scroll, ↑ / ↓, ⌥ with any arrow key.
- **Zoom:** pinch; scroll with ⌘, ⌥ or ⌃; a mouse wheel (zooms about the
  cursor, as in KiCad); `+` / `-` keys and View > Zoom In / Zoom Out
  (⌘= / ⌘-); `0` or ⌘0 zooms to fit.
- **Appearance panel:** layers in KiCad's order, each with its colour swatch
  and a visibility checkbox; object classes (zones, tracks, pads, vias, holes,
  text, footprint graphics, board graphics, rule areas, board outline, routed
  copper, wavefront).
  Masks, paste and courtyards start hidden, as in KiCad.
- **Status line:** cursor position in board millimetres, the zoom (100 % is
  zoom to fit) and (M5) what is under the cursor, topmost first: "pad U4.14,
  net +3V3, F.Cu", "router copper 659, net /OSC_EN, B.Cu, committed at event
  2380", "wavefront of search 1207 (/PAIR_BTN): cell (i, j) on F.Cu,
  expanded k of n, cost c".

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

## Playback

All of this is in the core (`rv.h`, section M3); the app only draws and
forwards clicks.

- **Events.** One per chunk of these kinds: `RUN`, `SNAP`, `SRCH`, `CMIT`,
  `VIA` with added = 1, `RNB`, `RNE`, `DROP`, `MARK` with level 0 (a phase),
  `BRD` and `END`. An `RBLD` and the `RIP` chunks with ctx = 1 right after it
  are one rip-up event; any other `RIP` is a rip-up of its own. Every other
  chunk (`NETS`, `PAD`, `RECT`, `DISC`, `GRDS`, `ROUT`, notes, `VIA` with
  added = 0, corrupt or unknown chunks) takes effect with the next event;
  chunks after the last event belong to it.
- **Position and pacing.** The position is a time t in playback seconds.
  The whole run lasts 180 s at 1× ("fit length"); each event gets a share
  proportional to its weight: a search 1 + ln(1 + expanded states), a commit
  2, a via 1, a rip-up 3, a route_net begin 0.5, an end 0.5 (2 with failed
  pads), a drop 1, a phase 4, a snapshot 0.5, a session 1, a board 2, the
  end 1 (a search that found nothing weighs as at most 1000 states). An
  event's chunks take effect at its start; time inside an event only
  animates the wavefront. What is shown is a function of t alone, so
  playing, stepping and seeking in either direction agree. Other pacing
  modes below.
- **Router copper** alive at the event: every `CMIT` / `VIA` copper item made
  at or before it and not removed by a `RIP` at or before it, in its router
  session; drawn above the board's tracks in the layer's colour, as the
  commit's path (polylines through the cell centres; collinear points
  dropped) plus pad stubs, vias and drills.
- **Ghosts:** copper removed by a rip-up whose event started less than 1.5 s
  of playback before t, in its layer's colour mixed half-way to white, alpha
  0.75 × (1 − age / 1.5 s) in 16 steps, below the live copper.
- **Failed pads** (red ring and cross): each label of an `RNE`'s failed list
  and each `DROP` with ok = 0, from that event until the next `RNB` of the
  same route_net name or the next `RUN`. Placed at the `RNB` pad with that
  label; a drop at the centre of its drop_to_plane search's sources, else at
  the latest `RNB` pad of that name.
- **Highlights** of the current event: the commit just made (lightened), a
  new via (white ring), the search's path (white), sources (green) and
  targets (orange), the pads of the route_net in progress (white rings).
- **Board underneath, no double drawing:** following the position, the
  board drawn is the latest `BRD` at or before the event's last chunk, or
  without one the router outline. Router copper and ghosts are hidden when
  that board already holds the routing: it has at least one track or via,
  and its `BRD` chunk comes after the session's last copper change.

### The wavefront (M4)

- **What:** while a search event plays, the cells its A* search expanded
  appear in expansion order (the `SRCH` trace arrays; a `repeat_of` search
  shows the arrays of the search it repeats). Over the first 80 % of the
  event's time the first floor(n · fraction / 0.8) of its n stored entries
  show; for the rest ("settled") all n, and the overlay adds the path (found)
  or the targets in red (not found). Only the current event's search shows:
  when the event ends, its wavefront goes.
- **Colours:** per router layer, its theme colour C; r is a cell's cost so
  far over the largest cost among the stored entries (or its position in
  the order over n − 1 without costs). Colour lerp(C mixed 18 % toward
  white, C, r), alpha lerp(0.22, 0.08, r): a faint tint of the layer's own
  colour, a little stronger near the sources and fading toward the
  expensive edge, so pads, tracks and text stay readable through it (two
  layers over one area add up to at most about 0.39). The frontier, the
  latest max(1, ceil(n · 0.02)) cells revealed, is C mixed 50 % toward white
  at alpha 0.55: a thin, visible edge. (M5 toned these down from M4's 55 %,
  0.60 to 0.28, a 4 % frontier and 85 % at 0.95, which washed the board out
  under a large search.)
- **Images:** the core gives one image per router layer at grid resolution
  (one pixel per cell, premultiplied sRGB) with its board rectangle, bounds,
  a generation and a dirty rectangle; moving within one search changes only
  the new cells. The app draws each as one more plane, under that layer's
  copper, scaled by Core Animation with nearest-neighbour sampling (one cell,
  one flat square at any zoom); a camera change only moves its frame. Hidden
  with the Wavefront class or its layer.
- **Pacing** (`rv_set_pacing`): *fit length* (the default; the run lasts
  180 s at 1×), *nets only* (searches get no time: copper appears commit by
  commit, no wavefront), *one net* (only the events of one router net get
  time), *skip failed* (searches that found nothing get no time), *real
  time* (each event lasts the router's own time since the event before,
  × 10 by default; a search that found nothing at most 0.010 s of router
  time). It is a setting of the session, kept when another trace is opened.

## Inspecting (M5)

Everything here is in the core (`rv.h`, section M5): the rows, their order,
every cell's text, the selection, the highlight's colours, hit testing and
the history lines. The app shows them and forwards clicks.

- **Nets table** (`rv_nets_get`, `rv_nets_row`, `rv_nets_cell`): one row per
  (router session, router net) that any chunk is about, plus a row without a
  session for each net that `NETS` names but no chunk mentions (the dongle's
  unconnected pins). Totals are a pure function of the file: route_net
  calls (`RNB`), searches, found, not found, commits, vias (those of its
  `CMIT`s and `VIA`s with added = 1), rip-ups (rip-up events that removed at
  least one of its copper items), failed pads (`RNE` labels and failed
  `DROP`s), states expanded, router seconds (each route_net call's
  t(`RNE`) − t(`RNB`), an unfinished call to the session's last chunk, plus
  the searches outside route_net), first and last event. The position's
  columns: copper alive, failures shown, and the **state**, the first of:
  *routing* (a route_net call of the net is in progress), *failed* (a failed
  pad of it is shown), *ripped* (the last change to its copper removed
  some), *complete* (its latest route_net call returned status 0 with no
  failed pads, and none of its copper was removed since), *routed* (copper
  alive), *not started*. A row of an earlier session shows that session's
  end. On the dongle most nets end *routed*, not *complete*: the
  generator's later passes rip them up and commit their copper again
  outside route_net.
- **Sorting and filtering** (`rv_nets_set_view`): by any column, either
  direction; ties by session, then net code; names ASCII case-insensitively.
  The filter keeps names containing its text, ASCII case-insensitively.
- **Selection** (`rv_select_net`, `rv_select_name`, `rv_select_clear`,
  `rv_click`): at most one net. **Names match nets:** a router net (`NETS`)
  and a board net (a `BRD`'s net table) are the same net when their names
  are equal, exactly; codes are never compared (KiCad's and the router's
  numbering differ). A router net without a name highlights no board item;
  a board item without a net (or with the net named "") is never
  highlighted and a click on it selects nothing. Selecting a name that no
  router net has (a board-only net) highlights its board items only; a name
  that nothing has dims everything. The selection survives seeking,
  pacing changes and `rv_refresh`; opening or closing a trace clears it.
- **Highlight**, KiCad's net highlight: two overlay slots, both always
  present and empty without a selection (so drawing without one is
  unchanged). A **dim** slot (order rank 955, above Edge.Cuts) holds one
  filled rectangle in the background colour at alpha 0.65 over everything
  drawn (the board's content and the router grid, 2 mm beyond); a
  **highlight** slot (ranks 960 to 969) holds copies of the net's copper:
  the board's pads on copper layers, zone fills, tracks and vias with their
  holes, and the router's copper, vias, drills and ghosts, each in its
  colour mixed 30 % toward white with its alpha kept (holes keep their
  colours), each shown only when what it copies is shown. They keep the
  layers' order (B.Cu lowest, then inner layers, then F.Cu; holes above
  all copper) and within a layer the sub-slot order. Markers (failed pads,
  the current event) stay on top, undimmed. **Deviation from KiCad:** the
  highlighted items are drawn above every layer's dimmed content, not each
  in its own layer's place; a selected B.Cu track shows through an
  unselected F.Cu pad above it. Selecting changes no board slab, so it costs
  one or two slot planes.
- **Hit testing** (`rv_hit_test`, `rv_hit_get`, `rv_hover_status`): what is
  drawn under a view point, topmost first: failed-pad markers (always on
  top), then router copper, pads, tracks and vias by draw order from the
  top, then zone fills and wavefront cells by draw order from the top; ties:
  the item written later, then the nearer. A shape is hit within 3 view
  points of its edge (`RV_PICK_POINTS`), a zone fill or wavefront cell only
  from inside. Only what is shown counts: classes, layers, router copper
  hidden by a board that holds the routing, cells not yet revealed. Pads
  carry their footprint reference and number (the nearest `PADINFO` of the
  footprint), router copper its copper id and the event that committed it,
  wavefront cells their layer, cell, expansion index and cost. Boards are
  indexed by bounding box; on the dongle a hit test takes 0.003 ms (mid-run)
  to 0.034 ms (final board) median over 2000 random points.
- **History** (`rv_net_history`, `rv_history_get`, `rv_net_events`): the
  chunks of a router net in file order, as lines with a depth: the session;
  each route_net call ("route_net 43/1023 /EN, 5 pads: ok"), inside it its
  searches (result, cost, path cells, states, seconds, "repeats search N"),
  commits, rebuilds ("rebuild: rips up A, B") and what it returned; outside
  route_net, commits, vias and drops; and the rip-ups that removed its
  copper, with their cause: the route_net of another net or its own (the
  `RIP`'s route_id), drop_to_plane, or a direct rip. Each line has the
  event to go to. `rv_net_events` are those events, for the timeline marks.

### Drawing it: planes

The overlay is a second display list (`rv_get_overlay`) whose batches go
between the board's by draw order. The core reports **slots**: ranges of the
order axis that hold overlay batches and no board batch (each router layer's
ghosts, copper and vias; router drills; the selection's dim and highlight,
M5; markers on top), each with a generation that changes only when its
batches do. `RVKit.LayeredRenderer`
keeps one cached bitmap per **plane**: the board batches between two slots
(a static slab, redrawn only when the board or the camera changes) and each
slot (redrawn when its generation changes, over its batches' bounds only).
`BoardView` gives each plane a Core Animation layer, so a playback frame
re-rasterises one to three slot planes and swaps their contents; the board
is never redrawn while playing. A wavefront slot (M4, under each router
layer's copper) is one more plane whose image is the core's grid-sized
wavefront image, never rasterised. Every plane but the bottom one covers
only the bounds of its batches (M5: static slabs too, M4 cropped only
slots), and a plane that is one solid rectangle (the dim) is a 1 × 1 image
stretched over it.

The root layer is geometry-flipped (y down, as AppKit makes the hosted root
of a flipped view); plane frames are placed in the root's actual geometry.
M3 assumed a y-up root and placed every cropped plane mirrored about the
view's middle, so router copper slid against the board while dragging or
zooming (fixed in M4; the gesture check above fails on the old formula).

On the dongle trace (8006 events, 1023 route_nets, 1254 rip-ups) at
3200 × 2000 pixels on an 18-core M5 Pro, a playback frame at 8× with a net
selected takes 1.55 ms median, 2.6 ms at the 99th percentile and
3.3 ms at worst (clock, overlay copy, wavefront images and the planes that
changed; M4: 1.5, 3.2 and about 11 ms; M3 without the wavefront: 1.5, 2.5
and 6.2 ms); a full redraw of every plane (a pan or zoom) 4.5 to 8.1 ms. Selecting or
clearing a net and showing it takes 0.3 to 0.6 ms.
M4's 11 ms frame was the first frame of the router session: the dongle
writes its "placed" board before the `RUN`, the overlay had no router slots
at that board's event, so the session's first event changed the slot
structure and every plane was drawn again. The core now gives an event
before the first `RUN` the first session's (empty) router slots. The
slowest frame left is the one that brings the "final" board (a full redraw,
like a pan). Seeking to any position, wavefront images included,
takes 0.06 ms median (`rvdump --timeline --bench 2000`); the worst, into the
largest search (148,027 entries), about 0.75 ms the first time and 0.48 ms
after. Each redraw gets a new bitmap (the old one stays on
screen until its layer takes the new one); bitmaps are anonymous mappings,
and up to 96 MB of freed ones are kept for reuse (`BitmapMemory`), which
keeps the frame times of malloc without its cache of several hundred
megabytes. Peak memory footprint of that run: 354 MB (354 MB with a net
selected), of which the plane bitmaps at the end are 122 MB (123 MB
selected; M4: 397 and 154 MB, 422 and 179 MB selected; M3: 336 and 102 MB;
the peak varies by about 30 MB from run to run). The wavefront
slots cut the board into six static slabs; cropping all but the bottom one
to their content saves about a quarter each on the dongle, whose board fills
most of the view.
Measure with `.build/release/rvrender --bench-playback FILE --size 3200x2000 --speed 8`
(under `/usr/bin/time -l` for the memory).

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
                                     [--at FRACTION | --event N [--into F]] [--pacing PACING]
                                     [--select NET]
.build/release/rvrender FILE OUTDIR --frames N [options as above]
.build/release/rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
.build/release/rvrender --bench FILE [--size WxH] [--frames N]
.build/release/rvrender --bench-playback FILE [--size WxH] [--speed S] [--frames N] [--pacing PACING]
                                            [--select NET]
.build/release/rvrender --selftest FILE [--size WxH]
.build/release/rvrender --repeat FILE [--size WxH] [--at FRACTION]
```

`--select NET` selects the net of that name (as a click on it does) before
drawing; with `--bench-playback` the run plays with it highlighted, and the
bench also times selecting and clearing it (the core, the planes, the
history, the timeline events and 40 rows of the table) at the start and the
end. The playback bench names its slowest dynamic frame and the plane
bitmaps' sizes. `--repeat` measures how repeatable the banded rasteriser is
(see "Bands").

PACING is `fit[:SECONDS]`, `nets`, `net:NET` (a router net), `skip-failed`
or `real[:SLOWDOWN]`, as for `rvdump --timeline`.

`rvrender` uses the app's renderer (`RVKit.LayeredRenderer`) at backing
scale 1 and writes an opaque sRGB PNG of the board and the overlay. Without
`--at` or `--event` the position is the end of the run, as after opening a
trace in the app. `--at` is a fraction of the playback time (0 to 1),
`--event` an event index from 0 (the caption's "event k of N" counts from
1); `--into F` goes F (0 to 1) of the way through that event, e.g. 0.4
for a search part revealed, 0.9 for one settled. It prints the position's
counts, caption, pacing and wavefront cells. `--frames N` writes
`frame_0000.png`… at N positions evenly from start to end. `--board` counts
from 0; `--window` is a board rectangle in mm; `--hide-classes` takes
`rv_object_class` numbers (10 is routed copper).

`make golden` renders fourteen cases from synthetic traces only (`rvsynth`
output, never the recorder's `small.grtrace`, which may be regenerated) and
compares them with `mac/Tests/golden/*.png`: a pixel differs when a channel
is off by more than 48, and at most 0.2 % may differ. Six are boards (M2);
three are events of the synthetic routing run (`rvsynth --timeline`): the
rebuild's rip-up with its ghost (event 16), the failed drop_to_plane with
two failed-pad markers (event 30), and a commit of the second router session
over the final board (event 40); three are wavefronts (M4) of that run: a
search 40 % through (event 17), the same search settled with its path, and
a failed search settled with its target in red (event 25); two are
selections (M5): /SIG highlighted at the end of the run, and at event 16
with its ghost highlighted. The cases are listed in `mac/Tests/golden.sh`.

To regenerate after an intended change to the drawing: run
`make golden-update` (it renders every case into `build/golden/` and copies
all of them over `mac/Tests/golden/`), open every PNG in
`mac/Tests/golden/` and check it shows what its comment in `golden.sh`
says, restore any that should not have changed (`git checkout --
mac/Tests/golden/NAME.png`), and commit them with the change that caused
them. To add a case, add a `golden NAME INPUT [options]` line, run
`make golden` (the new case fails with "no golden image"), look at
`build/golden/NAME.png`, and copy it to `mac/Tests/golden/`.

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

**Bands** (M5). The same picture drawn again with the same bands is
identical, bit for bit, whether the bands run on several threads or one
after another (`rvrender --repeat`: 0 pixels differ in 9 draws on the dongle
mid-run at 3200 × 2000, zoomed 4×). Drawn with another number of bands it
differs a little: 18 bands against 1 in 16,901 of 6.4 million pixels, by at
most 21 of 255, and two bands against one in 6,632 pixels spread over 1,210
rows, only 57 of them within 3 rows of the seam. So the difference is not
the threads and hardly the seams: Core Graphics anti-aliases a stroke a
little differently in a bitmap context whose origin is elsewhere. A
cropped slot plane's bands move when its bounds change, so a thin stroke
that stays inside it may change by up to about 20 levels when the plane is
redrawn: a possible faint flicker. Removing it would take bitmap contexts
fixed to the view (tiles at fixed view positions, crops in whole tiles);
not done in M5.

`rvdump` reads a trace through `rv.h`, like a client:

```sh
build/rvdump FILE              # summary: version, complete or not, chunk counts, runs, nets, searches, totals
build/rvdump --chunks FILE     # one line per chunk
build/rvdump --search N FILE   # one search, with its first wavefront entries
build/rvsynth OUT              # write the synthetic test trace (--no-end, --truncate N)
build/rvsynth --stress OUT     # write the synthetic stress board ("placed" and "final" BRD)
build/rvsynth --timeline OUT   # write the synthetic routing run (two sessions, rip-ups, failures)
build/rvdump --board N FILE    # a board's layers and display-list batches (N or "latest")
build/rvdump --timeline [--bench SEEKS] [--pacing PACING] FILE
                               # events by kind, phases, the state at 7 positions, the pacing,
                               # open time and seek timing (random seeks and the largest wavefront)
build/rvdump --nets [--at AT] [--sort COLUMN[:desc]] [--filter TEXT] FILE
                               # the nets table at a position (AT: 0..1 or end, the default),
                               # COLUMN a title in lower case with '-' for spaces (net, code, state, ...)
build/rvdump --net NAME [--at AT] FILE
                               # select a net: the highlight, its row, its history, its events
build/rvdump --hit X,Y [--at AT] [--bench N] FILE
                               # what is under board point (X, Y) mm in a fitted 1600 x 1000 view,
                               # the hover text, and the time of N hit tests at random points
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

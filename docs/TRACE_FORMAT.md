# gridrouter trace file format, version 1.0

A trace file records one routing run of `gridrouter.Router`: the router's
settings, every search with its A* wavefront, every copper change, every
rip-up, and optionally the KiCad board around it. It is the only contract
between the Python recorder (writer, `gridrouter/recorder.py` and
`gridrouter/tracefile.py`, using `struct`) and the native viewer's C++17
reader (`viewer/core`). Each side is implemented from this document alone.

- Name: "gridrouter trace". File extension `.grtrace`.
- Version: major 1, minor 0. Clarified twice on 2026-11-23 without a version
  change (see [16](#16-clarifications)).
- The words MUST, MUST NOT, SHOULD and MAY are normative.

Contents:

1. [Conventions](#1-conventions)
2. [File header](#2-file-header)
3. [Chunks](#3-chunks)
4. [Identifiers, contexts and enumerations](#4-identifiers-contexts-and-enumerations)
5. [What the recorder emits, and when](#5-what-the-recorder-emits-and-when)
6. [Order guarantees](#6-order-guarantees)
7. [Chunk reference](#7-chunk-reference)
8. [Copper state at any point (normative)](#8-copper-state-at-any-point-normative)
9. [Coordinates](#9-coordinates)
10. [Reader algorithm](#10-reader-algorithm)
11. [Versioning](#11-versioning)
12. [Sizes](#12-sizes)
13. [Worked example](#13-worked-example)
14. [What the format does not capture](#14-what-the-format-does-not-capture)
15. [Decisions and rationale](#15-decisions-and-rationale)
16. [Clarifications](#16-clarifications)
17. [Open points](#17-open-points)

---

## 1. Conventions

### 1.1 Byte order and scalars

- All multi-byte values are **little-endian**.
- Scalar types:

  | Type | Size | Meaning |
  |---|---|---|
  | `u8` | 1 | unsigned 8-bit |
  | `u16` | 2 | unsigned 16-bit |
  | `u32` | 4 | unsigned 32-bit |
  | `i32` | 4 | signed 32-bit, two's complement |
  | `i64` | 8 | signed 64-bit, two's complement |
  | `f64` | 8 | IEEE 754 binary64 |
  | `char[4]` | 4 | four ASCII bytes, no terminator |

- Readers MUST read every scalar with `memcpy` (or byte assembly) from the
  buffer, never by casting a pointer. The alignment rules below are a
  convenience (whole arrays can be copied in one go), not a licence to cast.
- Boolean-like `u32` fields hold 0 or 1. Readers treat any non-zero value as 1.
- "Reserved" fields MUST be written as 0 and MUST be ignored by readers.

### 1.2 Alignment

- The file header is 24 bytes and every chunk occupies a multiple of 8 bytes,
  so **every chunk header and every chunk payload starts at a file offset that
  is a multiple of 8**. Payload offsets in this document are relative to the
  payload start, so "payload offset is a multiple of 8" and "file offset is a
  multiple of 8" are the same thing.
- Every `u32` / `i32` field and array is at a payload offset that is a
  multiple of 4.
- Every `i64` / `f64` field is at a payload offset that is a multiple of 8.
  Where a variable-length part would break this, the layout either puts the
  fixed 8-byte fields first or contains an explicit `align8` (below).

### 1.3 Composite notations

- **`str`**: `u32 n` (number of UTF-8 bytes), then `n` bytes of UTF-8 text (no
  terminator), then 0 to 3 zero bytes so that the total size of the `str`
  (4 + n + padding) is a multiple of 4. Total size: `4 + ((n + 3) & ~3)`.
  An empty string is the 4 bytes `00 00 00 00`. Writers encode Python `str`
  with UTF-8 (`errors="replace"`); readers treat the bytes as opaque UTF-8 and
  MUST NOT assume a terminator or valid UTF-8.
  The padding is part of the `str` and is required, also when the `str` is the
  last field of a payload or record: a `str` whose `4 + ((n + 3) & ~3)` bytes
  run past the end of the payload (or record body) makes the chunk corrupt
  ([3.3](#33-general-chunk-rules)). Writers write the padding bytes as zero;
  readers do not check their contents.
- **`cell`**: a `u32` holding `layer * nc + idx`, where `nc = nx * ny` and
  `idx = j * nx + i` for grid column `i` and row `j`. `layer` is 0 (F.Cu) or 1
  (B.Cu). This is the router's own encoding (`Router.encode`). A valid cell is
  `< 2 * nc`.
- **`align8`**: 0 or 4 zero bytes, whichever makes the next field's payload
  offset a multiple of 8. Because every field before an `align8` ends on a
  4-byte boundary, a reader computes it as `if (off & 7) off += 4;`.
- **`X[n]`**: `n` consecutive values of type `X`, no separators.
- **`n × (…)`**: `n` consecutive records with the listed fields in order.
- **Bitmask of layers**: bit `k` set means router layer `k` (bit 0 = F.Cu,
  bit 1 = B.Cu).
- **Sentinels**: `NONE32 = 0xFFFFFFFF` for an absent `u32` id. `-1` for an
  absent `i32` / `i64` value where stated.

### 1.4 Units

- **Router-grid mm** (default for every `f64` length in router chunks): the
  router's own coordinate system. Cell `(i, j)` is at `(i * cell, j * cell)`
  mm. Pad centres, pad rectangles, stub ends and `routes()` output are already
  in these units, because that is what the caller passes to `Router`.
- **Board nm** (every `i32` coordinate in `BRD `): KiCad board coordinates in
  nanometres (KiCad internal units), y axis pointing down, as returned by
  `pcbnew` (`GetPosition()`, `GetStart()`, …).
- **Cells**: `i32` grid indices and radii in cells (`DISC`, `VIA `, `RUN `
  radii).
- **Costs**: `i32` / `i64` router cost units (the same integers the C core
  uses: `STEP`, `TURN`, `via_cost`, `rip_cost`).
- **Time**: `f64` seconds.

---

## 2. File header

24 bytes at file offset 0.

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | `char[8]` | magic | `"GRTRACE\0"` = `47 52 54 52 41 43 45 00` |
| 8 | `u16` | major | 1 |
| 10 | `u16` | minor | 0 |
| 12 | `u32` | flags | 0 (reserved; readers of 1.x ignore it) |
| 16 | `f64` | open_time | Unix time (seconds since 1970-01-01 UTC, `time.time()`) when the file was opened |

Python: `struct.pack("<8sHHId", b"GRTRACE\0", 1, 0, 0, time.time())`.

---

## 3. Chunks

After the header the file is a sequence of chunks, nothing else.

### 3.1 Chunk header (16 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `char[4]` | tag | chunk type, four ASCII bytes, e.g. `"SRCH"`, `"RUN "` (note the trailing spaces) |
| 4 | `u32` | len | payload length in bytes, **excluding** the padding |
| 8 | `f64` | t | seconds since the file was opened, measured with a monotonic clock (`time.perf_counter()` minus its value at open), taken **when the chunk is written** |

Python: `struct.pack("<4sId", tag, len(payload), t)`.

Then `len` payload bytes, then `pad = (8 - len % 8) % 8` zero bytes. The next
chunk starts at `chunk_offset + 16 + len + pad`.

- `t` is non-decreasing in file order. Because a chunk is written when its
  event has finished, `t` marks the end of the event. `SRCH` additionally
  carries `duration_s`, so a search ran from `t - duration_s` to `t`.
- The writer enforces the order: if the clock gives a value smaller than the
  previous chunk's `t`, it writes the previous chunk's `t` instead.
- `len` is a `u32`, so one chunk holds at most 4 GiB - 1 of payload.

### 3.2 Tags in version 1.0

| Tag | Content | Section |
|---|---|---|
| `RUN ` | router settings; starts a router session | [7.1](#71-run--router-settings) |
| `NETS` | router net number to name | [7.2](#72-nets-router-net-names) |
| `PAD ` | `add_pad` | [7.3](#73-pad--add_pad) |
| `RECT` | `claim_rect`, `block_rect`, `block_cells` | [7.4](#74-rect-claim_rect-block_rect-block_cells) |
| `DISC` | `claim_disc` | [7.5](#75-disc-claim_disc) |
| `VIA ` | standalone `add_via` | [7.6](#76-via--standalone-add_via) |
| `SNAP` | `snapshot` | [7.7](#77-snap-snapshot) |
| `GRDS` | full occupancy grids | [7.8](#78-grds-occupancy-grids) |
| `RNB ` | `route_net` begins | [7.9](#79-rnb--route_net-begins) |
| `RNE ` | `route_net` ends | [7.10](#710-rne--route_net-ends) |
| `SRCH` | one `search`, with its wavefront | [7.11](#711-srch-one-search) |
| `CMIT` | one `commit` | [7.12](#712-cmit-one-commit) |
| `RBLD` | one `rebuild` | [7.13](#713-rbld-one-rebuild) |
| `RIP ` | one `rip` | [7.14](#714-rip--one-rip) |
| `DROP` | one `drop_to_plane` | [7.15](#715-drop-drop_to_plane) |
| `MARK` | phase marker or note | [7.16](#716-mark-phase-or-note) |
| `ROUT` | `Router.routes()` output | [7.17](#717-rout-routes-output) |
| `BRD ` | KiCad board dump | [7.18](#718-brd--kicad-board) |
| `END ` | end of a complete file | [7.19](#719-end--end-of-file) |

### 3.3 General chunk rules

- **Unknown tags** MUST be skipped using `len`.
- **Trailing payload bytes**: a reader MUST ignore payload bytes after the
  last field it knows. A later minor version may append fields at the end of
  any chunk's payload (after all the 1.0 fields, including their
  variable-length parts).
- **Too-short payload**: if `len` is smaller than the fields a reader needs
  (fixed part, or a count that would run past `len`), the chunk is
  **corrupt**. The reader reports it and skips it using `len`.
- **Incomplete last chunk**: if fewer than 16 bytes remain after the last
  complete chunk, or `chunk_offset + 16 + len + pad` exceeds the file size,
  that chunk is **not written yet**. It is not an error. A viewer may be
  following a file that is still being written (see [10](#10-reader-algorithm)).
- **Writer flushing**: the writer builds header + payload + padding in memory,
  writes them with one `write` call, and flushes. It never writes a partial
  chunk deliberately, never rewrites bytes already written, and flushes only at
  chunk boundaries.
- **Tag characters**: every tag byte is printable ASCII (0x20 to 0x7E). A
  complete chunk header with any other tag byte means the file is corrupt from
  that offset on; the reader stops there and reports it.
- **Limits of framing.** The format has no checksum and no sync marker, so a
  reader cannot tell every corruption of a chunk header from a valid one:
  - a corrupt `len` that runs past the end of the file looks exactly like a
    chunk that is not written yet. Readers treat it as "not written yet"
    (above); the file then simply ends there, without `END `;
  - a corrupt `len` that is too small (or too large, but still inside the
    file) makes the reader take the wrong bytes as the next chunk header. The
    tag check above usually catches this at the next header, because payload
    bytes are rarely four printable ASCII bytes; it is not guaranteed to;
  - a corrupt byte inside a payload is found only if it breaks a count, a
    bound ([3.4](#34-value-bounds)) or a consistency rule
    ([10](#10-reader-algorithm)).

### 3.4 Value bounds

Every file a writer produces satisfies the bounds below. They are writer
guarantees. A reader MAY treat a chunk that violates one of them as a
**corrupt** chunk (report it and skip it, as for a too-short payload); a
reader is not required to check any of them.

| Chunk | Bound |
|---|---|
| `RUN ` | `0 < nx <= 8192` and `0 < ny <= 8192` (the router's limit, below), so every cell `< 2 * nc` fits in a `u32` |
| `RUN ` | `0 <= nd <= 16` |
| `RUN ` | `cell` finite and `> 0`; `origin_x`, `origin_y` finite |
| `SRCH` | every `path` and `trace_cell` entry `< 2 * nc`; `ntrace_total <= 2 * nc`; `ntrace <= ntrace_total` |
| `SRCH` | every `src` and `tgt` entry `< 2 * nc` when `error` is clear (the C core rejects a search with an invalid cell, so such a search always has `error` set) |
| `CMIT` | every `path` entry `< 2 * nc`; every stub `layer <= 1` |
| `DISC` | `layer` is 0 or 1 (the C core rejects other layers). `i`, `j` may lie outside the grid: the disc is clipped to it |
| `RECT` | `op <= 2` |
| `DROP` | `ok <= 2` |
| `GRDS` | `which <= 1`; `nc` equals the session's `nx * ny` ([7.8](#78-grds-occupancy-grids)) |
| `ROUT` | every segment `layer <= 1` |
| `BRD ` | see [7.18](#718-brd--kicad-board) |

Bounds that refer to `nx`, `ny` or `nc` use the `RUN ` of the chunk's session
([6](#6-order-guarantees) rule 3).

Values that are **not** bounded, because the router accepts them:

- **`src` and `tgt` of a `SRCH` with `error` set.** The writer stores them as
  passed; a negative cell becomes a large `u32`. Such a chunk is valid. A
  reader MUST NOT treat it as corrupt and does not report it; it keeps the
  search's `search_id` and the rest of its fields.
- **Via cells.** The C core does not reject an `add_via` whose `(i, j)` lies
  outside the grid: it clips the via's discs. So a `VIA ` (with `added = 1`
  or 0) or a `CMIT` via can hold an `(i, j)` outside `0 <= i < nx`,
  `0 <= j < ny`. For a `CMIT` via this happens when user code passes
  `commit` a path whose `(layer, idx)` pairs are not in canonical form
  (`idx` outside `0 .. nc - 1`) although the encoded cells are valid. Such a
  chunk is valid: it is not corrupt, a `VIA ` with `added = 1` takes its
  copper id ([4.1](#41-ids)) and the copper is alive as [8](#8-copper-state-at-any-point-normative)
  says. A reader MAY report it as a warning, and a viewer MAY leave the via
  undrawn.

The grid bound is the router's own: it rejects a grid with more than 8192
cells per side (`RT_MAXSIDE` in `gridrouter/gridrouter.h`), so no writer
produces one. A reader MAY reject a `RUN ` whose `nx` or `ny` is larger, as a
corrupt chunk; both readers do. A reader that sizes arrays by the grid can
then rely on this bound instead of trusting the file. The router also
supports (informative) at most 16 directions and disc radius 255.

Which bounds the existing readers check differs; neither checks every one
(both check the `RUN ` grid bound).
For example, `DISC` `i`, `j`, `RECT` `op`, `DROP` `ok` and `GRDS` `which` are
not checked by either reader; an out-of-range value there is shown as
unknown.

---

## 4. Identifiers, contexts and enumerations

### 4.1 Ids

Three independent counters. Each starts at 0 in a file and increases by 1 in
file order. **They are never reset**, not by a new `RUN `, not by `SNAP`.

| Id | Assigned by | Carried in |
|---|---|---|
| `search_id` | each `SRCH` chunk | `SRCH` |
| `route_id` | each `RNB ` chunk | `RNB `, `RNE `, and the `route_id` field of `SRCH`, `CMIT`, `VIA `, `RBLD`, `RIP ` |
| `copper_id` | each `CMIT` chunk, and each `VIA ` chunk with `added = 1` | `CMIT`, `VIA `, and the lists in `RIP ` |

So the `k`-th `SRCH` in the file has `search_id = k`, the `k`-th `RNB ` has
`route_id = k`. CMIT and VIA share the copper counter: the `k`-th chunk that is
either a `CMIT` or a `VIA ` with `added = 1` has `copper_id = k`. A `VIA ` with
`added = 0` created no copper; its `copper_id` is `NONE32` and it does not
advance the counter.

Readers SHOULD check these sequences and report a gap as corruption.

Checking the sequences:

- The expected id of a chunk is the one this section assigns it from its
  position in the file. A stored id (`search_id` in `SRCH`, `route_id` in
  `RNB `, `copper_id` in `CMIT` and in `VIA ` with `added = 1`) that differs
  from the expected id is an error, which the reader reports.
- A `VIA ` with `added = 0` and a `copper_id` other than `NONE32` is an error.
- A corrupt `SRCH`, `RNB ` or `CMIT` (one whose payload cannot be read,
  [3.3](#33-general-chunk-rules)) is still identified by its tag, so its
  position in the sequence is known. A reader MAY count it, so that it takes
  the next id and the following chunks keep their expected ids. Both
  existing readers do.
- A corrupt `VIA ` takes no copper id, because whether it has `added = 1` is
  unknown. If it did create copper, the next copper id in the file is one
  higher than the reader expects, and the reader reports that mismatch.
- **Resynchronising.** After a mismatch the reader reports one error and
  continues from the stored id: the next expected id is the stored id plus
  one. A gap in a sequence is therefore reported once, not at every later
  chunk, and two readers that count a corrupt chunk differently agree again
  from the next intact chunk of that sequence. A chunk keeps its stored id:
  `RIP ` lists and `repeat_of` name copper and searches by the ids stored in
  their chunks.

### 4.2 `ctx`: where a call happened

Used by `SRCH`, `CMIT`, `VIA `:

| Value | Meaning |
|---|---|
| 0 | direct call by the user of `Router` (not inside `route_net` or `drop_to_plane`) |
| 1 | inside `route_net` (the chunk's `route_id` is that call's) |
| 2 | inside `drop_to_plane` (`route_id = NONE32`) |

`RIP ` has its own `ctx`: 0 = direct call of `rip`, 1 = called by `rebuild`.

`route_id` in `SRCH`, `CMIT`, `VIA `, `RBLD`, `RIP ` is the `route_id` of the
`route_net` call active at the time, or `NONE32` if none is.

### 4.3 Router constants

- `BLOCK = 1 << 30 = 1073741824`. Grid value meaning "blocked for every net".
  Repeated in `RUN `.`block`.
- Router layer 0 = F.Cu, layer 1 = B.Cu. The `RUN ` chunk carries the
  canonical KiCad names of the two layers (`layer_name[2]`), normally
  `"F.Cu"` and `"B.Cu"`; a viewer maps router layer `k` to the `BRD ` layer
  with that name.
- Router net numbers are positive integers chosen by the user of `Router`.
  They are **not** KiCad net codes; see [7.2](#72-nets-router-net-names) and
  [7.18](#718-brd--kicad-board).

---

## 5. What the recorder emits, and when

The recorder is a `TracedRouter(gridrouter.Router)` subclass that overrides
these methods. The table is normative for the writer.

| Method | Chunk(s) | Emitted |
|---|---|---|
| `search` | `SRCH` | **always**, at any nesting depth, when the call returns or raises |
| `commit` | `CMIT` | **always**, at any nesting depth, when the call returns |
| `add_via` | `VIA ` | only when **not** inside `commit`. Inside `commit` the via is folded into that `CMIT` (listed only if newly created) |
| `rip` | `RIP ` | **always**, at any nesting depth, when the call returns |
| `rebuild` | `RBLD` | **always**, at entry (before the `RIP ` chunks it causes), including `rebuild([])` |
| `snapshot` | `SNAP`, then `GRDS` (`which = 1`) | always (never nested) |
| `route_net` | `RNB ` at entry, `RNE ` at exit (also if it raises) | always (never nested) |
| `drop_to_plane` | `DROP` at exit (also if it raises) | always (never nested) |
| `add_pad` | `PAD ` | only at the **outermost** level |
| `claim_rect`, `block_rect`, `block_cells` | `RECT` | only at the **outermost** level |
| `claim_disc` | `DISC` | only at the **outermost** level |

"Outermost" means no other overridden method of the same router is active on
the call stack. Checked against `gridrouter/__init__.py`:

- `add_pad` calls `claim_rect` → one `PAD `, no `RECT`.
- `block_rect` calls `claim_rect` → one `RECT` with `op = 1`, no `op = 0`.
- `block_cells`, `claim_disc` call the C core directly; nothing in `Router`
  calls them, so they are always outermost unless user code nests them.
- `route_net` calls `search`, `owners`, `rebuild` (→ `rip`), `path_free`,
  `commit` (→ `add_via`). It emits `SRCH`, `RBLD`, `RIP `, `CMIT`.
- `drop_to_plane` calls `search` and `commit` (→ `add_via`). It emits `SRCH`,
  `CMIT`.
- `commit` calls `add_via` for each layer change on the path and for
  `end_via`. These never produce `VIA ` chunks. With the current `Router`,
  `add_via` is reached only through `commit` or a direct user call, so a
  `VIA ` chunk always has `ctx = 0`.
- The constructor's edge-margin claims call the C core directly and happen
  before the recorder is attached. They are described by `RUN ` (see
  [7.1](#71-run--router-settings)), not by `RECT`.
- Read-only methods (`owners`, `path_free`, `via_ok`, `owned_count`,
  `search_stats`, `routes`, `encode`) emit nothing. `routes()` output is
  written only when the user asks the recorder for it (`ROUT`).

**Exceptions.** `SRCH` is written with flag `error` set; `RNE ` with
`status = 1`; `DROP` with `ok = 2`. For every other method, a call that raises
writes no chunk (see [14](#14-what-the-format-does-not-capture)), with one
exception: `RBLD` is written at entry, before `rebuild` does anything, so a
`rebuild` that raises leaves its `RBLD` in the file, followed by the `RIP `
chunks of the `rip` calls that returned before the exception (possibly fewer
than [6](#6-order-guarantees) rule 7 describes). An `RNE ` with `status = 1`
always has `nripped = 0` (the call returned no list); its `failed` list is
still filled ([7.10](#710-rne--route_net-ends)).

**Not router calls.** `NETS`, `GRDS` with `which = 0`, `MARK`, `ROUT`, `BRD `
and `END ` are written when the user of the recorder asks for them
(`Recorder.attach`, `grids`, `mark`, `phase`, `routes`, `dump_board`,
`close`).

---

## 6. Order guarantees

A writer MUST produce, and a reader MAY rely on, the following.

1. The file header, then chunks. `END ` is the last chunk of a complete file;
   nothing follows it.
2. `RUN ` precedes every chunk that refers to a router: `PAD `, `RECT`,
   `DISC`, `VIA `, `SNAP`, `GRDS`, `RNB `, `RNE `, `SRCH`, `CMIT`, `RBLD`,
   `RIP `, `DROP`, `ROUT`. `NETS`, `MARK` and `BRD ` may appear before the
   first `RUN `. A reader reports a router chunk before the first `RUN ` as
   an error and MAY treat it as a corrupt chunk; it belongs to no session.
   A corrupt `RUN ` (one the reader cannot parse) still starts a new
   session, as rule 3 says: the previous session's copper is gone, and the
   new session has no known grid. Every router chunk of that session is
   reported as an error and MAY be treated as a corrupt chunk; none of them
   changes the copper state. The next valid `RUN ` starts a usable session
   again. Ids keep counting through such a session ([4.1](#41-ids)).
3. A file may contain more than one `RUN `. Each `RUN ` starts a new router
   session (a new `Router` object was attached): all router state from the
   previous session (copper, grids, snapshot) is gone, and every later chunk
   refers to the latest `RUN `. `NETS` and `BRD ` are not reset by `RUN `.
   Ids are not reset (see [4.1](#41-ids)).
4. The recorder SHOULD write a `GRDS` with `which = 0` and label `"attach"`
   right after each `RUN `, so that the viewer knows the grids at the moment
   of attaching (pads and keep-outs claimed before attaching, direct grid
   writes, the edge margin). The recorder writes, for each `attach`: `RUN `,
   then `GRDS "attach"`, then a `NETS` if `attach` was given net names. No
   other chunk comes between them.
5. `SNAP` is immediately followed by `GRDS` with `which = 1`.
6. Between an `RNB ` and its `RNE ` (same `route_id`) there are only `SRCH`,
   `RBLD`, `RIP ` and `CMIT` chunks of that call (their `route_id` equals the
   `RNB `'s). `route_net` calls do not nest or overlap. For each pad the
   sequence is: 1 to 4 `SRCH` attempts in the order (`wide` argument, not
   relaxed), (thin, not relaxed), (`wide` argument, relaxed), (thin, relaxed),
   the relaxed ones only if the router has been snapshotted and while
   `Router.rips < MAX_RIPS`. "Snapshotted" means a `SNAP` earlier in the
   session, or a `snapshot()` made before the recorder attached, which the
   file does not show; so relaxed searches can occur in a session that has no
   `SNAP` chunk. After
   each relaxed `SRCH` that found a
   path, one `RBLD` followed by its `RIP ` chunks; then either the next `SRCH`
   attempt or one `CMIT` for that pad. A pad with no `CMIT` failed and is
   listed in the `RNE `.
7. The `RIP ` chunks caused by a `RBLD` follow it immediately, one per name in
   the `RBLD` whose `nets[k] != -1`, in the `RBLD`'s order, all with
   `ctx = 1` and the `RBLD`'s `route_id`. If `rebuild` raised, fewer `RIP `
   chunks follow ([5](#5-what-the-recorder-emits-and-when), Exceptions).
8. A `DROP` chunk is written when `drop_to_plane` returns. It is immediately
   preceded by the chunks that call emitted: its 1 or 2 `SRCH` (`ctx = 2`),
   and, on success, one `CMIT` (`ctx = 2`). These are contiguous: no other
   chunk is between them and the `DROP`.
9. The recorder SHOULD write `BRD ` with label `"placed"` before the first
   `RNB ` / `SRCH`, and `BRD ` with label `"final"` before `END `.
10. Chunk times `t` are non-decreasing in file order.

---

## 7. Chunk reference

Each section gives the payload layout. "Size" is the fixed part. Offsets are
payload offsets. `struct` formats are the little-endian Python formats of the
fixed parts (informative, they must agree with the tables).

### 7.1 `RUN ` router settings

Written by `Recorder.attach(router, …)`. Values are the router's at attach
time.

Fixed part: 168 bytes. `struct`: `"<17iI12d"`.

| Offset | Type | Field | Source in `Router` |
|---|---|---|---|
| 0 | `i32` | nx | `NX` |
| 4 | `i32` | ny | `NY` |
| 8 | `i32` | nd | `len(DIRS)` (4 or 8 normally, at most 16) |
| 12 | `i32` | block | `BLOCK` (1 << 30) |
| 16 | `i32` | via_cost | `VIA_COST` |
| 20 | `i32` | rip_cost | `RIP_COST` |
| 24 | `i32` | max_rips | `MAX_RIPS` |
| 28 | `i32` | h_max | `heuristic[0]` |
| 32 | `i32` | h_min | `heuristic[1]` |
| 36 | `i32` | h_off | `heuristic[2]` |
| 40 | `i32` | r_track[0] | T radius, thin track, cells |
| 44 | `i32` | r_track[1] | W radius, thin track |
| 48 | `i32` | r_wide[0] | T radius, wide track |
| 52 | `i32` | r_wide[1] | W radius, wide track |
| 56 | `i32` | r_via[0] | T radius, via |
| 60 | `i32` | r_via[1] | W radius, via |
| 64 | `i32` | r_via_space | `novia` disc radius around a via |
| 68 | `u32` | flags | bit 0 `strict`; bit 1 `skip_joined_stubs` (at attach time; see `CMIT` bit 2); bit 2 `has_edge_margin` (constructor got a non-`None` `edge_margin`); other bits 0 |
| 72 | `f64` | cell | `G`, cell pitch, mm |
| 80 | `f64` | origin_x | board position of cell (0, 0), mm, KiCad board coordinates (`Recorder.attach(origin=…)`, default 0) |
| 88 | `f64` | origin_y | same, y |
| 96 | `f64` | track | `TRACK`, thin track width, mm |
| 104 | `f64` | track_wide | `TRACK_PWR`, wide track width, mm |
| 112 | `f64` | pad_clear[0] | T pad clearance, mm |
| 120 | `f64` | pad_clear[1] | W pad clearance, mm |
| 128 | `f64` | pad_novia | `novia` margin around pads, mm |
| 136 | `f64` | edge_margin[0] | T edge margin, mm (0 if `has_edge_margin` is clear) |
| 144 | `f64` | edge_margin[1] | W edge margin, mm (0 if clear) |
| 152 | `f64` | via_dia | via diameter for drawing, mm (`Recorder.attach(via_dia=…)`, default 0.60, as `kicad.emit`) |
| 160 | `f64` | via_drill | via drill for drawing, mm (default 0.30) |

Variable part, starting at offset 168:

| Field | Type | Meaning |
|---|---|---|
| dirs | `i32[3 * nd]` | `DIRS` flattened: `dx, dy, base_cost` per direction |
| step | `i32[2 * nd]` | `STEP[0]` then `STEP[1]` (cost per direction on layer 0, layer 1) |
| turn | `i32[(nd + 1) * nd]` | `TURN` row-major; row `nd` is "path start / after a via"; `-1` = forbidden |
| producer | `str` | free text, e.g. `"gridrouter.recorder 1.0; Python 3.9.13"` |
| layer_name[0] | `str` | canonical KiCad name of router layer 0, normally `"F.Cu"` |
| layer_name[1] | `str` | normally `"B.Cu"` |

Edge margin (informative, from `Router.__init__`): when `has_edge_margin` is
set, for level `k` (0 = T, 1 = W) with `m = int(edge_margin[k] / cell + 0.999)`,
the cells in a frame `m` cells wide along all four grid borders are set to
`BLOCK` on both layers. These claims are part of the state before attaching;
the `GRDS "attach"` chunk shows them.

Bounds on `nx`, `ny`, `nd`, `cell` and the origin: [3.4](#34-value-bounds).

### 7.2 `NETS` router net names

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | n |
| 4 | `n × (i32 code, str name)` | entries |

`code` is a router net number (as passed to `search`, `commit`, …). May appear
more than once; a later entry for the same `code` replaces the earlier name.
Net names come from the user of the recorder; they let the viewer join router
nets with KiCad nets (by name).

### 7.3 `PAD ` add_pad

`add_pad(net, layers, cx, cy, x0, y0, x1, y1, power)`. Size 88 bytes.
`struct`: `"<iI10d"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | net | |
| 4 | `u32` | layers | bitmask of `layers` |
| 8 | `f64` | power | `power` as passed (used as a real multiplier: `cell * power`) |
| 16 | `f64` | cx | pad centre, router-grid mm |
| 24 | `f64` | cy | |
| 32 | `f64` | x0 | pad rectangle, router-grid mm |
| 40 | `f64` | y0 | |
| 48 | `f64` | x1 | |
| 56 | `f64` | y1 | |
| 64 | `f64` | d_t | T clearance used: `pad_clear[0] + cell * power` |
| 72 | `f64` | d_w | W clearance used: `pad_clear[1] + cell * power` |
| 80 | `f64` | d_novia | `novia` margin used: `pad_novia` |

The claimed cells follow `Router.claim_rect` with `d_t`, `d_w`; the `novia`
rectangle follows `Router.add_pad` with `d_novia`. A viewer that needs exact
cells uses the next `GRDS`.

### 7.4 `RECT` claim_rect, block_rect, block_cells

Size 64 bytes. `struct`: `"<IiII6d"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | op | 0 `claim_rect`, 1 `block_rect`, 2 `block_cells` |
| 4 | `i32` | net | `claim_rect`: the `net` argument (may be `BLOCK`). Ops 1, 2: `BLOCK` |
| 8 | `u32` | layers | layer bitmask. Op 1: 3. Op 2: `1 << layer` |
| 12 | `u32` | levels | bit 0 = T, bit 1 = W. Ops 0, 1: 3. Op 2: bitmask of the `levels` argument |
| 16 | `f64` | x0 | ops 0, 1: mm. Op 2: `i0` (an exact integer) |
| 24 | `f64` | y0 | op 2: `j0` |
| 32 | `f64` | x1 | op 2: `i1` (inclusive) |
| 40 | `f64` | y1 | op 2: `j1` (inclusive) |
| 48 | `f64` | d_t | op 0: `d_t`. Ops 1, 2: 0 |
| 56 | `f64` | d_w | op 0: `d_w`. Ops 1, 2: 0 |

Op 1 (`block_rect`) also marks a `novia` rectangle, see `Router.block_rect`.

### 7.5 `DISC` claim_disc

Size 24 bytes. `struct`: `"<6i"`. Fields in order: `i32 net, layer, i, j,
r_t, r_w` (offsets 0, 4, 8, 12, 16, 20). Radii in cells.

### 7.6 `VIA ` standalone add_via

`add_via(net, i, j, power)` called outside `commit`. Size 32 bytes.
`struct`: `"<IiiiiIII"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | copper_id | new copper id if `added = 1`, else `NONE32` |
| 4 | `i32` | net | |
| 8 | `i32` | i | cell column |
| 12 | `i32` | j | cell row |
| 16 | `i32` | power | `int(power)` (extra clearance, cells) |
| 20 | `u32` | added | 1 if a new via was created; 0 if the net already had a via at `(i, j)` (the call did nothing) |
| 24 | `u32` | ctx | see [4.2](#42-ctx-where-a-call-happened); 0 with the current `Router` |
| 28 | `u32` | route_id | `NONE32` unless inside `route_net` |

A via occupies both router layers. It is drawn at cell `(i, j)` with
`RUN `.`via_dia` / `via_drill`. `(i, j)` can lie outside the grid; the chunk
is still valid ([3.4](#34-value-bounds)).

### 7.7 `SNAP` snapshot

Payload: `u32 reserved` (4 bytes; the chunk is padded to 8). Immediately
followed by a `GRDS` with `which = 1`.

Meaning: everything committed so far becomes permanent (no later `rip` removes
it); `Router.log` is cleared and `Router.rips` is reset to 0.

### 7.8 `GRDS` occupancy grids

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | which | 0 = live grids (`T`, `W`, `novia`); 1 = the snapshot (`hardT`, `hardW`, `hard_novia`), written right after `SNAP` |
| 4 | `u32` | nc | `nx * ny` of the current `RUN ` (readers MUST check it) |
| 8 | `str` | label | e.g. `"attach"`, `"snapshot"`, or the user's label |
| A | `i32[nc]` | T0 | `T[0]`: thin-track level, layer 0 |
| A + 4nc | `i32[nc]` | T1 | `T[1]` |
| A + 8nc | `i32[nc]` | W0 | `W[0]`: wide-track / via level, layer 0 |
| A + 12nc | `i32[nc]` | W1 | `W[1]` |
| A + 16nc | `u8[nc]` | novia | non-zero = no via centre allowed in this cell |

`A = 8 + 4 + ((label_len + 3) & ~3)`. Grid values: 0 free, a router net
number (only that net may use the cell), or `BLOCK`. Total payload
`A + 17 * nc`. `novia` values other than 0 and 1 can occur (direct writes by
user code); treat any non-zero value as "forbidden".

"Readers MUST check `nc`" means: a `GRDS` whose `nc` differs from `nx * ny`
of its session's `RUN ` is a **corrupt** chunk. The reader reports it and
skips it ([3.3](#33-general-chunk-rules)); its grids are not used.

### 7.9 `RNB ` route_net begins

`route_net(net, name, pads, wide, power)` with
`pads = [(cx, cy, layers, cells, label, …), …]`. Written at entry.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | route_id | |
| 4 | `i32` | net | |
| 8 | `u32` | wide | |
| 12 | `i32` | power | `int(power)` |
| 16 | `u32` | npads | `len(pads)` |
| 20 | `u32` | reserved | 0 |
| 24 | `npads × (f64 cx, f64 cy, u32 layers, u32 ncells)` | pads | 24 bytes each: `cx`, `cy` router-grid mm; `layers` bitmask of `pad[2]`; `ncells = len(pad[3])` |
| … | `str` | name | `name` (the `Router.log` key) |
| … | `npads × str` | labels | `str(pad[4])` |

Pads are in argument order; `pads[0]` is the root of the tree. `struct` of
the fixed part `"<IiIiII"`, of each pad `"<ddII"`.

The record is written before `route_net` runs, so it cannot assume the pads
are well formed. If `len(pad[3])` fails (the cell collection has no length),
`ncells` is `NONE32` (0xFFFFFFFF, "unknown"). Likewise `cx`, `cy` are 0 if
they do not convert to numbers, `layers` is 0 if `pad[2]` is not a list of
layers, a label is the empty string if `str(pad[4])` fails, and `power` is 0
if `int(power)` fails.

### 7.10 `RNE ` route_net ends

Written when `route_net` returns or raises. Size of the fixed part 24 bytes.
`struct`: `"<IiiIII"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | route_id | same as the `RNB ` |
| 4 | `i32` | net | |
| 8 | `i32` | rips_after | `Router.rips` after the call |
| 12 | `u32` | nripped | length of the returned list of ripped names |
| 16 | `u32` | nfailed | number of pads of this call that failed |
| 20 | `u32` | status | 0 returned normally; 1 raised an exception (the lists then hold what was known) |
| 24 | `nripped × str` | ripped | the returned names, in order (may repeat a name) |
| … | `nfailed × str` | failed | labels of the failed pads (`Router.failed` entries with this `name` added by this call), in order |

`route_net` first removes every `Router.failed` entry with its `name` (also
one left by an earlier `drop_to_plane` with that name), then adds one per pad
it fails. The recorder writes the labels of all `Router.failed` entries with
this `name` when the call ends, which are exactly those this call added. With
`status = 1`, `nripped` is 0 and `failed` holds the pads that failed before
the exception.

### 7.11 `SRCH` one search

One per `search(net, sources, targets, wide, goal, max_cost, via_goal, relax)`
call, written when it returns (or raises).

Fixed part: 96 bytes. `struct`: `"<IIIiIiiiqqqdIIIIIIII"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | search_id | |
| 4 | `u32` | route_id | `NONE32` unless `ctx = 1` |
| 8 | `u32` | ctx | 0 direct, 1 `route_net`, 2 `drop_to_plane` |
| 12 | `i32` | net | |
| 16 | `u32` | flags | see below |
| 20 | `i32` | gi | goal column (0 if no goal) |
| 24 | `i32` | gj | goal row (0 if no goal) |
| 28 | `i32` | rips_at_call | `Router.rips` when the search started |
| 32 | `i64` | max_cost | `int(max_cost // 1)` as passed to the C core; `-1` if `has_max` is clear |
| 40 | `i64` | cost | path cost (`last_cost`); `-1` if not found |
| 48 | `i64` | expanded | expanded-state count (`last_expanded`); `-1` if unknown (error) |
| 56 | `f64` | duration_s | wall time of the `search` call, including the recorder's conversion of `sources` / `targets` to cells |
| 64 | `u32` | nsrc | number of source cells |
| 68 | `u32` | ntgt | number of target cells |
| 72 | `u32` | npath | path length in cells (0 if not found) |
| 76 | `u32` | ntrace | trace entries stored in this chunk |
| 80 | `u32` | ntrace_total | first expansions counted by the C core (`rt_trace_count`) |
| 84 | `u32` | repeat_of | `search_id` whose trace this search shares, or `NONE32` |
| 88 | `u32` | enc | trace encoding: 0 = raw (the only value in 1.0) |
| 92 | `u32` | reserved | 0 |

`flags`:

| Bit | Name | Meaning |
|---|---|---|
| 0 | wide | the `wide` argument |
| 1 | has_goal | a goal was given (`gi`, `gj` valid) |
| 2 | has_max | `max_cost` was given |
| 3 | via_goal | |
| 4 | relax | |
| 5 | found | a path was returned |
| 6 | new_via | the returned `new_via` (only with `via_goal`) |
| 7 | has_costs | the `trace_d` array is present |
| 8 | error | the call raised: `RouterError` from the C core (bad cell, cost overflow, relax before snapshot), or any other exception (see below); `found` is clear, `cost = -1`, `expanded = -1`; any trace present is the partial trace up to the error |
| 9 | has_trace | the wavefront was recorded. If clear, `ntrace = ntrace_total = 0` and the search has no wavefront (tracing was off), even if `expanded > 0` |
| 10-31 | | 0 |

How the recorder fills a `SRCH` when the arguments are unusual or the call
raises:

- `error` is set for **any** exception from `search`, not only
  `RouterError`: also `TypeError`, `ValueError`, `OverflowError` (or any
  other) raised while converting the arguments, before the C core runs.
- `has_trace` is set only if tracing was on and the C search actually ran:
  the call returned, or raised `RouterError` (which only the C core raises
  there). A search that raised `RouterError` because of an invalid argument
  has `has_trace` set and `ntrace = ntrace_total = 0`. A search that raised
  any other exception has `has_trace` clear.
- `has_costs` is set exactly when `has_trace` is set: the recorder always
  stores `trace_d`.
- `has_goal` is set only if `goal` is truthy and both `goal[0]`, `goal[1]`
  convert to integers; otherwise `gi = gj = 0`.
- `has_max` is set only if `max_cost` is not `None` and
  `int(max_cost // 1)` succeeds; otherwise `max_cost = -1`.
- If `sources` could not be converted to cells, `nsrc = 0`; if `targets`
  could not (or `sources` failed first), `ntgt = 0`. Such a search always has
  `error` set.

Variable part, starting at offset 96, in this order:

| Field | Type | Present |
|---|---|---|
| src | `cell[nsrc]` | always: sources as passed, in order (duplicates and inadmissible cells kept) |
| tgt | `cell[ntgt]` | always: targets as passed (a `set` is written in its iteration order) |
| path | `cell[npath]` | always: the returned path, source end first, target end last |
| trace_cell | `cell[ntrace]` | `enc = 0` |
| trace_d | `i32[ntrace]` | `enc = 0` and `has_costs` |

Payload size with `enc = 0`:
`96 + 4 * (nsrc + ntgt + npath + ntrace) + (has_costs ? 4 * ntrace : 0)`.

**Trace semantics.**

- The trace lists, in heap pop order, the **first** non-stale pop of each
  (layer, cell) during the search: `trace_cell[k] = layer * nc + idx` and
  `trace_d[k] = d`, the cost so far (A* g, not f) of that pop.
- Each cell appears at most once, so `ntrace_total <= 2 * nc`, and
  `ntrace_total <= expanded` (expanded counts states, which include the
  incoming direction).
- The final pop of the search is included if its cell is new: the target cell
  or via site that ended the search, or the pop whose `d > max_cost` stopped
  it. Sources that were not admissible are never popped and never appear.
- `trace_d` is not monotonic (A* pops by f = g + h).
- `ntrace < ntrace_total` only if the recorder's buffer was smaller than
  `ntrace_total`; the stored entries are then the first `ntrace` in pop order.
  The recorder allocates `2 * nc` entries, so in practice they are equal.

**Repeats.** When `repeat_of != NONE32`:

- `ntrace = 0` and no trace arrays are stored; the trace is that of search
  `repeat_of`, which is an earlier search in the same router session and is
  itself never a repeat.
- `has_trace`, `has_costs` and `ntrace_total` are copied from that search.
- The writer MUST set `repeat_of` only when the two traces are byte-identical
  (same `ntrace_total`, same `trace_cell` and, if present, `trace_d`). The
  recorder compares the arrays; it does not infer equality from equal
  arguments, because direct grid writes are invisible to it. The usual case is
  `route_net` called with `wide = False`: its first two attempts (thin, not
  relaxed) are identical, so a failed first attempt is repeated exactly.

**Consistency of the trace fields.** A writer MUST guarantee:

- if `has_trace` is clear: `ntrace = ntrace_total = 0`, `has_costs` clear,
  `repeat_of = NONE32`, and no trace arrays are stored;
- if `repeat_of != NONE32`: `ntrace = 0`; `has_trace`, `has_costs` and
  `ntrace_total` equal those of search `repeat_of`; and search `repeat_of` is
  an earlier `SRCH` of the same session, is not itself a repeat, has
  `has_trace` set, `enc = 0` and `ntrace > 0`.

A reader MAY report a violation as a warning; it is not a corrupt chunk (the
payload is still read with the sizes the counts give). A reader that shows
wavefronts then:

- ignores trace arrays stored without `has_trace` (no wavefront);
- shows no wavefront for a search whose `repeat_of` names a search that is
  not a valid target (a later search, itself a repeat, another session, or a
  search without a wavefront);
- uses the target's wavefront when the target is valid but the repeat's
  `ntrace`, `ntrace_total`, `has_trace` or `has_costs` differ.

**`enc != 0`** (reserved for later minor versions): only the trace arrays are
encoded. `src`, `tgt` and `path` stay raw and come first; the encoded trace
data runs from the end of `path` to the end of the payload. A reader that does
not know the `enc` value MUST still read the rest of the chunk and treat the
search as having no wavefront.

Every 1.x file written today has `enc = 0`; no other value is defined. A
reader bounds a `SRCH` with `enc != 0` by `len` like any chunk: it reads the
fixed part, `src`, `tgt` and `path`, and does not interpret the bytes after
`path`. Because the encoded data runs to the end of the payload, a `SRCH`
with `enc != 0` cannot also carry fields appended by a later minor version
([3.3](#33-general-chunk-rules)) that a reader could find; see
[Open points](#17-open-points).

### 7.12 `CMIT` one commit

`commit(net, path, wide, power, end_via)`, written when it returns.

Fixed part: 48 bytes. `struct`: `"<IiIiIIIIIId"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | copper_id | |
| 4 | `i32` | net | |
| 8 | `u32` | flags | bit 0 `wide`; bit 1 `end_via`; bit 2 `skip_joined_stubs` was true during this commit; other bits 0 |
| 12 | `i32` | power | `int(power)` |
| 16 | `u32` | ctx | 0 direct, 1 `route_net`, 2 `drop_to_plane` |
| 20 | `u32` | route_id | `NONE32` unless `ctx = 1` |
| 24 | `u32` | npath | |
| 28 | `u32` | nstub | pad stubs added by this commit |
| 32 | `u32` | nvia | vias newly created by this commit |
| 36 | `u32` | reserved | 0 |
| 40 | `f64` | width_mm | track width: `TRACK_PWR` if `wide` else `TRACK` |

Variable part, starting at offset 48:

| Field | Type | Meaning |
|---|---|---|
| stubs | `nstub × (u32 layer, u32 reserved, f64 x0, f64 y0, f64 x1, f64 y1, f64 w)` | 48 bytes each; router-grid mm; in the order `commit` appended them to `Router.stubs[net]` |
| vias | `nvia × (i32 i, i32 j)` | 8 bytes each; vias this commit created (its `add_via` calls that found no existing via of this net at `(i, j)`), in call order: layer changes along the path, then `end_via`. A via can lie outside the grid ([3.4](#34-value-bounds)) |
| path | `cell[npath]` | the committed path |

`struct` of a stub: `"<II5d"`; of a via: `"<ii"`.

A stub's `w` is the width the router's bookkeeping holds for that stub at
commit time (`Router._sw`, set by this `commit` to the track width). With the
current `Router` it therefore always equals `width_mm`. A later `rip` does not
restore `_sw` (see [14](#14-what-the-format-does-not-capture) item 8), so `w`
can differ from what `routes()` reports later.

How the viewer draws a `CMIT` (all of it belongs to `copper_id`):

- for each `k` in `1 .. npath-1` with `path[k-1]` and `path[k]` on the same
  layer: a round-capped segment between the two cell centres on that layer,
  width `width_mm`;
- consecutive cells on different layers are the same `idx`: a layer change.
  Its via is drawn only if listed in `vias` (otherwise the net already had a
  via there, from an earlier copper item);
- each stub as a segment of width `w` on `layer`;
- each listed via at its cell, size from `RUN `.

### 7.13 `RBLD` one rebuild

`rebuild(drop)`, written at entry, before the `RIP ` chunks it causes.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | route_id | the active `route_net`, or `NONE32` for a direct call |
| 4 | `u32` | n | `len(drop)` |
| 8 | `i32[n]` | nets | for each name: the net number of `Router.log[name][0]`, i.e. the net that will be ripped; `-1` if the name has no log entry (nothing is ripped for it) |
| … | `n × str` | names | `drop`, in order |

Inside `route_net`, every `RBLD` also marks one increment of `Router.rips`
(the increment happens just before `rebuild`), including `n = 0`.

### 7.14 `RIP ` one rip

`rip(net)`. Written when it returns. Fixed part 16 bytes, `struct` `"<iIII"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | net | |
| 4 | `u32` | ctx | 0 direct, 1 from `rebuild` |
| 8 | `u32` | route_id | the active `route_net`, or `NONE32` |
| 12 | `u32` | nremoved | |
| 16 | `u32[nremoved]` | removed | copper ids removed, ascending |

**Semantics.** `Router.rip(net)` resets the net to its state at the router's
last `snapshot()`: copper the net committed before that snapshot survives.
That snapshot may have been taken before the recorder attached; the file then
has no `SNAP` for it. So:

- If the router has been snapshotted (a `SNAP` earlier in the session, or a
  `snapshot()` before attaching), `removed` lists **exactly** the copper ids
  (`CMIT`, and `VIA ` with `added = 1`) of that net that were created in the
  current session after its last `SNAP` (after its `RUN ` if it has no
  `SNAP`) and are not listed in an earlier `RIP `. Copper the net committed
  between a snapshot before attaching and the attach is removed too, but it
  has no copper id and is not listed
  ([14](#14-what-the-format-does-not-capture) item 2).
- If the router has never been snapshotted, `rip` changes nothing and
  `nremoved = 0`.

A reader cannot tell the two cases apart before the first `SNAP` of a
session. There, both are valid and a reader reports neither: `removed` is
either empty (`nremoved = 0`), or exactly the net's copper of the session
since its `RUN ` that no earlier `RIP ` listed. After a `SNAP` in the session,
only the first case is possible, and the expected list is the net's copper
since the last `SNAP` that no earlier `RIP ` listed.

The recorder computes this by keeping, per net, the copper ids created since
the last `SNAP`; `SNAP` and `RUN ` clear all those lists, `RIP ` empties the
net's list. It writes the list if the router has a snapshot (`Router._snap`
is set) and an empty list otherwise.

Each listed copper id is checked in this order:

1. an id that does not exist (no earlier creating chunk) is an **error**;
2. an id created in an earlier session is an **error**;
3. an id that an earlier `RIP ` already removed is an **error**;
4. otherwise the id is removed, as [8](#8-copper-state-at-any-point-normative)
   says. If it belongs to another net (of the same session), it is still
   removed, and the reader MAY report a **warning**. The writer never
   produces this.

An id in cases 1 to 3 removes nothing. A `removed` list that is not strictly
ascending, or that is not exactly the list described above, is a warning
([10](#10-reader-algorithm)); the ids it lists are applied as above.

### 7.15 `DROP` drop_to_plane

`drop_to_plane(net, name, pad, joined, max_cost)`. Written when it returns or
raises. Fixed part 16 bytes, `struct` `"<iIq"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | net | |
| 4 | `u32` | ok | 1 returned True; 0 returned False (the pad failed); 2 raised |
| 8 | `i64` | max_cost | `int(max_cost // 1)`; `-1` if `max_cost` was `None` |
| 16 | `str` | name | |
| … | `str` | pad_label | `str(pad[4])` |

The `SRCH` / `CMIT` chunks of this call immediately precede it (rule 8 of
[6](#6-order-guarantees)).

### 7.16 `MARK` phase or note

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | level | 0 phase (a named phase starts here, e.g. `"fan-out"`, `"main queue"`); 1 note (free text for the inspector) |
| 4 | `str` | text | |

### 7.17 `ROUT` routes() output

`Router.routes()` at the time of writing: merged final geometry, spurs
dropped. May appear more than once; the latest one in the session is the
current one. Fixed part 8 bytes.

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | nseg |
| 4 | `u32` | nvia |
| 8 | `nseg × (u32 layer, i32 net, f64 x0, f64 y0, f64 x1, f64 y1, f64 w)` | segments, 48 bytes each, `struct` `"<Ii5d"`, router-grid mm, in `routes()` order |
| … | `nvia × (f64 x, f64 y, i32 net, u32 reserved)` | vias, 24 bytes each, `struct` `"<ddiI"`, router-grid mm |

### 7.18 `BRD ` KiCad board

A read-only dump of a KiCad board (`gridrouter/kicad_trace.py`,
`dump_board`). A later `BRD ` replaces the earlier one entirely. Labels used:
`"placed"` (after placement, before routing) and `"final"` (after zone fill
and the last DRC). Other labels are informational.

All coordinates are board nm (`i32`). Angles are degrees, counter-clockwise as
KiCad reports them (`f64`).

**Fixed part** (16 bytes, `struct` `"<dii"`):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `f64` | thickness_mm | board thickness |
| 8 | `i32` | origin_x_nm | router cell (0, 0) in board nm (copy of `RUN ` origin, see [9](#9-coordinates)) |
| 12 | `i32` | origin_y_nm | |
| 16 | `str` | label | |

Then these sections, in this order:

**Layers**: `u32 n`, then `n × (i32 kicad_id, i32 type, str name)`.

- `name` is the canonical KiCad layer name. Writers write the name that
  `BOARD.GetStandardLayerName(id)` returns in the KiCad version they run
  with. KiCad 10 (checked with 10.0.8) returns the long names: `"F.Cu"`,
  `"In1.Cu"` … `"B.Cu"`, `"F.Mask"`, `"B.Mask"`, `"F.Silkscreen"`,
  `"B.Silkscreen"`, `"F.Adhesive"`, `"B.Adhesive"`, `"F.Paste"`,
  `"B.Paste"`, `"F.Courtyard"`, `"B.Courtyard"`, `"F.Fab"`, `"B.Fab"`,
  `"User.Drawings"`, `"User.Comments"`, `"User.Eco1"`, `"User.Eco2"`,
  `"Edge.Cuts"`, `"Margin"`, `"Rescue"`, `"User.1"` … `"User.45"`.
- Readers SHOULD accept the older short names as synonyms of the long ones:
  `"F.SilkS"`, `"B.SilkS"` (silkscreen), `"F.CrtYd"`, `"B.CrtYd"`
  (courtyard), `"F.Adhes"`, `"B.Adhes"` (adhesive), `"Dwgs.User"`,
  `"Cmts.User"`, `"Eco1.User"`, `"Eco2.User"` (`User.Drawings`,
  `User.Comments`, `User.Eco1`, `User.Eco2`). Names that are the same in
  both forms (`F.Mask`, `F.Paste`, `F.Fab`, `Edge.Cuts`, `Margin`, copper)
  need no mapping. `RUN `.`layer_name` uses the same names.
- Names are the key; `kicad_id` is informational (KiCad 10 renumbered layer
  ids).
- `type`: for copper layers the KiCad `LAYER_T` value from
  `BOARD.GetLayerType(id)`; `-1` for non-copper layers.
- Records below refer to layers by `layer_index`, the 0-based position in this
  table.

**Stackup**: `u32 n`, then `n × (align8, f64 thickness_mm, f64 er, str name)`.

- Top to bottom. `er` is the relative permittivity, or NaN (written as
  `0x7FF8000000000000`) if the item has none. `align8` precedes each entry.

**Nets**: `u32 n`, then `n × (i32 code, str name)`.

- KiCad net codes and names. These are a different namespace from router net
  numbers (`NETS`); join the two by name. KiCad's unconnected net (code 0) is
  not listed; records use `-1` for "no net".

**Footprints**: `u32 n`, then
`n × (align8, f64 rot_deg, i32 x, i32 y, u32 flipped, u32 reserved, str ref, str value, str fpid)`.

- `x`, `y`: footprint position; `flipped`: 1 if on the back; `fpid`:
  `"Library:Name"`. Fixed part of each entry 24 bytes, `struct` `"<diiII"`.
- Records refer to footprints by `fp`, the 0-based position in this table,
  `-1` for none.

**Records**: `u32 n`, `align8`, then `n × (u32 kind, u32 nbytes, body)`.

- `nbytes` is the body size **including** trailing zero padding; it is always
  a multiple of 8, so every record header and body is 8-aligned. The next
  record starts at `record_offset + 8 + nbytes`.
- A reader skips unknown `kind` values using `nbytes`, and ignores bytes in a
  known body after the fields it knows.

**Validity.** Corruption in a `BRD ` is per chunk: if any table entry or any
record cannot be read (a count or `nbytes` that runs past the payload, a
known record whose body is shorter than its fields) or breaks a bound below,
the whole `BRD ` chunk is corrupt and is skipped; the previous `BRD ` (if
any) stays current. Writers guarantee, and readers MAY check:

- `nbytes` is a multiple of 8;
- every `layer_index` (POLYSET, TRACK, TEXT, and VIA `top_layer_index`,
  `bottom_layer_index`) is `0 <= layer_index < n`, `n` being the number of
  entries in the layer table;
- every `fp` is `-1` or `0 <= fp < nfp`, `nfp` being the number of entries in
  the footprint table; PADINFO `fp` is never `-1` (see PADINFO below).

Record kinds (body layouts; offsets relative to the body start):

**1 POLYSET** (filled or stroked polygons):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | layer_index | |
| 4 | `i32` | net | KiCad net code or `-1` |
| 8 | `i32` | fp | footprint index or `-1` |
| 12 | `u32` | role | 0 pad, 1 graphic, 2 text (glyph outlines), 3 zone fill, 4 zone outline, 5 board outline, 6 rule area (keep-out) |
| 16 | `u32` | sub | 0 = filled form; > 0 = stroked with this line width in nm (roles 4 to 6: see below) |
| 20 | `u32` | npoly | |
| 24 | | polygons | `npoly ×` ( `u32 ncont`, then `ncont ×` ( `u32 npts`, `i32 xy[2 * npts]` ) ) |

- Filled (`sub = 0`): each polygon is filled with the **even-odd** rule over
  all its contours. The first contour is the outline, the others are holes.
  Contours are implicitly closed; the writer does not repeat the first point
  and the reader tolerates it if it does.
- Stroked (`sub > 0`): every contour is an open polyline drawn with round caps
  and joins at width `sub`. A closed shape (circle, rectangle outline) repeats
  its first point at the end.
- Zone fills are the unfractured fill (outline plus holes), one POLYSET per
  zone per layer. Text is converted to glyph outline polygons
  (`TransformTextToPolySet`), role 2.
- **Area boundaries: roles 4 (zone outline), 5 (board outline) and 6 (rule
  area).** These polygons mark the edge of an area. Writers use either form,
  and readers MUST accept both for each of these roles:
  - `sub = 0`: the filled form's geometry (first contour the outline, the
    others holes, implicitly closed). A reader that draws these records
    SHOULD draw each contour as a closed line, not fill the area; the line
    width is the reader's choice.
  - `sub > 0`: the stroked form. Each contour is a polyline of width `sub`;
    a closed boundary repeats its first point at the end.

  The writers in this repository: `gridrouter/kicad_trace.py` writes role 5
  with `sub = 0` (the polygon from `BOARD.GetBoardPolygonOutlines`, with a
  hole for each cut-out), and roles 4 and 6 with `sub = 0` (the zone's
  outline). `examples/record_bench.py` writes role 5 with `sub = 50000`
  (one closed rectangle, 0.05 mm wide) and role 6 with `sub = 0`. The C++
  viewer (`viewer/core/src/display.cpp`) draws roles 4 to 6 as lines in both
  cases: `sub = 0` contours closed at a 0.05 mm hairline, `sub > 0` contours
  as written at width `sub`; it also hatches role 6, and takes the board's
  extent from role 5.
- **Overlap.** Within one POLYSET record, writers SHOULD NOT write polygons
  that overlap one another; they unite them first (`kicad_trace.py` calls
  `SHAPE_POLY_SET.Simplify` when a filled shape gives more than one
  polygon). Readers MUST NOT rely on this for correctness: each polygon is
  filled on its own, so overlapping polygons still cover the right area.
  The only allowed visible effect of an overlap is that a translucent colour
  is blended twice where the polygons overlap.

**2 HOLE** (40 bytes, `struct` `"<iiiidIIii"`):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | x | centre |
| 4 | `i32` | y | |
| 8 | `i32` | w | size along the hole's own x axis (diameter for a round hole) |
| 12 | `i32` | h | size along its own y axis (equal to `w` for a round hole) |
| 16 | `f64` | rot_deg | rotation of the slot |
| 24 | `u32` | plated | 1 plated, 0 not |
| 28 | `u32` | shape | 0 round, 1 slot (oval `w × h`, rotated by `rot_deg`) |
| 32 | `i32` | fp | footprint index or `-1` (`-1` for vias' drills, if written) |
| 36 | `i32` | net | KiCad net code or `-1` |

**3 PADINFO** (20 bytes + `str`, padded to 8):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `i32` | fp | footprint index, `0 <= fp < nfp`; never `-1` (every KiCad pad belongs to a footprint). Readers MAY accept `-1` as "no footprint" |
| 4 | `i32` | net | KiCad net code or `-1` |
| 8 | `i32` | x | pad position |
| 12 | `i32` | y | |
| 16 | `u32` | attrib | KiCad `PAD_ATTRIB` value from `GetAttribute()` (informational) |
| 20 | `str` | number | pad number, e.g. `"1"`, `"A3"` |

The pad's copper shapes are separate POLYSET records with role 0.

**4 TRACK** (28 bytes, padded to 32; `struct` `"<7i"`): `i32 layer_index,
net, x0, y0, x1, y1, w` (offsets 0 to 24). A straight round-capped track.
KiCad arcs are written as several TRACK records approximating the arc.

**5 VIA** (28 bytes, padded to 32; `struct` `"<7i"`): `i32 x, y, dia, drill,
net, top_layer_index, bottom_layer_index`.

**6 TEXT** (24 bytes + `str`, padded to 8; `struct` of the fixed part
`"<iiiid"`): `i32 layer_index` (0), `i32 fp` (4), `i32 x` (8), `i32 y` (12),
`f64 angle_deg` (16), `str text` (24). Informational only (inspector, search);
the drawn text is the role-2 POLYSET.

### 7.19 `END ` end of file

Marks a complete file. Fixed part 40 bytes, `struct` `"<IIIiqqd"`.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | searches | number of `SRCH` chunks in the file |
| 4 | `u32` | routes | number of `RNB ` chunks |
| 8 | `u32` | nfailed | `len(Router.failed)` at close (0 if no router attached); see below |
| 12 | `i32` | rips | `Router.rips` at close (0 if no router) |
| 16 | `i64` | expanded_total | sum of `expanded` over all `SRCH` with `expanded >= 0` |
| 24 | `i64` | trace_entries | sum of `ntrace` over all `SRCH` (entries actually stored) |
| 32 | `f64` | wall_s | seconds from open to close |
| 40 | `nfailed × (str name, str label)` | failed | `Router.failed` at close, in order |

`failed` is the attached router's `Router.failed` list at close, as it is; it
is not computed from the chunks. It holds:

- for each `route_net` name, the pads that failed in the **latest**
  `route_net` call with that name, as `(name, pad label)`; these are the
  `RNE ` `failed` labels of that call;
- one `(name, pad label)` for each `drop_to_plane` call that returned False
  (`DROP` with `ok = 0`), unless a later `route_net` with the same `name`
  removed it. A `drop_to_plane` that raised (`ok = 2`) adds nothing;
- entries made before the recorder attached, and any edits by user code
  ([14](#14-what-the-format-does-not-capture) items 2 and 4).

So `nfailed` counts plane-drop failures as well as `route_net` pad failures,
and it need not equal the sum of the `RNE ` `nfailed` values, nor that sum
plus the number of `DROP` chunks with `ok = 0`. Only the router attached at
close (the latest session's) contributes. Readers do not check `nfailed`
against the chunks.

---

## 8. Copper state at any point (normative)

Let position P be "just after chunk P has been applied". Let R be the latest
`RUN ` at or before P. The copper present at P is:

> every `copper_id` whose creating chunk (`CMIT`, or `VIA ` with `added = 1`)
> is after R and at or before P, and which is not listed in any `RIP ` that is
> after R and at or before P.

If there is no `RUN ` at or before P, or R is corrupt, no copper is present
([6](#6-order-guarantees) rule 2). A `RIP ` removes every listed id that
exists, belongs to R's session and is not yet removed, whichever net it
belongs to ([7.14](#714-rip--one-rip)).

Nothing else adds or removes copper. In particular `SNAP`, `RBLD`, `GRDS`,
`ROUT` and `BRD ` do not change the copper set. A viewer can therefore seek in
either direction without keyframes: each copper item is alive from its
creating chunk until the `RIP ` that lists it (or the next `RUN `).

The router copper drawn this way is the union of all commits. It can differ
from `routes()` (spur pruning, merging, see [14](#14-what-the-format-does-not-capture));
`ROUT` gives the merged result when the recorder writes it.

---

## 9. Coordinates

- Router cell `(i, j)` is at router-grid mm `(i * cell, j * cell)`; `j`
  increases in the same direction as KiCad's y (down). There is no flip.
- Router mm to board nm:
  `x_nm = origin_x_nm + llround(x_mm * 1e6)`, `y_nm = origin_y_nm + llround(y_mm * 1e6)`,
  where `llround` rounds half away from zero.
  For a cell: `x_nm = origin_x_nm + llround(i * cell * 1e6)`.
- `origin_x_nm = llround(RUN.origin_x * 1e6)` from the current `RUN `; same
  for y. `BRD `.`origin_*_nm` is a copy written for viewers that show a board
  with no `RUN ` yet; when both exist the writer makes them equal and the
  `RUN ` value is authoritative for router geometry.
- `gridrouter.kicad.emit` places objects at `FromMM(origin + x)`, which can
  differ from the formula above by 1 nm. Viewers ignore this.
- Router layer `k` maps to the `BRD ` layer named `RUN.layer_name[k]`.

---

## 10. Reader algorithm

```
open file
read 24 bytes; fewer than 24 → "not written yet" (retry later; see below)
check magic == "GRTRACE\0"            else: not a trace file (reject)
check major == 1                      else: unsupported version (reject)
(minor may be anything; flags ignored)

off = 24
loop:
    if size - off < 16: stop (tail not written yet; remember off)
    read tag, len, t at off
    if any tag byte outside 0x20..0x7E: stop, report corruption at off
    pad  = (8 - len % 8) % 8
    next = off + 16 + len + pad
    if next > size: stop (tail not written yet; remember off)
    index.append({tag, t, payload_off = off + 16, len})
    if tag == "END ": complete = true; stop
    off = next

for each indexed chunk, in order:
    if tag unknown: skip
    parse fixed part; if len too small or a count runs past len: report and skip this chunk
    parse known fields; ignore bytes after them
    (SRCH trace arrays may be left unread and loaded on demand by offset)
```

"Reject" and "stop" in the algorithm: a reader MAY either stop reading and
report the problem, or fail the open / refresh call (raise an exception,
return an error status). Both are conforming.

**Header.** A file shorter than 24 bytes, including an empty file, whose
bytes do not contradict the header (the bytes present equal the start of the
magic, and the major version, once both its bytes are present, is 1) is a
header **not written yet**: not an error. A reader MUST NOT reject such a
file; it retries later. A reader MAY reject a shorter file as soon as the
bytes present contradict the magic, or as soon as both bytes of an
unsupported major version are present; it MAY also wait until 24 bytes are
present before checking.

Following a live file: keep `off` and the index; when the file grows, resume
the loop at `off`. A chunk is only indexed once complete, so a reader never
sees half a chunk. Without an `END `, the file is an incomplete run (still
being written, or the writer crashed); everything indexed is valid.

**Files replaced or rewritten while being followed.** The writer only
appends ([3.3](#33-general-chunk-rules)). Writers SHOULD never rewrite or
truncate a trace file, or replace it under the same name, while a reader may
be following it; to start again, write a new file under a new name. (The
Python writer opens its path for writing, so recording again to an existing
path truncates that file in place.) If it happens anyway:

- replacement by rename (a new file moved onto the old name) is not
  detectable by a reader that keeps the old file open: it goes on seeing the
  old file, which no longer grows. A reader that reopens the file by name on
  each refresh reads the new file from its old offset instead;
- an in-place rewrite or truncation MAY be detected by a reader on refresh:
  the file is smaller than before, the 24-byte header differs from the one
  read first, or the 16-byte header of the last indexed chunk differs. A
  reader that detects it stops following the file and reports it.

Readers SHOULD also check: `search_id` / `route_id` / `copper_id` sequences
([4.1](#41-ids)); `GRDS`.`nc` equals the current `RUN ` `nx * ny`; every
`cell < 2 * nc`.

**Errors and warnings.** Corrupt chunks ([3.3](#33-general-chunk-rules),
[3.4](#34-value-bounds)), id mismatches ([4.1](#41-ids), reported once per
gap), router chunks before the first `RUN ` or in a session whose `RUN ` is
corrupt ([6](#6-order-guarantees) rule 2), a `route_id` that names no
earlier `RNB `, and a `RIP ` that names a missing, earlier-session or
already-removed copper id ([7.14](#714-rip--one-rip)) are errors. The
following are inconsistencies a reader MAY report as **warnings**. None of
them makes a chunk corrupt; a reader does not skip or reject a chunk because
of them:

- `t` smaller than the previous chunk's `t`, or `t` NaN
  ([6](#6-order-guarantees) rule 10);
- bytes after `END ` (rule 1; the reader ignores them);
- a `SNAP` not immediately followed by a `GRDS` with `which = 1` (rule 5);
- `RNB ` / `RNE ` pairing (an `RNB ` while another `route_net` has no
  `RNE `, an `RNE ` whose `route_id` is not the open `route_net`'s, a second
  `RNE ` for a `route_id`, an `RNE ` `net` that differs from its `RNB `'s),
  and a `SRCH`, `CMIT`, `VIA `, `RBLD` or `RIP ` whose `route_id` names an
  existing `RNB ` other than the open one (rule 6);
- `END ` counts or totals (`searches`, `routes`, `expanded_total`,
  `trace_entries`) that differ from the chunks;
- a `RIP ` `removed` list that is not strictly ascending, or not exactly the
  copper [7.14](#714-rip--one-rip) describes, and a `RIP ` that removes
  another net's copper of the session;
- a `VIA ` or `CMIT` via whose `(i, j)` lies outside the grid
  ([3.4](#34-value-bounds));
- the `SRCH` trace-field inconsistencies of [7.11](#711-srch-one-search).

These are not reported at all: a `RIP ` with `nremoved = 0` before the
session's first `SNAP` ([7.14](#714-rip--one-rip)), and `src` / `tgt` cells
of any value in a `SRCH` with `error` set ([3.4](#34-value-bounds)).

The Python reader (`python3 -m gridrouter.tracefile check`, `tracefile.check`)
also reports these as warnings, which this section does not list: a chunk
other than `SRCH`, `RBLD`, `RIP `, `CMIT` inside a `route_net`, a `SRCH` or
`CMIT` whose `ctx` does not match the open `route_net`, a `SRCH` that lists a
trace cell twice, an `RBLD` not followed by its `RIP ` chunks (rule 7), and
a `DROP` not preceded by its chunks (rule 8).

---

## 11. Versioning

- **Minor version** (1.x, x > 0) may only:
  - append fields at the end of a chunk's payload, after all fields of earlier
    versions (readers ignore them, see [3.3](#33-general-chunk-rules));
  - append fields at the end of a `BRD ` record body;
  - add new chunk tags and new `BRD ` record kinds (skipped by old readers);
  - define new `SRCH`.`enc` values, new flag bits, new enumeration values in
    fields where old readers can safely ignore them, and header `flags` bits.

  A 1.0 reader reads any 1.x file.
- **Major version** changes for anything else: a field moved, resized or with
  changed meaning, a change to the header or chunk framing. A reader rejects
  a major version it does not know.
- Writers write the lowest version whose features they use.

---

## 12. Sizes

- Fixed chunks are small: `CMIT` about 48 + 4 per path cell, `SRCH` 96 plus
  arrays.
- A trace entry is 8 bytes (4 cell + 4 cost), or 4 bytes without `has_costs`.
  The largest possible search on a grid is `2 * nc` entries: on a 501 x 361
  grid (`nc` = 180,861) that is 361,722 entries, 2.9 MB.
- `GRDS` is `17 * nc` bytes plus 12 bytes and the label: about 3.1 MB on
  501 x 361.
- Measured or estimated totals:
  - `tests/bench.py` board (501 x 361, 76 nets): about 12 MB of `SRCH`
    arrays, after repeats are shared (repeats save about 35%). Few searches
    are repeats, but they are the large ones: a failed search explores
    everything it can reach before giving up. On this board 4 of 184
    searches are repeats, and they account for 34.8 % of all trace entries.
    The whole file written by `examples/record_bench.py` is 21.8 MB.
  - The real 501 x 361 dongle board, with its fan-out, CAN bus, plane stubs,
    main queue, rip-ups and polishing passes: estimated at 80 to 240 MB;
    one run measured on 2026-11-23 wrote 145.8 MB.
  - A `BRD ` chunk is dominated by text glyph polygons and zone fills: a few
    MB.
- No compression in 1.0, so the C++ reader stays dependency-free; `enc` is
  reserved for it.

---

## 13. Worked example

A file header opened at Unix time 1795392000.25 (2026-11-23 00:00:00.25 UTC),
followed by one `MARK` chunk, level 0 (phase), text `"CAN"`, written at
t = 1.5 s. 56 bytes:

```
offset  bytes                     meaning
0000    47 52 54 52 41 43 45 00   magic "GRTRACE\0"
0008    01 00 00 00 00 00 00 00   major 1, minor 0, flags 0
0010    00 00 10 80 e0 c0 da 41   open_time = 1795392000.25 (f64)
0018    4d 41 52 4b 0c 00 00 00   tag "MARK", len = 12
0020    00 00 00 00 00 00 f8 3f   t = 1.5 (f64)
0028    00 00 00 00 03 00 00 00   level = 0; str length = 3
0030    43 41 4e 00 00 00 00 00   "CAN", 1 byte str padding (to 4), 4 bytes chunk padding (to 8)
```

The payload is `u32 level` (4) + `str` (4 + 3 + 1 padding = 8) = 12 bytes, so
`len = 12` and the chunk is padded with 4 zero bytes to 16 + 16 = 32 bytes. The
next chunk would start at offset 0x38.

Python:

```python
import struct
hdr = struct.pack("<8sHHId", b"GRTRACE\0", 1, 0, 0, 1795392000.25)
text = b"CAN"
payload = struct.pack("<II", 0, len(text)) + text + b"\0" * (-len(text) % 4)
chunk = struct.pack("<4sId", b"MARK", len(payload), 1.5) + payload + b"\0" * (-len(payload) % 8)
```

---

## 14. What the format does not capture

Known gaps, by design or because the recorder cannot see them:

1. **Direct grid writes.** Writes to `T`, `W`, `novia` through the ctypes
   views (the dongle's CAN-bus `novia` fill, `BUCK_AREA`, widening and feud
   protection) are invisible to the recorder. Only a later `GRDS` shows their
   effect. The dongle records them as `MARK` notes. They do not change copper,
   but they change which cells a search can use.
2. **State before attaching.** Claims, pads and commits made before
   `Recorder.attach`, including the constructor's edge margin, appear only in
   the `GRDS "attach"` grids (and the edge-margin parameters in `RUN `), not as
   chunks or copper ids. Copper committed before attaching is not drawn as
   router copper. A `snapshot()` made before attaching is not recorded
   either; its effects are ([6](#6-order-guarantees) rule 6,
   [7.14](#714-rip--one-rip)).
3. **Attribute changes after attaching.** `RUN ` holds the values at attach
   time. Later changes to `MAX_RIPS`, `TRACK`, `pad_clear`, `pad_novia`,
   `skip_joined_stubs` and similar are not recorded as such; their effects are
   (`CMIT` width and bit 2, `PAD ` clearances, `RNE ` rips). Changing
   `VIA_COST` / `RIP_COST` / `heuristic` attributes after construction has no
   effect on the C core anyway.
4. **Edits of bookkeeping by user code**: `log`, `rips`, `failed`, `edges`,
   `stubs`, `vias`, `width`, `padcell`.
5. **Calls that raise** (other than `search`, `route_net`, `drop_to_plane`).
   `commit`, `add_via`, `claim_*`, `add_pad` can leave the grids partly
   changed before raising; no chunk is written.
6. **Read-only queries.** `owners` and `path_free` inside `route_net` are not
   recorded. The viewer sees their consequences: the `RBLD` victims (which
   exclude owner nets that have no `log` entry), and whether a `CMIT` or a
   further `SRCH` follows.
7. **Pad cells.** `RNB ` stores only the number of cells of each pad; the
   targets of each search are in its `SRCH`. `Router.padcell` is not recorded.
8. **`routes()` differs from the union of commits.** `routes()` drops spurs
   and merges collinear edges. Also, `rip` does not restore the per-stub width
   map (`Router._sw`), so after a rip-up `routes()` can give a stub that
   predates the snapshot the width of a later, ripped commit; the viewer, which
   uses each `CMIT`'s own widths, then differs from `ROUT` for that stub.
9. **One router per session.** A recorder follows one router; if user code
   drives two routers at once, only the attached one is recorded.
10. **Rounding.** Board positions computed with [9](#9-coordinates) can differ
    by 1 nm from what `kicad.emit` writes.

---

## 15. Decisions and rationale

Changes made to the layout given in the brief, and choices it left open.

1. **`RNB ` reordered.** In the brief, per-pad `f64 cx, cy` followed a
   variable-length `str`, so they could be 4-aligned only. Now the fixed part
   is 24 bytes (`u32 reserved` added after `npads`), the per-pad numeric
   records (24 bytes each) follow it, and all strings (`name`, then labels)
   come last.
2. **`PAD ` power is `f64`, and the clearances used are stored.** `add_pad`
   uses `power` as a real multiplier (`pad_clear + cell * power`), not as
   `int(power)` like `commit`. The brief's `i32 power, u32 0` became
   `f64 power`, and `f64 d_t, d_w, d_novia` were appended (88 bytes), so the
   chunk stays correct if `pad_clear` / `pad_novia` change after attaching.
3. **`VIA `: last field is `route_id`** instead of a reserved 0, for
   consistency with `SRCH` / `CMIT` / `RIP `.
4. **`VIA ` with `added = 0` gets no copper id** (`NONE32`, counter not
   advanced). Such a call creates nothing, and `RIP ` lists only real copper.
5. **`RUN `: `via_dia`, `via_drill` added** (f64 block is 12 values, fixed
   part 168 bytes). Nothing else said how large to draw router vias.
6. **Two origins.** `RUN ` (mm) is authoritative for router geometry; `BRD `
   (nm) is a copy for showing a board before any `RUN `.
7. **Several `RUN ` chunks** are allowed; each starts a new router session
   (copper reset). Ids are never reset, so they stay unique in a file.
8. **`SRCH` flags:** bit 8 `error` and bit 9 `has_trace` added. A search can
   raise (`RouterError`), and a search can run with tracing off; neither was
   representable. On error, `cost = expanded = -1`. When `has_max` is clear,
   `max_cost = -1`.
9. **`repeat_of` defined by identical traces**, not identical arguments,
   because direct grid writes between two searches are invisible. It may point
   to any earlier non-repeat search of the session. `ntrace_total`,
   `has_trace` and `has_costs` are copied from the target, `ntrace = 0`.
10. **`SRCH` 96-byte header verified**: `i64` fields at 32, 40, 48 and `f64`
    at 56 are 8-aligned; the brief's final `0` is the `reserved` field at 92.
    No other padding is needed.
11. **`enc` scope.** Only the trace arrays are encoded; `src`, `tgt` and
    `path` are always raw and come first, so a reader that does not know an
    `enc` value still gets the search.
12. **`CMIT` flags bit 2 `skip_joined_stubs`.** The dongle sets the attribute
    after constructing the router, possibly after attaching, so the `RUN `
    flag alone can be wrong.
13. **`CMIT` vias** are only those newly created (`add_via` returns early when
    the net already has a via at that cell); order and stub order specified.
14. **`RNE ` gets `u32 status`** (fixed part 24 bytes) and **`DROP` `ok = 2`**
    for a call that raised, so the begin/end pairing is never broken.
15. **`END ` failures are `(str name, str label)` pairs**, since
    `Router.failed` holds `(name, label)` tuples; a single string would lose
    the structure.
16. **`RBLD` is emitted for every `rebuild` call**, including `rebuild([])`
    (which still counts as a rip inside `route_net`) and direct calls
    (`route_id = NONE32`). `nets[k] = -1` for a name not in `log`. Its `RIP `
    chunks follow immediately.
17. **`RIP ` before any `SNAP`** has `nremoved = 0` if the router has never
    been snapshotted (the C core has no ownership records before the
    snapshot, and the Python side returns early). If the router was
    snapshotted before the recorder attached, `rip` does remove copper, and
    `RIP ` lists the net's copper ids of the session
    ([7.14](#714-rip--one-rip); clarified 2026-11-23).
18. **Chunk time `t` is the time of writing** (end of the event), so `t` is
    non-decreasing in file order; `SRCH` start is `t - duration_s`.
19. **`DROP` is written at exit** (its result is not known earlier), and its
    nested `SRCH` / `CMIT` chunks are guaranteed to precede it contiguously.
20. **`VIA ` `ctx` is always 0 with the current `Router`**, because inside
    `route_net` / `drop_to_plane` vias are only created through `commit`. The
    field is kept for generality.
21. **`BRD ` alignment.** The fixed `f64` / `i32` fields come before the
    label; stackup and footprint entries put their `f64` first and are each
    preceded by `align8`; the record list starts with `align8`; record bodies
    are zero-padded to a multiple of 8 and `nbytes` includes the padding, so
    every record is 8-aligned (the `f64` in HOLE and TEXT sits at body offset
    16).
22. **Stroked POLYSET contours** (`sub > 0`) are open polylines; a closed
    shape repeats its first point. The brief did not say whether an unfilled
    contour was closed.
23. **Net namespaces.** Router net numbers (`NETS`, every router chunk) and
    KiCad net codes (`BRD `) are different; the viewer joins them by name.
    KiCad's net 0 is written as `-1` ("no net") in `BRD ` records.
24. **`GRDS` `novia`** values other than 0/1 occur (the dongle fills the grid
    with 2 temporarily); any non-zero value means forbidden.
25. **Chunk length** is a `u32`: one chunk holds at most 4 GiB - 1. The
    largest `SRCH` on the maximum 8192 x 8192 grid is about 1.1 GB, so this
    is enough.
26. **Tag validation.** Tag bytes must be printable ASCII, so a reader can
    usually tell a corrupt file from a truncated one (not always: see "Limits
    of framing" in [3.3](#33-general-chunk-rules)).

---

## 16. Clarifications

**2026-11-23.** Clarifications of version 1.0 after the first two
implementations (Python writer and reader, C++ reader). No byte layout,
field, size or number changed; files valid before are valid now.

- [1.3](#13-composite-notations): `str` padding is part of the `str` and
  required; writers write zeros, readers do not check them.
- [3.1](#31-chunk-header-16-bytes): the writer clamps `t` to be
  non-decreasing.
- [3.3](#33-general-chunk-rules): limits of framing (no checksum or sync
  marker; a `len` past end of file reads as "not written yet"; a wrong `len`
  inside the file is usually caught by the tag check).
- [3.4](#34-value-bounds) (new): value bounds as writer guarantees, which
  readers MAY treat as corruption; no maximum grid size beyond the `u32`
  cell bound.
- [4.1](#41-ids): id checking when a chunk is corrupt or an id mismatches.
- [5](#5-what-the-recorder-emits-and-when): `RBLD` is written even when
  `rebuild` raises; `RNE ` with `status = 1` has an empty `ripped` list.
- [6](#6-order-guarantees): rule 2 (router chunks before any `RUN `), rule 4
  (`RUN `, `GRDS "attach"`, `NETS` order), rule 6 (relaxed searches after a
  snapshot made before attaching), rule 7 (a `rebuild` that raised).
- [7.8](#78-grds-occupancy-grids): a `GRDS` whose `nc` differs from its
  session's grid is a corrupt chunk.
- [7.9](#79-rnb--route_net-begins): `ncells = NONE32` when a pad's cell
  collection has no length, and the other fallbacks.
- [7.10](#710-rne--route_net-ends): what the `failed` list holds.
- [7.11](#711-srch-one-search): `duration_s` includes the cell conversion;
  `error`, `has_trace`, `has_costs`, `has_goal`, `has_max` and `nsrc` /
  `ntgt` when the call raises or the arguments do not convert; trace-field
  consistency (writer guarantees, reader handling); `enc = 0` in every file
  written today, and how a reader bounds a `SRCH` with `enc != 0`.
- [7.12](#712-cmit-one-commit): a stub's `w`.
- [7.14](#714-rip--one-rip) and [15](#15-decisions-and-rationale) item 17:
  `rip` before the session's first `SNAP` removes copper when the router was
  snapshotted before attaching; errors in `removed`.
- [7.18](#718-brd--kicad-board): corruption is per `BRD ` chunk;
  `layer_index` and `fp` ranges; PADINFO `fp` is never `-1`; `nbytes` is a
  multiple of 8.
- [7.19](#719-end--end-of-file): `END ` `failed` is `Router.failed` at
  close, which includes `drop_to_plane` failures.
- [10](#10-reader-algorithm): short and empty headers; "reject" may raise;
  files replaced or rewritten while followed; errors and warnings.
- [12](#12-sizes): few searches are repeats, but they are large ones.
- [14](#14-what-the-format-does-not-capture) item 2: a snapshot made before
  attaching.

**2026-11-23, second pass.** Open points 1 to 6 of [17](#17-open-points)
settled; open point 8 closed after checking
`gridrouter/kicad_trace.py`; `BRD ` details from running KiCad 10. No byte layout, field, size or number changed; files valid
before are valid now. Some files that a reader could report as corrupt or as
having errors before are valid now (a `SRCH` with `error` set and invalid
`src` / `tgt`, a via outside the grid, a `RIP ` of another net's copper).

- [3.4](#34-value-bounds): `src` / `tgt` of a `SRCH` with `error` set are not
  bounded; such a chunk is valid and is not reported (open point 4). Via
  cells (`VIA `, `CMIT` vias) are not bounded; a via outside the grid is a
  valid chunk, a `VIA ` with `added = 1` takes its copper id, and a reader
  MAY warn (open point 5).
- [4.1](#41-ids): after an id mismatch a reader reports one error and
  continues from the stored id, so one gap gives one error; a reader MAY
  count a corrupt `SRCH`, `RNB ` or `CMIT`, and both readers do (open point
  3). The Python `check` now counts them too.
- [6](#6-order-guarantees) rule 2 and [8](#8-copper-state-at-any-point-normative):
  a corrupt `RUN ` starts a new session with no grid and no copper; its
  router chunks are errors until the next valid `RUN ` (open point 2). The
  Python reader now does this; before, it continued in the previous session.
- [7.14](#714-rip--one-rip): before the session's first `SNAP`, an empty
  list and the net's copper since `RUN ` are both valid without a warning
  (open point 1). Each listed id is checked in a fixed order: missing,
  earlier session or already removed is an error and removes nothing;
  another net's copper of the session is removed, with a warning (open
  point 6).
- [7.18](#718-brd--kicad-board): layer names are those
  `BOARD.GetStandardLayerName` returns in the writer's KiCad version; KiCad
  10.0.8's list is given, and readers SHOULD accept the older short names as
  synonyms. Roles 4 to 6 are area boundaries, in either the filled or the
  stroked form, and both forms of the board outline that the writers produce
  are described. Polygons within one POLYSET record SHOULD NOT overlap.
  PADINFO `fp` is never `-1`: `gridrouter/kicad_trace.py` writes pads only
  from their footprint, with that footprint's index (open point 8).
- [10](#10-reader-algorithm): the error and warning lists follow the points
  above; what is not reported at all; the extra warnings the Python reader
  gives.
- [12](#12-sizes): measured file sizes added (bench 21.8 MB, dongle
  145.8 MB).
- `examples/record_bench.py` writes KiCad 10's layer names
  (`F.Silkscreen`), and the viewer's sample `small.grtrace` was regenerated
  with it.

**2026-11-24.** The grid bound of [3.4](#34-value-bounds) is now the
router's limit: `0 < nx <= 8192` and `0 < ny <= 8192`, where before only
`2 * nx * ny <= 2^32` was stated and no maximum grid size was set. No byte
layout, field or size changed, and every file the recorder can write is
valid as before (the router rejects larger grids). A reader MAY reject a
larger `RUN ` as corrupt, and both readers now do, so a file cannot make a
reader size per-cell arrays beyond what the router could have used.

---

## 17. Open points

Points the clarifications could not settle without changing an
implementation, or that need a later version. Until settled, a file that
triggers one of them may be read differently by the two existing readers.
Settled points keep their numbers, because code comments refer to them.

1. *Settled 2026-11-23 (second pass):* `RIP ` before the session's first
   `SNAP`, router snapshotted before attaching. See
   [7.14](#714-rip--one-rip).
2. *Settled 2026-11-23 (second pass):* router chunks after a corrupt `RUN `.
   See [6](#6-order-guarantees) rule 2.
3. *Settled 2026-11-23 (second pass):* continuing after an id mismatch. See
   [4.1](#41-ids).
4. *Settled 2026-11-23 (second pass):* invalid cells in a `SRCH` with
   `error` set. See [3.4](#34-value-bounds).
5. *Settled 2026-11-23 (second pass):* `VIA ` and `CMIT` vias outside the
   grid. See [3.4](#34-value-bounds).
6. *Settled 2026-11-23 (second pass):* `RIP ` listing another net's copper.
   See [7.14](#714-rip--one-rip).
7. **`enc != 0` and appended fields** ([7.11](#711-srch-one-search),
   [11](#11-versioning)). The encoded trace data runs to the end of the
   payload, so a minor version that defines an `enc` value and also appends
   fields to `SRCH` must first say how the end of the encoded data is found
   (for example, a length inside the encoding). The Python reader keeps all
   bytes after `path` as the encoded data; the C++ reader ignores them.
   Nothing written today is affected (`enc = 0`).
8. *Settled 2026-11-23 (second pass):* PADINFO `fp` is never `-1`
   ([7.18](#718-brd--kicad-board)). `gridrouter/kicad_trace.py` writes a
   pad only while it walks the pads of a footprint, with that footprint's
   index. The C++ reader also accepts `-1`, which is allowed.

Only point 7 is open.

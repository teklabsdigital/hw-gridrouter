# Visual Gridrouter

A native desktop app that replays a `gridrouter` routing run from a trace
file (`.grtrace`): the board as KiCad draws it, the A* wavefront of each
search, paths committing, rip-ups, retries and failures. It only watches; it
never steers the router.

Status: **milestone M1**, the trace reader. The core reads `.grtrace` files
(every chunk kind of format 1.0, following a file that is still being
written), and `rvdump` prints what it reads. The app still opens one empty
window; board drawing and playback come in later milestones.

## Layout

```
viewer/
  core/                 shared core: portable C++17, standard library only
    include/rv.h        the C99 interface every client uses
    src/                implementation (api.cpp is the C boundary, trace.cpp the
                        reader, file.cpp the only platform-specific file I/O)
    tests/              self-contained tests (no framework), a C99 smoke test,
                        and synth.cpp, a test-only trace writer built from the spec
  tools/                rvdump (trace inspector) and rvsynth (writes the synthetic trace)
  mac/                  macOS client: Swift 6 + AppKit, programmatic UI
    Sources/RVKit/      Swift wrapper over rv.h (shared with later tools)
    Sources/RouteViewer the app
    Info.plist          bundle metadata, .grtrace document type
    bundle.sh           wraps the binary into a signed .app
  Package.swift         SwiftPM targets RVCore, RVKit, RouteViewer
  Makefile              test, asan, dump, app, selftest, run, clean
  check_additive.sh     checks the branch changed nothing outside the plan
```

The core owns everything that is not pixels or widgets, so the later Win32
and Qt6 clients reuse it unchanged. Clients are thin: they call `rv.h` and
draw what it returns.

## Build, run, test

From this folder, or from the repository root with `make -C viewer <target>`:

```sh
make test       # core tests + C99 header smoke test (clang++ -std=c++17 -Werror)
make asan       # the same under AddressSanitizer and UndefinedBehaviorSanitizer
make dump       # build/rvdump and build/rvsynth
make app        # swift build -c release, then build/Visual Gridrouter.app (ad-hoc signed)
make selftest   # builds the app, starts it, checks window and core, prints "selftest ok", exits
make run        # opens the app
make clean      # removes build/ and .build/
sh check_additive.sh   # read-only: fails if anything outside viewer/ changed beyond the allow-list
```

`make selftest` opens its window for a fraction of a second and exits on its
own (0 on success, 1 on a failed check, 2 if it does not finish in 10 s).

`rvdump` reads a trace through `rv.h`, like a client:

```sh
build/rvdump FILE              # summary: version, complete or not, chunk counts, runs, nets, searches, totals
build/rvdump --chunks FILE     # one line per chunk
build/rvdump --search N FILE   # one search, with its first wavefront entries
build/rvsynth OUT              # write the synthetic test trace (--no-end, --truncate N)
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

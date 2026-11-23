# Visual Gridrouter

A native desktop app that replays a `gridrouter` routing run from a trace
file (`.grtrace`): the board as KiCad draws it, the A* wavefront of each
search, paths committing, rip-ups, retries and failures. It only watches; it
never steers the router.

Status: **milestone M0**, the skeleton. The app opens one empty window that
shows the core's API version, which proves the Swift → C → C++ linkage. The
trace reader, board drawing and playback come in later milestones.

## Layout

```
viewer/
  core/                 shared core: portable C++17, standard library only
    include/rv.h        the C99 interface every client uses
    src/                implementation (api.cpp is the C boundary)
    tests/              self-contained tests (no framework) and a C99 smoke test
  mac/                  macOS client: Swift 6 + AppKit, programmatic UI
    Sources/RVKit/      Swift wrapper over rv.h (shared with later tools)
    Sources/RouteViewer the app
    Info.plist          bundle metadata, .grtrace document type
    bundle.sh           wraps the binary into a signed .app
  Package.swift         SwiftPM targets RVCore, RVKit, RouteViewer
  Makefile              test, asan, app, selftest, run, clean
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
make app        # swift build -c release, then build/Visual Gridrouter.app (ad-hoc signed)
make selftest   # builds the app, starts it, checks window and core, prints "selftest ok", exits
make run        # opens the app
make clean      # removes build/ and .build/
sh check_additive.sh   # read-only: fails if anything outside viewer/ changed beyond the allow-list
```

`make selftest` opens its window for a fraction of a second and exits on its
own (0 on success, 1 on a failed check, 2 if it does not finish in 10 s).

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

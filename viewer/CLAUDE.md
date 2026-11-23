# CLAUDE.md: instructions for AI assistants working in `viewer/`

`viewer/` is Visual Gridrouter, a native app that replays `.grtrace` files
written by the router's recorder. See `README.md` here for layout and build
commands, and `../docs/TRACE_FORMAT.md` for the file format (the single
contract between recorder and viewer).

## Hard rules

1. **Zero third-party packages.** System frameworks and standard libraries
   only. No SwiftPM dependencies, no Homebrew packages, no CMake, no vendored
   libraries. Do not install, upgrade or download anything.
2. **Native per platform, thin clients over one core.**
   - macOS: Swift 6 + AppKit, programmatic UI (no nib or storyboard).
   - Later: C++/Win32 (Direct2D) and C++/Qt6, on the same core.
   - Anything a second client would have to repeat belongs in the core, not
     in Swift. Clients translate types, draw what the core returns and own
     widgets; nothing more.
3. **The core is portable C++17**, standard library only. No platform
   headers. The whole core may contain exactly one `#ifdef _WIN32`, for file
   I/O (`core/src/file.cpp`; its Windows branch is not yet compiled or
   tested). The interface is the plain C99 header
   `core/include/rv.h`; it must compile as C99 (`make test` checks with
   `-std=c99 -Wpedantic -Werror`) and as C++17.
4. **The C boundary never throws or aborts.** Every entry point in
   `core/src/api.cpp` catches all exceptions and maps them to an `rv_status`.
   Sessions are opaque and single-threaded; there are no callbacks; returned
   pointers are borrowed until the next mutating call on that session.
5. **Every change to the core needs a test** in `core/tests/` (plain
   `TEST`/`CHECK` from `check.hpp`, no framework). `make test` and
   `make asan` must pass. Bump `RV_API_VERSION` on any change to `rv.h`.
   `make test` also compares the golden images (`mac/Tests/golden/`). Run
   `make golden-update` only for an intended change to the drawing, and
   look at every regenerated PNG before keeping it.
6. **Swift:** Swift 6 language mode, strict concurrency clean, all UI and all
   core calls on `@MainActor`. `make app` builds with warnings as errors.
7. **Additive only.** The viewer must not change how the router behaves for
   anyone else. Run `sh viewer/check_additive.sh` before declaring work done.

## Relation to the router repository's rules

The router's `CLAUDE.md` rule 6 ("standard-library Python plus a C99
compiler") applies to `gridrouter/`, not here. The viewer has its own
equivalent: rule 1 above. Do not touch `gridrouter/`, `tests/` or
`tests/ref_router.py` from viewer work.

## Owner's tool and package policy

For any tool, compiler, SDK or package this project might use:

- ask the owner first;
- pin an exact version released at least 30 days earlier;
- disable auto-update, and never upgrade by reflex.

The toolchain in `README.md` (Command Line Tools 26.6) is the one in use.
Windows and Linux toolchains are chosen and pinned with the owner when that
work starts.

## Working in this environment

A Bash hook rejects compound commands: one command per Bash call, no `&&`,
`;` or a leading `cd`. Use `make -C viewer <target>`,
`swift build --package-path viewer`, and absolute paths. Pipelines are fine.

Never leave a GUI window open while verifying: use `make -C viewer selftest`,
which starts the app, checks it and exits within a second.

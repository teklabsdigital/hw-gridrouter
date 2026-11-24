#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital
#
# check_additive.sh: read-only check that the visual-router work is additive.
#
# Lists every path changed on this branch relative to main (committed, staged,
# unstaged and untracked) and fails if any lies outside viewer/ and is not in
# the allow-list of files the plan permits to change.  Changes nothing.
#
#   sh viewer/check_additive.sh [BASE]      (BASE defaults to main)
set -eu

base=${1:-main}
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$root"

allowed='
gridrouter/gridrouter.c
gridrouter/gridrouter.h
gridrouter/recorder.py
gridrouter/tracefile.py
gridrouter/kicad_trace.py
tests/test_trace.py
tests/test_kicad_trace.py
tests/run_traced.py
tests/run_all.py
examples/record_bench.py
docs/TRACE_FORMAT.md
.gitignore
README.md
CLAUDE.md
docs/assets/kicadautorouter.mp4
docs/assets/kicadautorouter-small.mp4
docs/assets/visual-gridrouter.gif
docs/assets/visual-gridrouter-wavefront.png
docs/assets/visual-gridrouter-ripup.png
docs/assets/visual-gridrouter-final.png
'

changed=$(
    {
        git diff --name-only "$base...HEAD"        # committed on the branch
        git diff --name-only HEAD                  # staged and unstaged
        git ls-files --others --exclude-standard   # untracked, not ignored
    } | sort -u
)

viewer_count=0
allowed_list=''
bad_list=''
# One path per line; no word splitting on spaces, no globbing.
set -f
IFS='
'
for path in $changed; do
    case "$path" in
        viewer/*) viewer_count=$((viewer_count + 1)) ;;
        *)
            if printf '%s' "$allowed" | grep -qxF "$path"; then
                allowed_list="$allowed_list  $path
"
            else
                bad_list="$bad_list  $path
"
            fi
            ;;
    esac
done

echo "check_additive: changes relative to $base"
echo "  viewer/: $viewer_count file(s)"
if [ -n "$allowed_list" ]; then
    echo "allowed outside viewer/:"
    printf '%s' "$allowed_list"
fi
if [ -n "$bad_list" ]; then
    echo "NOT ALLOWED outside viewer/:"
    printf '%s' "$bad_list"
    echo "check_additive: FAILED"
    exit 1
fi
echo "check_additive: ok"

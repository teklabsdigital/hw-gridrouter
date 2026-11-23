#!/bin/sh
# golden.sh: golden-image tests for the board renderer.
#
#   sh mac/Tests/golden.sh check    render every case and compare it with mac/Tests/golden/NAME.png
#   sh mac/Tests/golden.sh update   render every case and replace the golden PNGs
#
# Run through `make golden` / `make golden-update`, which build rvsynth and
# rvrender first.  Inputs come only from rvsynth (synthetic and fixed), never
# from core/tests/data/small.grtrace, which the recorder may regenerate.
# Comparison: rvrender --compare, a pixel differs when a channel is off by
# more than 48; at most 0.2 % of pixels may differ (anti-aliasing and the
# band split vary a little between machines).

set -eu

mode=${1:-check}
case "$mode" in
check | update) ;;
*)
    echo "usage: golden.sh check|update" >&2
    exit 2
    ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
viewer=$(cd "$here/../.." && pwd)
rvrender="$viewer/.build/release/rvrender"
rvsynth="$viewer/build/rvsynth"
out="$viewer/build/golden"
golden="$here/golden"
mkdir -p "$out" "$golden"

"$rvsynth" --stress "$out/stress.grtrace" >/dev/null
"$rvsynth" "$out/sample.grtrace" >/dev/null

failures=0
cases=0

# golden NAME INPUT [rvrender options...]
golden() {
    name=$1
    input=$2
    shift 2
    cases=$((cases + 1))
    if ! "$rvrender" "$out/$input.grtrace" "$out/$name.png" "$@" >/dev/null; then
        echo "golden.sh: $name: rvrender failed: FAIL"
        failures=$((failures + 1))
        return 0
    fi
    if [ "$mode" = update ]; then
        cp "$out/$name.png" "$golden/$name.png"
        echo "golden.sh: updated $name.png"
    elif [ ! -f "$golden/$name.png" ]; then
        echo "golden.sh: $name.png: no golden image (make golden-update): FAIL"
        failures=$((failures + 1))
    elif ! "$rvrender" --compare "$golden/$name.png" "$out/$name.png"; then
        failures=$((failures + 1))
    fi
}

# The stress board ("final", latest), default visibility, fitted.
golden stress_fit stress --size 480x348
# A close-up: SOIC pads, tracks, vias and holes, silkscreen text.
golden stress_closeup stress --size 640x400 --window 104,54,124,66.5
# Every layer shown, masks, paste and courtyards included.
golden stress_all_layers stress --size 480x348 --all-layers
# The "placed" board: no tracks, vias or zones.
golden stress_placed stress --size 640x464 --board 0
# Only F.Cu and Edge.Cuts, zones and text hidden.
golden stress_fcu stress --size 640x464 --layers F.Cu,Edge.Cuts --hide-classes 0,5
# The synthetic sample board around (101, 51) mm: track, plated pad hole,
# via (copper, hole wall, drill) and a 45-degree NPTH slot.
golden sample sample --size 400x400 --window 99.6,49.6,102.8,52.8

if [ "$failures" -ne 0 ]; then
    echo "golden.sh: $failures of $cases cases FAILED"
    exit 1
fi
echo "golden.sh: $cases cases $( [ "$mode" = update ] && echo updated || echo ok )"

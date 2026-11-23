#!/bin/sh
# bundle.sh: wrap the release build of RouteViewer into
# build/Visual Gridrouter.app and sign it ad hoc.
#
# Run `swift build -c release` first (`make app` does both).  Uses only
# tools from the Command Line Tools: swift, plutil, codesign.
set -eu

viewer=$(cd "$(dirname "$0")/.." && pwd)
bin_dir=$(swift build -c release --package-path "$viewer" --show-bin-path)
exe="$bin_dir/RouteViewer"
app="$viewer/build/Visual Gridrouter.app"

if [ ! -x "$exe" ]; then
    echo "bundle.sh: $exe not found; run swift build -c release first" >&2
    exit 1
fi

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$exe" "$app/Contents/MacOS/RouteViewer"
cp "$viewer/mac/Info.plist" "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"

plutil -lint "$app/Contents/Info.plist"
codesign --force --sign - --timestamp=none "$app"
codesign --verify --strict "$app"
echo "bundle.sh: built $app"

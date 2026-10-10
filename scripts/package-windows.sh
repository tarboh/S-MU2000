#!/usr/bin/env bash
# Stage a Windows release tree from a built BUILD dir.
# Usage: package-windows.sh <build-dir> <dist-dir>
# Layout: <dist>/{LICENSE.txt,NOTICE.txt,roms.txt.example,bin/*.exe,plugins/...}
set -euo pipefail

BUILD="${1:?usage: $0 <build-dir> <dist-dir>}"
DIST="${2:?usage: $0 <build-dir> <dist-dir>}"

BINARIES="verify boot render live midisend panel gui statetest rec blocktime vst3probe clapprobe vstiprobe"

mkdir -p "$DIST/bin" "$DIST/plugins"

for b in $BINARIES; do
  if [ -f "$BUILD/$b.exe" ]; then
    cp -f "$BUILD/$b.exe" "$DIST/bin/"
  else
    echo "warning: missing $BUILD/$b.exe" >&2
  fi
done

for p in "$BUILD/S-MU2000.vst3" "$BUILD/S-MU2000.clap" "$BUILD/S-MU2000.dll"; do
  if [ -e "$p" ]; then
    cp -rf "$p" "$DIST/plugins/"
  else
    echo "warning: missing $p" >&2
  fi
done

# The photo-style panel art: the gui finds art/real one level above bin/
# (layout.cpp, find_default). Without it the standalone gui falls back to
# the plain built-in panel.
mkdir -p "$DIST/art"
cp -rf art/real "$DIST/art/"

cp -f LICENSE "$DIST/LICENSE.txt"
cp -f NOTICE.txt "$DIST/NOTICE.txt"
if [ -f "$BUILD/ASIO-GPL-3.0.txt" ]; then
  cp -f "$BUILD/ASIO-GPL-3.0.txt" "$DIST/ASIO-GPL-3.0.txt"
else
  rm -f "$DIST/ASIO-GPL-3.0.txt"
fi
[ -f doc/vst3-readme.txt ] && cp -f doc/vst3-readme.txt "$DIST/plugins/vst3-readme.txt" || true

printf '# Put the path to your ROM folder on the first line, e.g.\n# C:\\Users\\you\\roms\n' > "$DIST/roms.txt.example"

echo "staged windows dist in $DIST:"
ls "$DIST" "$DIST/bin" "$DIST/plugins"

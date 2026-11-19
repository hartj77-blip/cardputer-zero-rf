#!/usr/bin/env bash
# Pack a device build into the kit that gets copied to the Cardputer Zero.
#   tools/package-device.sh <device build dir> <out.tar.gz>
# The build dir comes from "cmake -S shell -B <dir> -DZS_BACKEND=device ..."
# (zeroshell + rf/ programs) or from "cmake -S rf -B <dir>" (radio programs only).
set -euo pipefail
BUILD=$(realpath "$1")
OUT=$(realpath -m "$2")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
KIT=$STAGE/zeroos-rf
mkdir -p "$KIT/bin"
STRIP=${STRIP:-strip}
for f in zeroshell rf/zrfd rf/zrf rf/zrf-sim-sensors rf/zrf-sim-aircraft zrfd zrf zrf-sim-sensors zrf-sim-aircraft; do
    [ -f "$BUILD/$f" ] || continue
    cp "$BUILD/$f" "$KIT/bin/"
    "$STRIP" "$KIT/bin/$(basename "$f")"
done
[ -f "$KIT/bin/zrfd" ] || { echo "no zrfd in $BUILD" >&2; exit 1; }
cp "$ROOT"/device/install.sh "$ROOT"/device/bringup.sh "$ROOT"/device/60-zeroos-rf.rules \
   "$ROOT"/device/zeroos-rf-blacklist.conf "$KIT/"
git -C "$ROOT" describe --always --dirty > "$KIT/VERSION" 2>/dev/null || echo dev > "$KIT/VERSION"
tar -C "$STAGE" -czf "$OUT" zeroos-rf
echo "$OUT: $(ls "$KIT/bin" | tr '\n' ' ')"

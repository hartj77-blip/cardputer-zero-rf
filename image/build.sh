#!/usr/bin/env bash
# Build a flashable ZeroOS RF image for the Cardputer Zero: the base ZeroOS
# image with the RF home screen and the radio programs already installed.
#
# We don't fork the base's image builder. We run it (from the base/ submodule)
# with our ZeroShell, and add one stage of our own after its stage.
#
# Usage: image/build.sh <device build dir> [--prepare-only]
#   <device build dir> holds arm64 builds of zeroshell and rf/zrfd, rf/zrf, rf/zrf-sim-*
#   --prepare-only stops before the long build, leaving the builder ready in $WORK
# Env:   WIFI_COUNTRY, FIRST_USER_PASS, PIGEN_NATIVE as for the base builder; WORK (build dir)
# Needs: what the base builder needs (Docker or root, ~20 GB disk, 1-3 hours).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD=$(realpath "${1:?usage: image/build.sh <device build dir> [--prepare-only]}")
PREPARE_ONLY=${2:-}
export WORK=${WORK:-$ROOT/build/image}
BASE=$ROOT/base

[ -f "$BASE/image/build.sh" ] || { echo "error: base OS not found; run: git submodule update --init base" >&2; exit 1; }
for f in zeroshell rf/zrfd rf/zrf rf/zrf-sim-sensors rf/zrf-sim-aircraft rf/zrf-sim-aprs; do
    [ -x "$BUILD/$f" ] || { echo "error: $BUILD/$f not found (build the device target first)" >&2; exit 1; }
done
file "$BUILD/rf/zrfd" | grep -q "aarch64" || { echo "error: $BUILD/rf/zrfd is not an arm64 program" >&2; exit 1; }

VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)

# Our stage, with the programs it installs.
STAGE=$WORK/stage-zeroos-rf
rm -rf "$STAGE"
mkdir -p "$WORK"
cp -r "$HERE/stage-zeroos-rf" "$STAGE"
mkdir -p "$STAGE/files"
cp "$BUILD"/rf/zrfd "$BUILD"/rf/zrf "$BUILD"/rf/zrf-sim-sensors "$BUILD"/rf/zrf-sim-aircraft "$BUILD"/rf/zrf-sim-aprs "$STAGE/files/"
cp "$ROOT"/device/bringup.sh "$ROOT"/device/60-zeroos-rf.rules "$ROOT"/device/zeroos-rf-blacklist.conf \
   "$ROOT"/docs/THIRD_PARTY.md "$STAGE/files/"
sed -i "s|@ZEROOS_RF_VERSION@|$VERSION|" "$STAGE/01-run.sh"

# The base builder, with two changes: our stage goes in after its own, and the
# image is named for this edition. Like the base does with M5's scripts, each
# edit is checked, so a changed base script stops the build instead of
# silently producing a plain ZeroOS image.
PATCHED=$WORK/base-image
rm -rf "$PATCHED"
cp -r "$BASE/image" "$PATCHED"
B=$PATCHED/build.sh
sed -i "s|^install -m 0755 \"\$ZEROSHELL_BIN\" stage2/06-zeroos/files/zeroshell\$|&\nrm -rf stage2/07-zeroos-rf\ncp -r \"$STAGE\" stage2/07-zeroos-rf|" "$B"
grep -q "cp -r \"$STAGE\" stage2/07-zeroos-rf" "$B" || { echo "error: could not add the RF stage (base image/build.sh changed?)" >&2; exit 1; }
sed -i -e 's|^IMG_NAME=zeroos$|IMG_NAME=zeroos-rf|' -e 's|-zeroos-cardputerzero-arm64|-zeroos-rf-cardputerzero-arm64|g' "$B"
grep -q '^IMG_NAME=zeroos-rf$' "$B" && grep -q 'zeroos-rf-cardputerzero-arm64' "$B" || { echo "error: could not rename the image (base image/build.sh changed?)" >&2; exit 1; }
# The base script works out its own location and repository root; it is now a copy.
sed -i "s|^ROOT=\$(cd \"\$HERE/..\" \&\& pwd)\$|ROOT=\"$BASE\"|" "$B"
grep -q "^ROOT=\"$BASE\"\$" "$B" || { echo "error: could not point the base builder at the base repository" >&2; exit 1; }

if [ "$PREPARE_ONLY" = "--prepare-only" ]; then
    echo "Prepared. Builder: $B   RF stage: $STAGE"
    exit 0
fi
ZEROOS_VERSION="rf-$VERSION" bash "$B" "$BUILD/zeroshell"
echo "ZeroOS RF image written to $WORK/pi-gen/deploy/"

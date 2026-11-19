#!/usr/bin/env bash
# Render the RF screens with the headless build and save PNGs.
#   shell/tools/screenshots.sh <headless build dir> <out-dir>
# The build dir holds zeroshell and rf/zrfd. The radio is the simulated one.
set -euo pipefail
BUILD=$(realpath "$1")
OUT=$(realpath -m "$2")
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
DEMO=$(mktemp -d)
trap 'rm -rf "$DEMO"' EXIT   # the daemon the app starts exits by itself when idle

STEPS="shot:01-home.ppm"
STEPS+=",open:spectrum,wait:4000,shot:02-spectrum-fm.ppm"
STEPS+=",right,right,right,right,wait:1500,shot:03-spectrum-retuned.ppm"
STEPS+=",key:p,type:433.92,wait:300,shot:04-spectrum-goto.ppm"
STEPS+=",enter,wait:5000,shot:05-spectrum-433.ppm"
STEPS+=",type:162.55,enter,key:z,key:m,key:m,wait:4000,shot:06-spectrum-listening.ppm"
STEPS+=",open:spectrum,key:b,wait:300,shot:07-spectrum-bands.ppm"
STEPS+=",down,down,down,down,down,down,down,down,down,down,enter,key:m,key:s,wait:3000,shot:08-spectrum-sideband.ppm"
STEPS+=",open:scanner,wait:6000,shot:09-scanner-stopped.ppm"
STEPS+=",key:l,wait:800,shot:10-scanner-scanning.ppm"
STEPS+=",right,right,right,wait:6000,shot:11-scanner-2m.ppm"
STEPS+=",open:sensors,wait:4000,shot:12-sensors.ppm"
STEPS+=",down,down,down,down,enter,wait:400,shot:13-sensors-detail.ppm"

(cd "$OUT" && HOME="$DEMO" TZ=UTC ZRFD_SOCKET="$DEMO/zrfd.sock" ZRFD_BIN="$BUILD/rf/zrfd" ZRF_SOURCE=sim ZRF_AUDIO=none \
    "$BUILD/zeroshell" --script "$STEPS")
python3 "$HERE/../../base/shell/tools/ppm2png.py" "$OUT"

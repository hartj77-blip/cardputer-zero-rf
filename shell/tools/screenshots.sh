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
STEPS+=",down,down,down,down,down,down,down,down,down,down,down,down,down,down,down,enter,key:m,key:s,key:r,wait:3000,shot:08-spectrum-sideband.ppm"
STEPS+=",open:scanner,wait:6000,shot:09-scanner-stopped.ppm"
STEPS+=",key:l,wait:800,shot:10-scanner-scanning.ppm"
STEPS+=",right,right,right,right,right,right,wait:6000,shot:11-scanner-2m.ppm"
STEPS+=",open:sensors,wait:4000,shot:12-sensors.ppm"
STEPS+=",down,down,down,down,enter,wait:400,shot:13-sensors-detail.ppm"
STEPS+=",open:aircraft,wait:9000,shot:14-aircraft.ppm"
STEPS+=",open:sdrserver,wait:300,shot:15-sdr-server-off.ppm"
STEPS+=",enter,wait:1500,shot:16-sdr-server-on.ppm"
STEPS+=",open:station,wait:200,shot:17-station.ppm"
STEPS+=",enter,type:w1aw,enter,down,enter,type:FN31pr,wait:200,shot:18-station-editing.ppm"
STEPS+=",enter,down,down,down,down,enter,type:1,enter,wait:200,shot:19-station-set.ppm"
STEPS+=",open:aprs,wait:5000,shot:20-aprs.ppm"
STEPS+=",enter,wait:300,shot:21-aprs-detail.ppm"
STEPS+=",open:spectrum,key:b,down,down,down,down,down,down,down,down,down,down,down,down,down,down,down,enter,key:m,wait:400"
STEPS+=",open:logbook,wait:200,shot:23-logbook-empty.ppm"
STEPS+=",key:n,wait:200,type:k1xyz,wait:100,shot:24-logbook-new.ppm"
STEPS+=",enter,down,down,down,down,down,enter,type:Field day,enter,down,enter,wait:400"
STEPS+=",key:n,wait:200,type:ve3abc,enter,down,down,enter,enter,down,down,down,down,enter,wait:400"
STEPS+=",key:n,wait:200,type:k1xyz,wait:100,shot:25-logbook-worked-before.ppm"
STEPS+=",enter,esc,wait:300,shot:26-logbook.ppm"
STEPS+=",down,enter,wait:300,shot:27-logbook-contact.ppm"
STEPS+=",open:ft8,wait:300,shot:29-ft8-waiting.ppm"
STEPS+=",wait:16000,shot:30-ft8.ppm"
# Following a rig needs Hamlib; its model 1 is a pretend rig sitting on 145 MHz.
if command -v rigctld >/dev/null; then
    STEPS+=",open:spectrum,key:f,wait:4000,shot:28-spectrum-follow-rig.ppm"
fi

(cd "$OUT" && HOME="$DEMO" TZ=UTC ZRFD_SOCKET="$DEMO/zrfd.sock" ZRFD_BIN="$BUILD/rf/zrfd" ZRF_SOURCE=sim ZRF_AUDIO=none \
    "$BUILD/zeroshell" --script "$STEPS")
# The contacts logged above must be on disk as ADIF another program can read.
grep -q "<CALL:5>K1XYZ <QSO_DATE:8>" "$DEMO/Documents/logbook.adi" || { echo "logbook file missing the first contact" >&2; exit 1; }
grep -q "<CALL:6>VE3ABC .*<MODE:2>CW <RST_SENT:3>599" "$DEMO/Documents/logbook.adi" || { echo "logbook file missing the CW contact" >&2; cat "$DEMO/Documents/logbook.adi" >&2; exit 1; }
cp "$DEMO/Documents/logbook.adi" "$OUT/logbook-sample.adi"
python3 "$HERE/../../base/shell/tools/ppm2png.py" "$OUT"

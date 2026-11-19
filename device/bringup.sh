#!/usr/bin/env bash
# First-run check of ZeroOS RF on a real Cardputer Zero with a real dongle.
# Run as the normal user (pi), over SSH or in the Terminal app, with the
# dongle and an antenna plugged in:
#   zeroos-rf-bringup            everything (about 3 minutes)
#   zeroos-rf-bringup --quick    skip the long sample-loss and decoder tests
# Nothing here changes the system. The full output is also saved to
# ~/zeroos-rf-bringup.txt; that file is what to send back.
set -u
QUICK=0; [ "${1:-}" = "--quick" ] && QUICK=1
REPORT=$HOME/zeroos-rf-bringup.txt
exec > >(tee "$REPORT") 2>&1

PASS=0; FAIL=0; WARN=0
ok()   { echo "  PASS  $*"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }
warn() { echo "  WARN  $*"; WARN=$((WARN + 1)); }
note() { echo "        $*"; }
step() { echo; echo "== $*"; }
field() { sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }

DIR=$(mktemp -d)
export ZRFD_SOCKET=$DIR/zrfd.sock
ZRFD_PID=
cleanup() { [ -n "$ZRFD_PID" ] && kill "$ZRFD_PID" 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT

# Battery gauge: prints "<volts> <amps> <watts>" or nothing.
power_now() {
    local d v i
    for d in /sys/class/power_supply/*; do
        [ -r "$d/voltage_now" ] && [ -r "$d/current_now" ] || continue
        v=$(cat "$d/voltage_now"); i=$(cat "$d/current_now")
        awk -v v="$v" -v i="$i" 'BEGIN { if (i < 0) i = -i; printf "%.2f %.3f %.2f\n", v / 1e6, i / 1e6, v * i / 1e12 }'
        return
    done
}
# Average watts over N seconds.
power_avg() {
    local n=$1 sum=0 count=0 w
    for _ in $(seq "$n"); do
        w=$(power_now | awk '{print $3}')
        [ -n "$w" ] && { sum=$(awk -v a="$sum" -v b="$w" 'BEGIN { print a + b }'); count=$((count + 1)); }
        sleep 1
    done
    [ "$count" -gt 0 ] && awk -v s="$sum" -v c="$count" 'BEGIN { printf "%.2f\n", s / c }'
}
# CPU percent (of one core) and memory in MB of a process over N seconds.
proc_load() {
    local pid=$1 n=$2 t0 t1 hz
    hz=$(getconf CLK_TCK)
    t0=$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null) || return
    sleep "$n"
    t1=$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null) || return
    awk -v a="$t0" -v b="$t1" -v hz="$hz" -v n="$n" -v rss="$(awk '/VmRSS/ {print $2}' "/proc/$pid/status")" \
        'BEGIN { printf "%.0f%% of one core, %.1f MB\n", (b - a) / hz / n * 100, rss / 1024 }'
}

echo "ZeroOS RF bring-up, $(date -u '+%Y-%m-%d %H:%M UTC')"

step "System"
note "model:  $( (tr -d '\0' < /proc/device-tree/model) 2>/dev/null || echo unknown)"
note "kernel: $(uname -r)"
note "os:     $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
note "memory: $(awk '/MemTotal/ {t=$2} /MemAvailable/ {a=$2} END {printf "%d MB total, %d MB available", t/1024, a/1024}' /proc/meminfo)"
note "zrfd:   $(command -v zrfd || echo MISSING)   rtl_test: $(command -v rtl_test || echo MISSING)"
note "librtlsdr: $(dpkg-query -W -f='${Version}' librtlsdr2 2>/dev/null || echo unknown)"
if [ -e /sys/class/power_supply ]; then
    for d in /sys/class/power_supply/*; do
        [ -r "$d/charge_full_design" ] && note "battery design capacity ($(basename "$d")): $(( $(cat "$d/charge_full_design") / 1000 )) mAh"
        [ -r "$d/status" ] && note "battery status: $(cat "$d/status")"
    done
fi
[ -e /sys/class/rtc/rtc0 ] && note "clock chip: $(cat /sys/class/rtc/rtc0/name 2>/dev/null)" || note "clock chip: none found"
IDLE_W=$(power_avg 5)
[ -n "$IDLE_W" ] && note "power draw now, radio idle: $IDLE_W W" || note "no battery gauge readings"

step "Dongle on USB"
if lsusb 2>/dev/null | grep -qi "0bda:283[28]"; then
    ok "found: $(lsusb | grep -i '0bda:283[28]' | sed 's/.*ID //')"
else
    bad "no RTL-SDR dongle on USB (looked for 0bda:2838 / 0bda:2832)"
    lsusb 2>/dev/null | sed 's/^/        /'
fi
if lsmod 2>/dev/null | grep -q dvb_usb_rtl28xxu; then
    bad "the TV driver dvb_usb_rtl28xxu is loaded and holds the dongle; run install.sh, then replug"
else
    ok "TV driver not loaded"
fi
if command -v uhubctl >/dev/null; then
    if sudo -n uhubctl 2>/dev/null | grep -q "ppps"; then ok "USB hub can switch port power (uhubctl sees ppps)"
    else warn "uhubctl found no hub with per-port power switching (or needs sudo): $(sudo -n uhubctl 2>&1 | head -1)"; fi
fi

step "Driver (rtl_test)"
OUT=$(timeout 5 rtl_test -t 2>&1)
echo "$OUT" | sed 's/^/        /' | head -14
if echo "$OUT" | grep -qi "Blog V4"; then ok "driver recognises the RTL-SDR Blog V4"
elif echo "$OUT" | grep -q "Found.*device"; then warn "dongle opens, but the driver did not say 'RTL-SDR Blog V4'. If this is a V4, HF and tuning may be wrong; the RTL-SDR Blog driver would be needed."
else bad "rtl_test could not open the dongle"; fi

if [ "$QUICK" = 0 ]; then
    step "Sample loss (10 s each)"
    for rate in 1024000 2048000 2400000; do
        OUT=$(timeout 10 rtl_test -s $rate 2>&1)
        lost=$(echo "$OUT" | grep -c "lost at least")
        if [ "$lost" = 0 ]; then ok "$rate samples/s: no loss reported"
        else warn "$rate samples/s: $lost loss reports"; fi
    done
fi

step "Radio daemon on the real dongle"
zrfd --source rtlsdr --audio none &
ZRFD_PID=$!
for _ in $(seq 30); do [ -S "$ZRFD_SOCKET" ] && break; sleep 0.1; done
if zrf send hello 2>/dev/null | grep -q "^ok zrfd"; then ok "zrfd started"; else bad "zrfd did not start"; fi

BEST_DB=-999; BEST_HZ=0
zrf send rate 2048000 >/dev/null
for mhz in $(seq 89 2 107); do
    zrf send tune ${mhz}000000 >/dev/null
    OUT=$(zrf peak 320 2>&1) || { bad "no spectrum at $mhz MHz: $OUT"; break; }
    db=$(echo "$OUT" | field db); hz=$(echo "$OUT" | field hz)
    note "$((mhz - 1))-$((mhz + 1)) MHz: strongest $(awk -v h="$hz" 'BEGIN { printf "%.1f", h / 1e6 }') MHz at $db dB"
    if awk -v a="$db" -v b="$BEST_DB" 'BEGIN { exit !(a > b) }'; then BEST_DB=$db; BEST_HZ=$hz; fi
done
zrf send rate 1024000 >/dev/null
if [ "$BEST_HZ" != 0 ]; then
    # Round to the 100 kHz broadcast grid.
    STATION=$(awk -v h="$BEST_HZ" 'BEGIN { printf "%d", int(h / 1e5 + 0.5) * 1e5 }')
    ok "spectrum works; strongest FM signal near $(awk -v h="$STATION" 'BEGIN { printf "%.1f", h / 1e6 }') MHz ($BEST_DB dB)"
    zrf send tune "$STATION" >/dev/null
    zrf snapshot "$HOME/zeroos-rf-fm.ppm" 320 80 >/dev/null 2>&1 && note "picture saved: ~/zeroos-rf-fm.ppm"
else
    STATION=98300000
fi

step "Load while watching the spectrum"
zrf watch 320 >/dev/null 2>&1 &
WATCH=$!
sleep 2
note "zrfd: $(proc_load "$ZRFD_PID" 5)"
W=$(power_avg 5); [ -n "$W" ] && note "power draw, receiving: $W W (idle was ${IDLE_W:-?} W)"
kill $WATCH 2>/dev/null; wait $WATCH 2>/dev/null

step "Load while listening to broadcast FM (audio discarded)"
zrf listen wfm 8 >/dev/null 2>&1 &
LISTEN=$!
sleep 2
note "zrfd: $(proc_load "$ZRFD_PID" 5)"
wait $LISTEN
kill "$ZRFD_PID" 2>/dev/null; wait "$ZRFD_PID" 2>/dev/null; ZRFD_PID=

step "Sound: 6 seconds of $(awk -v h="$STATION" 'BEGIN { printf "%.1f", h / 1e6 }') MHz through the speaker"
zrfd --source rtlsdr &
ZRFD_PID=$!
for _ in $(seq 30); do [ -S "$ZRFD_SOCKET" ] && break; sleep 0.1; done
zrf send tune "$STATION" >/dev/null
OUT=$(zrf listen wfm 6 2>&1)
if echo "$OUT" | grep -q "audio output failed"; then bad "the audio player could not start (tried sox 'play', then 'aplay')"
else warn "audio was sent to the speaker. DID YOU HEAR THE STATION? This script can't tell."; fi
kill "$ZRFD_PID" 2>/dev/null; wait "$ZRFD_PID" 2>/dev/null; ZRFD_PID=

if [ "$QUICK" = 0 ]; then
    step "Sensor decoder (rtl_433 at 433.92 MHz, 30 s)"
    if command -v rtl_433 >/dev/null; then
        COUNT=$(timeout 30 rtl_433 -f 433920000 -F json 2>/dev/null | tee "$DIR/sensors.json" | wc -l)
        if [ "$COUNT" -gt 0 ]; then
            ok "$COUNT messages decoded"
            sed -n 's/.*"model" *: *"\([^"]*\)".*/\1/p' "$DIR/sensors.json" | sort | uniq -c | sed 's/^/        /'
        else warn "rtl_433 ran but decoded nothing in 30 s (there may be no sensors nearby)"; fi
    else
        bad "rtl_433 is not installed"
    fi
fi

echo
echo "== Result: $PASS passed, $WARN to check by hand, $FAIL failed"
echo "Report saved to $REPORT"

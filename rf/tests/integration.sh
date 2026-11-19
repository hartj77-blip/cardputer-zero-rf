#!/usr/bin/env bash
# End-to-end test: start the radio daemon on the simulated source and drive it
# with the zrf client. Usage: integration.sh <dir containing zrfd and zrf>
set -euo pipefail

BIN=$(cd "$1" && pwd)
DIR=$(mktemp -d)
export ZRFD_SOCKET="$DIR/zrfd.sock"
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

"$BIN/zrfd" --source sim --audio "file:$DIR/audio.raw" &
PID=$!
for _ in $(seq 50); do [ -S "$ZRFD_SOCKET" ] && break; sleep 0.1; done

fail() { echo "FAIL: $*" >&2; exit 1; }
field() { sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }

"$BIN/zrf" send hello | grep -q "^ok zrfd 1 source=sim" || fail "hello"

# Nobody is subscribed yet, so the receiver must be closed.
[ "$("$BIN/zrf" send status | field running)" = 0 ] || fail "receiver open with no subscribers"

# Tuned to the simulated station at 98.3 MHz: the peak is near the middle.
"$BIN/zrf" send tune 98300000 >/dev/null
col=$("$BIN/zrf" peak 320 | field col)
[ "$col" -ge 140 ] && [ "$col" -le 180 ] || fail "peak at column $col, expected near 160"

# Tuned 200 kHz lower, the same station moves right by about 62 columns.
"$BIN/zrf" send tune 98100000 >/dev/null
out=$("$BIN/zrf" peak 320)
col=$(echo "$out" | field col); hz=$(echo "$out" | field hz)
[ "$col" -ge 200 ] && [ "$col" -le 245 ] || fail "peak at column $col after retune, expected near 222"
[ "$hz" -ge 98200000 ] && [ "$hz" -le 98400000 ] || fail "peak reported at $hz Hz"

# An offset (IF tap or upconverter) shifts what the hardware is tuned to.
"$BIN/zrf" send offset 200000 >/dev/null
col=$("$BIN/zrf" peak 320 | field col)
[ "$col" -ge 140 ] && [ "$col" -le 180 ] || fail "peak at column $col with offset, expected near 160"
"$BIN/zrf" send offset 0 >/dev/null

# Bad input is refused, not applied.
"$BIN/zrf" send tune banana >/dev/null && fail "accepted a bad frequency"
"$BIN/zrf" send spectrum 5000 >/dev/null && fail "accepted too many columns"
"$BIN/zrf" send selfdestruct >/dev/null && fail "accepted an unknown command"

# The snapshot is a complete picture.
"$BIN/zrf" snapshot "$DIR/out.ppm" 320 20 >/dev/null
[ "$(head -c 14 "$DIR/out.ppm" | tr '\n' ' ')" = "P6 320 70 255 " ] || fail "snapshot header"
[ "$(stat -c %s "$DIR/out.ppm")" = $((14 + 320 * 70 * 3)) ] || fail "snapshot size"

# Listening writes audio at the reported rate, and stops when the listener leaves.
"$BIN/zrf" send tune 162550000 >/dev/null
"$BIN/zrf" listen nfm 2
sleep 0.3
status=$("$BIN/zrf" send status)
[ "$(echo "$status" | field mode)" = off ] || fail "still listening after the listener left: $status"
bytes=$(stat -c %s "$DIR/audio.raw")
[ "$bytes" -ge 96000 ] && [ "$bytes" -le 150000 ] || fail "2 s of 32 kHz audio should be about 128000 bytes, got $bytes"
"$BIN/zrf" send mode ssb >/dev/null && fail "accepted an unknown mode"

# Squelch: silence on an empty frequency, audio on a signal.
"$BIN/zrf" send squelch 10 >/dev/null
"$BIN/zrf" send tune 163200000 >/dev/null
"$BIN/zrf" listen nfm 2
[ "$(tr -d '\0' < "$DIR/audio.raw" | wc -c)" -lt 2000 ] || fail "squelch let noise through on an empty frequency"
"$BIN/zrf" send tune 162550000 >/dev/null
"$BIN/zrf" listen nfm 2
[ "$(tr -d '\0' < "$DIR/audio.raw" | wc -c)" -gt 60000 ] || fail "squelch muted a strong signal"
"$BIN/zrf" send squelch off >/dev/null
"$BIN/zrf" send squelch loud >/dev/null && fail "accepted a bad squelch level"

# An IQ recording has the right size and description, and plays back with the
# signal where it was.
"$BIN/zrf" send tune 98300000 >/dev/null
"$BIN/zrf" record "$DIR/fm station.cu8" 2
sleep 0.3
bytes=$(stat -c %s "$DIR/fm station.cu8")
[ "$bytes" -ge 3000000 ] && [ "$bytes" -le 5000000 ] || fail "2 s at 1.024 MS/s should be about 4 MB, got $bytes"
grep -q '"core:frequency": 98300000' "$DIR/fm station.sigmf-meta" || fail "recording description missing or wrong"
[ "$("$BIN/zrf" send status | field recorded)" = 0 ] || fail "still recording after the client left"
"$BIN/zrfd" --source "file:$DIR/fm station.cu8" --socket "$DIR/play.sock" &
PLAY=$!
for _ in $(seq 50); do [ -S "$DIR/play.sock" ] && break; sleep 0.1; done
col=$("$BIN/zrf" --socket "$DIR/play.sock" peak 320 | field col)
kill "$PLAY"; wait "$PLAY" || true
[ "$col" -ge 140 ] && [ "$col" -le 180 ] || fail "recording plays back with its peak at column $col, expected near 160"
"$BIN/zrf" send record relative/path.cu8 >/dev/null && fail "accepted a relative recording path"

# The sensor decoder's output reaches the client, and stops when it leaves.
out=$("$BIN/zrf" sensors 2)
echo "$out" | grep -q '"model":"Acurite-Tower"' || fail "no decoded sensor messages: $out"
sleep 0.3
[ "$("$BIN/zrf" send status | field decode)" = off ] || fail "still decoding after the client left"
pgrep -P "$PID" >/dev/null && fail "decoder program left running"
# Aircraft frames come through the same way, one job replacing another.
out=$("$BIN/zrf" aircraft 3)
[ "$(echo "$out" | grep -c '^\*8D[0-9A-F]\{26\};$')" -ge 5 ] || fail "no aircraft frames: $out"
sleep 0.3
pgrep -P "$PID" >/dev/null && fail "aircraft program left running"
"$BIN/zrf" send decode submarines >/dev/null && fail "accepted an unknown decoder"   # our daemon's children only
# The spectrum comes back afterwards.
"$BIN/zrf" send tune 98300000 >/dev/null
"$BIN/zrf" peak 320 >/dev/null || fail "no spectrum after decoding"
# A decoder that isn't installed is reported, not hidden.
ZRF_SENSORS_CMD="exec no-such-decoder" "$BIN/zrfd" --source sim --socket "$DIR/alt.sock" &
ALT=$!
for _ in $(seq 50); do [ -S "$DIR/alt.sock" ] && break; sleep 0.1; done
"$BIN/zrf" --socket "$DIR/alt.sock" sensors 1 2>&1 | grep -q "not installed" || fail "missing decoder not reported"
kill "$ALT"; wait "$ALT" || true

# After the last client leaves, the receiver is released again.
sleep 0.3
[ "$("$BIN/zrf" send status | field running)" = 0 ] || fail "receiver still open after clients left"

# A clean stop removes the socket.
kill -TERM "$PID"; wait "$PID" || fail "daemon exited with an error"
[ ! -e "$ZRFD_SOCKET" ] || fail "socket left behind"

# Started with --idle-exit, the daemon leaves by itself once nobody is connected.
"$BIN/zrfd" --source sim --idle-exit 1 &
PID=$!
for _ in $(seq 50); do [ -S "$ZRFD_SOCKET" ] && break; sleep 0.1; done
"$BIN/zrf" send hello >/dev/null || fail "idle-exit daemon did not answer"
for _ in $(seq 40); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$PID" 2>/dev/null && fail "daemon still running 4 s after its last client left"
[ ! -e "$ZRFD_SOCKET" ] || fail "socket left behind after idle exit"

echo "integration test passed"

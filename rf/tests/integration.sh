#!/usr/bin/env bash
# End-to-end test: start the radio daemon on the simulated source and drive it
# with the zrf client. Usage: integration.sh <dir containing zrfd and zrf>
set -euo pipefail

BIN=$(cd "$1" && pwd)
DIR=$(mktemp -d)
export ZRFD_SOCKET="$DIR/zrfd.sock"
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

"$BIN/zrfd" --source sim &
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

# rf: the radio core

The part of ZeroOS RF that talks to the receiver. It has no screen code and does not depend on
the base OS, so it builds and runs on any Linux PC.

| Piece | What |
| --- | --- |
| `zrfd` | The radio daemon. The one process that owns the receiver. Tunes it, computes the spectrum and serves clients over a Unix socket. |
| `zrf` | A command-line client for scripts, tests and trying things out. |
| `src/sdr/` | Receiver backends: `sim` (generated signals), `file:<path>` (IQ recording playback), `rtlsdr` (a real dongle). |
| `src/dsp/` | FFT and power spectrum. |

![Simulated FM broadcast band](../docs/screenshots/spectrum-fm.png)

*Two simulated FM stations at 320 columns, shown at 2x: spectrum trace above, waterfall below.*

## Status

- Working and tested: the simulated and file sources, the spectrum, the daemon, the client.
- Written but **never run on a real dongle**: the `rtlsdr` backend. It has only been
  syntax-checked against the librtlsdr header.
- Not started: listening (demodulation and audio), the on-screen apps, decoders, rig control.

## Build and try

```sh
sudo apt install cmake g++ librtlsdr-dev     # librtlsdr-dev is optional: without it, no dongle support
cmake -S rf -B build/rf
cmake --build build/rf -j
ctest --test-dir build/rf --output-on-failure
```

```sh
./build/rf/zrfd --source sim &               # or: --source rtlsdr, --source file:capture.u8
./build/rf/zrf send tune 98300000            # Hz
./build/rf/zrf watch                         # live spectrum as text; Ctrl-C to stop
./build/rf/zrf snapshot fm.ppm 320 100       # spectrum and waterfall picture
```

The simulated source has FM stations at 88.1, 94.5, 98.3, 98.7, 101.1 and 104.7 MHz, an airband
carrier at 121.5 MHz, narrow FM at 145.5 and 162.55 MHz, and sensor-style bursts at 433.92 MHz.

## Power rule

The daemon opens the receiver only while at least one client is subscribed to its output, and
closes it when the last one leaves. A dongle that nobody is watching is not streaming.

## Protocol

A Unix stream socket, by default `$XDG_RUNTIME_DIR/zrfd.sock` (override with `--socket` or
`ZRFD_SOCKET`). Only the owning user can connect. Everything is text lines, so `socat` works as a
client.

Commands, each answered with `ok ...` or `err <reason>`:

| Command | Effect |
| --- | --- |
| `hello` | `ok zrfd 1 source=<name>` (1 is the protocol version) |
| `tune <hz>` | Set the frequency shown and listened to. |
| `offset <hz>` | Added to the frequency before it reaches the hardware. For a dongle on a rig's IF output, or an external upconverter. Default 0. |
| `rate <hz>` | Sample rate, which is also the width of the view. Default 1024000. |
| `gain <db>` or `gain auto` | Tuner gain. |
| `spectrum <cols> [fps]` | Subscribe to spectrum frames reduced to `cols` columns (1 to 1024), at up to 30 frames a second. |
| `spectrum off` | Unsubscribe. |
| `status` | `ok freq=... offset=... rate=... gain=... fps=... running=0|1 source=... [error=...]` |

Lines the daemon sends without being asked, to subscribed clients only:

| Line | Meaning |
| --- | --- |
| `S <freq_hz> <rate_hz> <hex>` | One spectrum frame. Two hex digits per column, lowest frequency first, so `freq_hz` is the middle. Level in dB relative to full scale is `value * 0.5 - 120`. |
| `E <reason>` | The receiver can't be opened or stopped working (for example `no dongle`). The daemon retries every 2 seconds, so plugging a dongle in is enough. |

A client that falls behind has frames dropped rather than queued.

# cardputer-zero-rf

ZeroOS RF: a radio-focused edition of ZeroOS for the M5Stack Cardputer Zero Lite, built around a
USB software-defined radio dongle (RTL-SDR Blog V4 first, others later).

![RF screens](docs/screenshots/contact-sheet.png)

*Rendered from the real UI code at 320x170 against a simulated radio, shown at 2x.*

| Part | Status |
| --- | --- |
| `rf/` radio core: receiver backends, spectrum, FM and AM demodulation, the radio daemon `zrfd` and its protocol | Working against a simulated receiver and recorded files. Never run on a real dongle; audio checked by measurement, never by ear. |
| `shell/` the base OS's ZeroShell plus the RF apps. First app: **Spectrum** | Working in the headless build. Never run on a device. |
| `base/` the base OS, [cardputer-zero](https://github.com/hartj77-blip/cardputer-zero), pinned as a submodule | Unchanged; we build on it, we don't fork it |
| Image (SD card) build with the SDR packages | Not started |

## Spectrum

Left/Right tune by one step, Up/Down change the step size, **Z**/**X** narrow or widen the view,
**G** cycles gain, **P** toggles peak hold, **C** centres on the strongest signal. **M** cycles
listening: off, broadcast FM, narrow FM, AM. Type a frequency in MHz and press **Enter** to jump
to it. Audio stops when you leave the app.

## Build and try on a PC

```sh
git submodule update --init base             # needs access to the base repository
sudo apt install cmake g++ pkg-config libsdl2-dev librtlsdr-dev
cmake -S shell -B build/sim -DZS_BACKEND=sim
cmake --build build/sim -j
ZRFD_BIN=$PWD/build/sim/rf/zrfd ./build/sim/zeroshell
```

With no dongle support built in or no dongle wanted, the PC build uses the simulated radio. Set
`ZRF_SOURCE=rtlsdr` to use a real dongle, or `ZRF_SOURCE=file:capture.u8` to replay a recording.

Render the RF screens to PNG (what CI does):

```sh
cmake -S shell -B build/headless -DZS_BACKEND=headless && cmake --build build/headless -j
shell/tools/screenshots.sh build/headless build/screenshots
```

## CI and the private base

The base repository is private, so CI here can't fetch it by default and the RF shell job skips
with a warning. To turn it on, add a repository secret named `BASE_REPO_TOKEN` holding a
fine-grained token with read-only Contents access to `cardputer-zero`. The radio core job needs no
token.

## Docs

- [docs/PLAN.md](docs/PLAN.md): the architecture, the power, CPU and memory budget, the app list
  and the phases.
- [rf/README.md](rf/README.md): the radio core and the daemon's protocol.

# ZeroOS RF: plan

A radio-focused edition of [ZeroOS](https://github.com/hartj77-blip/cardputer-zero) for the
M5Stack Cardputer Zero **Lite**, built around a USB software-defined radio (SDR) dongle. The first
supported dongle is the RTL-SDR Blog V4.

Status: phases R0, R2, R3 (except ships) and R4 are written and run against a simulated receiver:
Spectrum with FM, AM, sideband and CW listening, squelch and IQ recording; Scanner; Sensors;
Aircraft; APRS; SDR Server; Station settings; and rig frequency control through Hamlib. Phase R1,
the first run on real hardware, is next: [BRINGUP.md](BRINGUP.md). Nothing has been tried on a
device or a real dongle. Pager decoding has been dropped (private communications).

## Goal

Turn the Cardputer Zero Lite into a pocket radio receiver and signal tool: see the spectrum, listen,
scan, decode common digital signals, record, and serve the dongle to a bigger computer.

**Design rule (inherited from ZeroOS):** it can do almost anything, but only one heavy thing at a
time. For RF that becomes: **one program owns the dongle at a time.**

## Not in scope

- **Transmitting.** RTL-SDR dongles are receive-only, and the plan stays receive-only even when
  other dongles are added later. **(decide)**
- Decrypting or defeating protection on any signal. Decoders are limited to open, unencrypted
  protocols.
- A general desktop SDR suite (GNU Radio, GQRX, SDR++) on the built-in screen. They don't fit 320x170
  or 512 MB. They remain usable in Dock mode or on a PC through the remote server (phase R4).

What may legally be received differs by country; the docs will say so and leave the choice to the
owner.

## Target hardware

| | Lite (target) | Full (later port) |
| --- | --- | --- |
| SoC, RAM | Raspberry Pi CM0: 4x Cortex-A53 at 1 GHz, 512 MB | same |
| Screen, keyboard | 320x170, 46 keys | same |
| USB | 1x USB-A, 2x USB-C, behind a GL852G hub | same |
| Audio, Wi-Fi/BT, IR, RTC, fuel gauge, expansion ports | yes | same |
| Motion sensors (BMI270 + BMM150) | **no** | yes |
| Camera (IMX219) | **no** | yes |
| microSD card | not included | 32 GB included |

The Full model is a superset of the Lite, so the rule that makes the port free is simple: **nothing
in the RF edition may depend on the motion sensors or the camera.** The base hardware layer already
shows `--` for a missing sensor instead of failing. On the Lite the edition hides Compass and turns
the motion wake and face-down settings off; on the Full they come back with no RF code changes.

Battery: M5Stack's own documentation lists 3.7 V, 1750 mAh (about 6.5 Wh) and gives one figure
for both models. Launch press coverage in May 2026 said 1500 mAh. The Kickstarter page could not
be read from here, so which cell actually ships is unconfirmed; the fuel gauge reports the design
capacity, so the first unit settles it **(measure)**. At 1500 mAh the receiving estimate below
drops by about a seventh.

To confirm: whether the Lite has the RX8130 clock chip. M5's spec table lists it without splitting
by model.

## The three hard limits

### 1. Power

| Load | Draw |
| --- | --- |
| Cardputer Zero idle, stock | about 2.5 W |
| RTL-SDR Blog V4, running | 250-270 mA at 5 V, about 1.3 W (datasheet) |
| V4 bias tee, if switched on for an external amplifier | up to 180 mA more, about 0.9 W |

A running dongle adds roughly half again to the idle draw, so expect **about 1.5 hours** of
receiving on battery **(measure)**. What the edition does about it:

- The dongle streams only while an RF app that needs it is open. Leaving the app stops the stream.
- Cut USB power to the dongle when idle if the GL852G hub supports per-port switching **(measure)**.
  If it doesn't, the dongle still draws some power while plugged in, and the app says so.
- Screen-off keeps receiving only for jobs that asked for it (audio playing, recording, scanning,
  decoding). A plain spectrum view stops with the screen.
- The bias tee is off by default, has its own toggle, and shows in the status bar when on.
- The time-left estimate in the status bar already follows real draw, so it stays honest.

Also to check: whether the USB-A port can supply about 450 mA (dongle plus bias tee) without the
voltage sagging, and how hot the V4 gets against the case.

### 2. CPU

Four A53 cores at 1 GHz, shared with the UI. Budget:

| Job | Plan | Expected load |
| --- | --- | --- |
| Sample rate | 1.024 MS/s by default; 2.048-2.4 MS/s only for jobs that need it (ADS-B, wide view) | USB and copy cost rises with rate **(measure)** |
| Spectrum | 1024-point FFT with NEON, 15-20 frames a second, averaged down to 320 columns | well under one core **(measure)** |
| Narrow FM, AM, SSB audio | decimate early, demodulate at 48 kHz or less | under half a core **(measure)** |
| Broadcast FM | mono first; stereo and RDS only if there is headroom | about one core **(measure)** |
| ADS-B (2.4 MS/s) | existing decoder; known to run on slower Pis | about one core **(measure)** |

All DSP runs outside the UI process (see Architecture), so a busy decoder can't freeze the keyboard.
RF apps switch the power profile to Performance while receiving and restore it afterwards.

### 3. Memory

About 512 MB total, with ZeroShell and services already using some. Each RF job is one small
native process: sample buffers are a few MB, and the decoders we wrap are in the 10-30 MB range
**(measure)**. No Python or browser in the receive path. IQ recordings go straight to the card and
are never held in RAM.

Smaller limits to design around:

- **Screen.** Apps get 320x150. One FFT column per pixel, a spectrum trace of about 50 rows over
  a waterfall of about 80 rows, and one line for frequency, mode and signal level.
- **Interference.** The onboard 2.4 GHz Wi-Fi, the USB hub and the screen all radiate. A short USB
  extension lead and the dongle's own shielding help; a "quiet mode" that pauses Wi-Fi while
  receiving is cheap to add **(measure whether it matters)**.
- **Recording size.** 1.024 MS/s of 8-bit IQ is about 2 MB a second, 7 GB an hour. The recorder
  shows time left on the card and stops before it fills.

## Architecture

### How it relates to the base OS

**Decided: layer on the base, don't fork it.** `base/` is `cardputer-zero` pinned as a git
submodule, the same way the base pins M5Stack's image builder instead of forking it. Base fixes
arrive by moving the pin.

Today this needs no change to the base: `shell/CMakeLists.txt` compiles the base shell's sources
as they are, leaves out its launcher list, and adds ours (`shell/src/registry.cpp`) plus the RF
apps. The cost is that our build file mirrors the base's and our launcher list repeats the base's
apps, so both need a look whenever the pin moves. Two small hooks in the base would remove that:

1. **An edition hook for apps.** Let a build add apps to the launcher and choose their order.
2. **An edition hook for the image.** Let `image/build.sh` take extra stage directories, extra
   packages and an image name. The RF image build waits on this or on a wrapper script here.

The base repository is private and this one is public, so CI here needs a read token to build the
shell (see the README).

### Pieces

```
 ZeroShell RF apps (LVGL screens, thin)        Spectrum, Radio, Scanner, Decoders, Recorder
        |  Unix socket: commands in, spectrum rows / status / decoded lines out
 zrfd  (one small daemon, owns the dongle)     tuning, gain, FFT, demodulation, audio out, IQ record
        |                         \
 SDR source layer                  wrapped tools, run one at a time in place of zrfd's own DSP:
   rtlsdr | file playback | sim      rtl_433, an ADS-B decoder, multimon-ng, AIS, rtl_tcp
        |
 librtlsdr, USB
```

- **`zrfd`, the radio daemon.** One process holds the dongle. It starts when the first RF app
  opens and exits when the last one closes. It does the FFT and demodulation and plays audio through
  PipeWire. The UI only draws what it's sent. This keeps the keyboard responsive, lets the radio
  keep playing while another app is in front, and gives scripts and Remote mode the same interface
  the UI uses.
- **SDR source layer.** One small interface (open, tune, set gain, set sample rate, read samples)
  with three backends from day one: `rtlsdr`, **IQ file playback**, and a **simulated** source that
  generates carriers, an FM station and noise. The simulator and CI use the last two, so every RF
  screen can be built, tested and screenshotted with no radio attached, matching how the base
  treats its hardware layer.
- **Wrapped tools.** For protocols that already have a good small decoder, `zrfd` hands the dongle
  to that program and passes its output (JSON lines where available) to the UI. We write screens,
  not decoders.
- **Image stage.** Adds the SDR packages, a udev rule so the `pi` user can open the dongle, a
  blacklist for the kernel's DVB-T TV driver (which otherwise grabs the dongle), and the `zrfd`
  service.

### RTL-SDR Blog V4 notes

- The V4 needs a driver that knows its R828D tuner and built-in HF upconverter. Debian trixie ships
  rtl-sdr 2.0.x, which should include that support **(confirm with `rtl_test` on the unit; fall
  back to building the RTL-SDR Blog driver in the image if not)**.
- With the right driver, HF (about 0.5-28.8 MHz) tunes like any other frequency; the driver
  switches the upconverter itself. Range overall is about 0.5 MHz to 1.76 GHz.
- Other RTL-SDR dongles (V3, generic R820T) use the same library and should work unchanged; on a V3,
  HF needs direct-sampling mode, which gets a toggle.

## Apps

| App | What it does | Phase |
| --- | --- | --- |
| **Spectrum** | Live spectrum and waterfall. Arrows tune, digits enter a frequency, keys for span, gain and peak hold. The base every other app builds on. | R0 |
| **Radio** | Listen: broadcast FM, narrow FM, AM, SSB. Squelch, volume, presets, band list (FM broadcast, airband, marine, weather, amateur bands), record audio to `~/Recordings` (which Sync already copies off the device). | R2 |
| **Scanner** | Step through presets or a range, stop on a signal, log what was heard and when. Wide survey mode: sweep a large range and draw it as one picture. | R3 |
| **Sensors** | Weather stations, tyre sensors, doorbells and remotes on 315/433/868/915 MHz, using rtl_433. A list of devices with latest readings. | R3 |
| **Aircraft** | ADS-B at 1090 MHz: a table of aircraft, and distance and bearing when a GPS module is fitted. | R3 |
| **Pager / Ships** | POCSAG text and AIS ship positions, using existing decoders. Lower priority; added if the first two wrapped decoders go smoothly. | R3 |
| **Recorder** | Record and replay raw IQ, with a small metadata file (SigMF) so PC tools can open it. | R4 |
| **SDR server** | Serve the dongle over the network (rtl_tcp) so SDR++ or GQRX on a PC or phone uses the Cardputer as a remote receiver, over Wi-Fi or Tailscale. | R4 |

All RF apps share one status line (frequency, mode, gain, signal level, a clear "no dongle" state)
and the same keys for tuning, so learning one teaches the rest.

## Connecting to other radios

The device is a standalone receiver first. Linking to a transceiver is an optional layer on top,
not something the core depends on:

- **Rig control.** Read and set a rig's frequency and mode over USB serial with Hamlib. Frequency
  and mode only; no transmit keying.
- **Panadapter.** The dongle shows the spectrum around the rig's frequency and follows its dial,
  either from the rig's IF output or from a shared antenna.
- **Rig audio in.** Decode digital modes from the rig's USB sound card instead of the dongle.

The daemon already has the one thing this needs from the core: a frequency `offset`. A second USB
device adds to the battery load **(measure)**. Planned after Radio (phase R2).

## Phases

| Phase | Deliverable | Needs hardware |
| --- | --- | --- |
| **R0. Foundation** | Repository layout, base pin and the edition hooks, SDR source layer with simulated and file sources, `zrfd` with FFT, the Spectrum app in the simulator, CI with screenshots. | no |
| **R1. Bring-up on the Lite** | Image stage, V4 driver check, udev and TV-driver blacklist, real Spectrum on the device. Run the measurements in the list below and replace the estimates in this file. | yes |
| **R2. Radio** | Demodulators, audio out, presets, squelch, audio recording, HF on the V4. | yes, for tuning by ear |
| **R3. Scanner and decoders** | Scanner, Sensors, Aircraft, then Pager and Ships. | partly (decoders can be tested from recorded IQ files) |
| **R4. Record and serve** | IQ Recorder, SDR server, a Remote-mode entry that opens it over Tailscale. | yes |
| **R5. More radios, Full model** | Other dongles through SoapySDR (Airspy, HackRF receive). Full-model check: confirm nothing changes, turn motion features back on. Optional GPS tagging of recordings and logs with the Cap LoRa-1262 or a USB GPS. | yes |

R0 is the only phase that can be finished without a device, and it is built so that everything
after it can be developed and reviewed in the simulator first.

## To measure on the unit (phase R1)

- Battery life while receiving, in each power profile, screen on and off.
- Whether the GL852G hub can switch port power, and what the dongle draws when idle but plugged in.
- USB-A voltage under dongle plus bias tee load.
- CPU load and dropped samples at 1.024, 2.048 and 2.4 MS/s.
- Spectrum frame rate and ZeroShell's own CPU use while drawing the waterfall over SPI.
- Memory used by `zrfd` and by each wrapped decoder.
- Noise from Wi-Fi, the hub and the screen, with the dongle plugged in directly and on a short lead.
- V4 temperature after 30 minutes against the case.
- Whether trixie's rtl-sdr package drives the V4 correctly, including HF.

## Decisions needed

1. **Private or public:** the base is private and this repository is public. CI needs a token
   until that changes.
2. **Base hooks:** add the two small hooks to the base, or keep mirroring its build here.
3. **Receive-only:** confirm transmit stays out of scope even for dongles that can.
4. **Boot target:** boot to the normal launcher with RF apps first (recommended), or straight into
   Spectrum.
5. **Base apps to keep:** the whole base set (recommended; they cost nothing when closed), or a
   trimmed RF-only launcher.

## Sources

- Base OS docs: `docs/ARCHITECTURE.md`, `docs/HARDWARE.md`, `docs/PERFORMANCE.md`, `docs/ROADMAP.md`
  in `cardputer-zero`.
- [RTL-SDR Blog V4 datasheet](https://www.rtl-sdr.com/wp-content/uploads/2024/12/RTLSDR_V4_Datasheet_V_1_0.pdf)
  (current draw, bias tee) and [V4 users guide](https://www.rtl-sdr.com/v4) (driver requirement).
- Lite versus Full: [Liliputing](https://liliputing.com/cardputerzero-pocket-computer-combines-the-brains-of-a-raspberry-pi-zero-2w-with-a-tiny-display-keyboard-and-battery/),
  [CNX Software](https://www.cnx-software.com/2026/05/25/cardputerzero-a-raspberry-pi-cm0-pocket-computer-for-makers/).

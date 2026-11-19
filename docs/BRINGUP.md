# First run on a real Cardputer Zero

Nothing in this repository has run on the device or with a real dongle yet. This page is the
shortest path from an unboxed unit to knowing what works, and what to send back when it doesn't.

You need: the Cardputer Zero, a microSD card (the Lite ships without one), the RTL-SDR dongle with
an antenna, Wi-Fi, and a PC on the same network.

## 1. Get a system booting

Use whichever image you trust most tonight:

- **M5Stack's stock image** is the safe choice: it is known to boot. Flash it to the card.
- **The ZeroOS image** from the base repository's *Build image* workflow has itself never booted on
  hardware. Use it if you want to test both at once, and keep a stock card as a fallback.

Boot, join Wi-Fi, and check you can log in from the PC:

```bash
ssh pi@cardputer.local
```

## 2. Copy the kit over and install

The kit is `zeroos-rf-arm64.tar.gz`: the RF home screen, the radio programs, and two scripts.
From the PC:

```bash
scp zeroos-rf-arm64.tar.gz pi@cardputer.local:
```

On the device:

```bash
tar xzf zeroos-rf-arm64.tar.gz
```

```bash
sudo zeroos-rf/install.sh
```

The installer needs the internet. It installs `rtl-sdr`, `rtl-433` and `sox`, lets the normal user
open the dongle, keeps the kernel's TV driver off it, and installs the programs.

- On a **ZeroOS** image it replaces the home screen with the RF edition (the original is kept as
  `/usr/bin/zeroshell.base`) and restarts it.
- On the **stock** image it installs alongside and prints how to start it by hand.

`sudo zeroos-rf/install.sh --uninstall` puts things back.

## 3. Run the check

Plug in the dongle (unplug and replug it if it was already in), attach the antenna, then:

```bash
zeroos-rf-bringup
```

It takes about 4 minutes (`--quick` skips the long tests) and changes nothing. It checks, in
order: the dongle on USB, the driver and whether it recognises the V4, sample loss at three
rates, the radio daemon on the real dongle, a scan of the FM band, CPU and memory while watching
and while listening, power draw idle and receiving, six seconds of the strongest FM station through
the speaker, 30 seconds of sensor decoding and 30 seconds of listening for aircraft.

It also records the facts the plan is waiting on: the battery's design capacity, whether there is
a clock chip, and whether the USB hub can switch port power.

**Send back `~/zeroos-rf-bringup.txt`.** It is the whole output.

```bash
scp pi@cardputer.local:zeroos-rf-bringup.txt .
```

The one thing the script can't judge is whether you heard the radio station. Note that yourself.

## 4. Try the screens

Open **Spectrum**: you should see the FM band if you type a local station's frequency and press
Enter, or press **B** for the band list. **M** cycles listening. Open **Sensors** and wait a minute
or two; most neighbourhoods have a weather station or two on 433.92 MHz (**F** changes band;
915 MHz is common in the Americas). **Scanner** on Airband or Weather radio should stop on
something within a sweep or two. **Aircraft** needs a view of the sky: try it by a window.

## If something fails

| What you see | Likely cause | Try |
| --- | --- | --- |
| "No radio found" | Dongle not seen on USB, or the TV driver holds it | `lsusb` should list `0bda:2838`. Replug. `lsmod \| grep dvb` should print nothing; if it does, rerun the installer and reboot. |
| "Radio: can't open dongle (in use, or no permission)" | Another program has it, or the access rule isn't active | Close other SDR programs; replug the dongle. |
| The driver check doesn't say "RTL-SDR Blog V4" | The distribution's driver is too old for the V4 | Tell me: the image then needs the RTL-SDR Blog driver built in. |
| Spectrum is flat everywhere | No antenna, or gain too low | Check the antenna; press **G** to step through gain. |
| No sound, but the spectrum works | The audio player can't reach the speaker | Run `play -n synth 1 sine 440` on the device. If that is silent too, it is the base system's audio, not the radio. |
| Scanner stops on a channel with nothing on it and stays | The dongle's own centre spike is above the stop line | Press **L** to lock that channel out, and tell me which band: the stop line may need raising. |
| Aircraft stays empty | Indoors, or the stock antenna is the wrong length for 1090 MHz | Go to a window; shorten a telescopic antenna to about 7 cm. Installing `dump1090-mutability` gives a more sensitive decoder, which the app uses when present. |
| "Radio service not available" | The home screen can't start `zrfd` | Run `zrfd --source rtlsdr` in a terminal and send what it prints. |
| The home screen doesn't come up | A base OS or display problem, not RF | Everything in step 3 still works over SSH; send the report anyway. |
| Device resets or the dongle drops out when it starts | USB port can't supply the dongle | Try a short USB extension lead, another port, or run from USB-C power. |

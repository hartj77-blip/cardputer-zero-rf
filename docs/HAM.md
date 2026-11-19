# For amateur radio operators

ZeroOS RF is a **receiver**. An RTL-SDR dongle cannot transmit, and nothing here keys a
transmitter, so no licence is needed to use it and none of it puts you on the air. What follows
is what a licensed operator will look for, what is there, and what is not.

Nothing on this page has been tried on real hardware yet.

## First: Station

Open **Station** and set:

| Setting | What it does |
| --- | --- |
| Callsign | Shown in Station's title. Kept for features that will need it (logging). |
| Grid locator | 4 or 6 characters (`FN31` or `FN31pr`). APRS shows each station's distance and direction from it. |
| Frequency correction | The dongle's crystal error in ppm. An RTL-SDR Blog V4 is within about 1 ppm, so 0 is usually right. To measure it, tune a signal of known frequency (a NOAA weather channel, a beacon) and adjust until it sits on the centre line. |
| Antenna power | The V4's bias tee: 4.5 V up the coax for a masthead amplifier, up to 180 mA. **Leave it off unless the antenna needs it**; into a DC-shorted antenna it does nothing useful and costs battery. Takes effect the next time an RF app opens. |
| APRS frequency | In kHz: 144390 in the Americas, 144800 in Europe. |
| Rig model, port, speed | For Hamlib; see Rig control below. |

## Bands and modes

**B** in Spectrum lists every amateur band the dongle covers: 160, 80, 60, 40, 30, 20, 17, 15, 12,
10 and 6 m, 2 m, 1.25 m, 70 cm, 33 cm and 23 cm. Band edges follow **ITU Region 2** (the
Americas); a few differ elsewhere (40 m ends at 7.2 MHz in Regions 1 and 3, for example). The
edges only label the display; nothing is blocked.

The first press of **M** picks the conventional mode for where you are:

| Where | Mode |
| --- | --- |
| Bottom fifth of an HF band | CW |
| Rest of 160, 80, 40 m | LSB |
| 60 m and 20 m upward, and 6 m | USB |
| 30 m | CW (no voice there) |
| 2 m, 1.25 m, 70 cm, 33 cm, 23 cm | Narrow FM |

Press **M** again to step through the others. Tuning steps go down to 10 Hz (**Up**/**Down**),
and the frequency shows to 10 Hz.

- **CW**: a carrier on the displayed frequency is heard at 700 Hz through a filter about 500 Hz
  wide.
- **SSB**: 200 to 3000 Hz from the displayed (suppressed carrier) frequency.
- **HF needs the right dongle.** The RTL-SDR Blog V4 covers 500 kHz upward with its built-in
  upconverter. Older dongles start near 24 MHz. An 8-bit receiver on a short antenna will hear
  strong HF stations, not weak ones; a proper HF antenna helps far more than gain.

The bottom line of Spectrum names the band you are in and, while listening, shows the signal's
strength in dB above the noise floor. It is not calibrated in S-units.

## Rig control and panadapter

Spectrum can follow your transceiver's dial through Hamlib, which makes it a panadapter for the
rig, and can send its own frequency to the rig. **Frequency only. There is no PTT and no transmit
control.**

1. Connect the rig's CAT port (USB) to the Cardputer.
2. In Station set **Rig model** to the rig's Hamlib number, **Rig port** to its serial device
   (usually `/dev/ttyUSB0` or `/dev/ttyACM0`), and **Rig speed** to the rig's CAT baud rate (0
   uses the model's default).
3. In Spectrum press **F** to follow the rig, and **T** to send the displayed frequency to it.

Find the model number in a terminal with `rigctl -l | grep -i <name>`. Some common ones (check
against your installed Hamlib, the list grows): Icom IC-7300 3073, IC-705 3085, IC-9700 3081;
Yaesu FT-991A 1035, FT-891 1036, FT-817/818 1020/1041; Kenwood TS-590SG 2037; Xiegu G90 3088.
Model 1 is a pretend rig for trying this out with nothing connected.

For a true panadapter the dongle must hear what the rig hears: either from the rig's IF output
(set the daemon's `offset` to the IF frequency; this has no screen setting yet) or from the same
antenna through a splitter or a receive-antenna port. **Never connect the dongle to an antenna
you transmit into without a transmit/receive switch or protection: transmit power will destroy
it.**

Rig control listens only on the device itself, not on the network.

## APRS

**APRS** decodes 1200 baud packet on the APRS frequency and lists stations with distance and
bearing from your grid, plus weather, status and messages. **F** steps through 144.390, 144.800,
145.175, 144.575 and 145.825 MHz (the ISS and satellite digipeaters). Plain and compressed
positions are decoded; Mic-E positions (many mobile rigs) are listed but not placed yet. It
receives only: it does not beacon, digipeat or gate to the internet.

## Also useful

- **Scanner** on 2 m, 1.25 m and 70 cm: stops on active simplex and repeater outputs and logs them.
- **SDR Server**: use the Cardputer and dongle as a remote receiver for SDR++ or GQRX on a PC.
- **Recording**: **R** in Spectrum saves raw IQ with a SigMF description for later analysis.
- **Mesh** and **GPS** (base OS): Meshtastic over LoRa with the Cap LoRa-1262.

## Not there yet

- Transmitting of any kind, PTT, or split/offset handling for repeaters.
- FT8, FT4, RTTY, PSK31 and other digital modes.
- A logbook with ADIF export.
- Mic-E position decoding, and sending APRS.
- Band edges for Regions 1 and 3, and licence-class sub-bands.
- 50 versus 75 microsecond broadcast FM de-emphasis as a setting (it is 75, the Americas' value).
- S-meter calibration.

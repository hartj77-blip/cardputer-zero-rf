# ZeroOS RF kit

Radio apps for the M5Stack Cardputer Zero with an RTL-SDR dongle: Spectrum, Scanner, Sensors,
Aircraft, APRS, SDR Server and Station. Receive only. Amateur operators: see docs/HAM.md in the
repository, and set your callsign and grid in Station first.

## Before you start

The Cardputer needs a working system on its microSD card, Wi-Fi, and SSH turned on. If the card is
blank (the Lite ships without one), flash it first:

1. Install **M5 Imager** from https://github.com/CardputerZero/m5stack-imager/releases
2. Put a microSD card of 16 GB or more in a card reader. It will be erased.
3. In M5 Imager choose **CardputerZero**, the newest image, and the card.
4. Set a username (`pi` is assumed below) and password, your Wi-Fi, and **enable SSH**.
5. Write, eject, insert the card with the Cardputer off, and slide the power switch to the right.
6. Read the device's IP address from its *IP Panel* or *Settings*.

Full steps and troubleshooting: docs/FLASHING.md in the repository
(https://github.com/hartj77-blip/cardputer-zero-rf).

## Install

Copy this kit to the device from your computer (use the device's address):

    scp zeroos-rf-arm64.tar.gz pi@192.168.1.57:

On the device, over SSH:

    tar xzf zeroos-rf-arm64.tar.gz
    sudo zeroos-rf/install.sh

The installer needs the internet. It installs the SDR packages, lets you open the dongle without
root, keeps the kernel's TV driver off it, and installs the programs.

- On a ZeroOS image it stops: there, install RF Tools from the Apps app instead.
- On M5Stack's stock image it installs alongside. `zeroos-rf-start` shows the RF home screen on
  the built-in screen (log out and in once first, so new group memberships apply);
  `zeroos-rf-stop`, or Ctrl-C, brings M5's launcher back.

## Check

Plug in the dongle and antenna. With no RF app open on the device, run:

    zeroos-rf-bringup

It takes about 4 minutes and changes nothing. Send back ~/zeroos-rf-bringup.txt.

## Remove

    sudo zeroos-rf/install.sh --uninstall

## What's in here

    bin/zeroshell         the RF home screen (absent in a radio-only kit)
    bin/zrfd              the radio service
    bin/zrf               command-line client: zrf send tune 98300000, zrf watch, zrf listen wfm
    bin/zrf-sim-*         stand-ins used when no radio is attached
    install.sh            installer
    bringup.sh            first-run check (installed as zeroos-rf-bringup)
    VERSION               the source revision this was built from

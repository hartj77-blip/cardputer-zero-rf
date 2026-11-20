#!/usr/bin/env bash
# Install ZeroOS RF onto a Cardputer Zero. Run on the device, from the
# unpacked kit:
#   sudo ./install.sh              install
#   sudo ./install.sh --uninstall  put things back
# Works on a ZeroOS image (replaces its ZeroShell with the RF edition) and on
# M5Stack's stock image (installs alongside; start it by hand).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
USER_NAME=${SUDO_USER:-pi}

if [ "${1:-}" = "--uninstall" ]; then
    if [ -f /usr/bin/zeroshell.base ]; then
        mv /usr/bin/zeroshell.base /usr/bin/zeroshell
        systemctl restart zeroshell.service || true
    fi
    rm -f /usr/local/bin/zrfd /usr/local/bin/zrf /usr/local/bin/zrf-sim-sensors /usr/local/bin/zrf-sim-aircraft /usr/local/bin/zrf-sim-aprs /usr/local/bin/zeroshell-rf \
          /usr/local/bin/zeroos-rf-bringup /usr/local/bin/zeroos-rf-start /usr/local/bin/zeroos-rf-stop /etc/udev/rules.d/60-zeroos-rf.rules /etc/modprobe.d/zeroos-rf-blacklist.conf
    echo "ZeroOS RF removed. Packages (rtl-sdr, rtl-433, multimon-ng, Hamlib, sox) were left installed."
    exit 0
fi

echo "== Packages"
apt-get update || echo "WARNING: could not refresh the package list; carrying on"
# One at a time, so a package this image doesn't have can't stop the rest.
MISSING=""
for pkg in rtl-sdr rtl-433 multimon-ng libhamlib-utils sox alsa-utils usbutils uhubctl; do
    apt-get install -y "$pkg" || MISSING="$MISSING $pkg"
done
[ -z "$MISSING" ] || echo "WARNING: could not install:$MISSING"
command -v rtl_test >/dev/null || echo "WARNING: rtl-sdr is not installed, so the radio programs cannot run yet."

echo "== Dongle access"
install -m 0644 "$HERE/60-zeroos-rf.rules" /etc/udev/rules.d/60-zeroos-rf.rules
install -m 0644 "$HERE/zeroos-rf-blacklist.conf" /etc/modprobe.d/zeroos-rf-blacklist.conf
usermod -aG audio,video,input "$USER_NAME" || true
modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true
udevadm control --reload-rules && udevadm trigger || true

echo "== Programs"
for f in zrfd zrf zrf-sim-sensors zrf-sim-aircraft zrf-sim-aprs; do
    [ -f "$HERE/bin/$f" ] && install -m 0755 "$HERE/bin/$f" /usr/local/bin/$f
done
install -m 0755 "$HERE/bringup.sh" /usr/local/bin/zeroos-rf-bringup
# The radio program must start here. If it was built for another system it
# stops with a missing-library message, and nothing else will work.
if ! /usr/local/bin/zrfd --help >/dev/null 2>/tmp/zrfd-check.txt; then
    echo
    echo "ERROR: the radio program will not start on this system:"
    sed 's/^/    /' /tmp/zrfd-check.txt
    echo "Send this message back; the kit needs rebuilding for this system."
    exit 1
fi

if [ ! -f "$HERE/bin/zeroshell" ]; then
    echo "No zeroshell in this kit: radio programs only."
elif [ -f /etc/systemd/system/zeroshell.service ]; then
    # ZeroOS image: swap the home screen for the RF edition, keeping the original.
    [ -f /usr/bin/zeroshell.base ] || cp /usr/bin/zeroshell /usr/bin/zeroshell.base
    install -m 0755 "$HERE/bin/zeroshell" /usr/bin/zeroshell
    systemctl restart zeroshell.service
    echo "ZeroShell replaced with the RF edition and restarted."
else
    # Stock image: install alongside M5's launcher, with a pair of commands to
    # swap between the two. Both draw on the built-in screen, so only one runs.
    install -m 0755 "$HERE/bin/zeroshell" /usr/local/bin/zeroshell-rf
    UNIT=$(systemctl list-unit-files --type=service --no-legend 2>/dev/null | awk '{print $1}' | grep -i -m1 -E 'applaunch|launcher' || true)
    cat > /usr/local/bin/zeroos-rf-start <<START
#!/bin/sh
# Stop M5's launcher and show the ZeroOS RF home screen. Ctrl-C (or
# zeroos-rf-stop from another login) brings the launcher back.
UNIT="$UNIT"
if [ -n "\$UNIT" ]; then sudo systemctl stop "\$UNIT"; else sudo pkill -i -f applaunch; fi
trap 'zeroos-rf-stop' EXIT INT TERM
# Runs as you, not root: the screen and keyboard are open to the video and
# input groups. If it reports it can't open them, send that message back.
ZS_FRAMEBUFFER=/dev/fb0 /usr/local/bin/zeroshell-rf
START
    cat > /usr/local/bin/zeroos-rf-stop <<STOP
#!/bin/sh
# Close the ZeroOS RF home screen and bring M5's launcher back.
UNIT="$UNIT"
pkill -x zeroshell-rf
pkill -x zrfd
if [ -n "\$UNIT" ]; then sudo systemctl start "\$UNIT"; else echo "Restart the device to get M5's launcher back."; fi
STOP
    chmod 0755 /usr/local/bin/zeroos-rf-start /usr/local/bin/zeroos-rf-stop
    echo "Stock image: the RF home screen is installed next to M5's launcher${UNIT:+ ($UNIT)}."
    [ -n "$UNIT" ] || echo "NOTE: could not find M5's launcher service by name; zeroos-rf-start will stop it by process name instead."
    echo "To show the RF home screen on the built-in screen:   zeroos-rf-start"
    echo "To go back to M5's launcher:                         zeroos-rf-stop"
fi

echo
echo "Done. Unplug and replug the dongle, then run:"
echo "    zeroos-rf-bringup"

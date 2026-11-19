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
    rm -f /usr/local/bin/zrfd /usr/local/bin/zrf /usr/local/bin/zrf-sim-sensors /usr/local/bin/zrf-sim-aircraft /usr/local/bin/zeroshell-rf \
          /usr/local/bin/zeroos-rf-bringup /etc/udev/rules.d/60-zeroos-rf.rules /etc/modprobe.d/zeroos-rf-blacklist.conf
    echo "ZeroOS RF removed. Packages (rtl-sdr, rtl-433, sox) were left installed."
    exit 0
fi

echo "== Packages"
apt-get update
apt-get install -y rtl-sdr librtlsdr2 rtl-433 sox alsa-utils usbutils
apt-get install -y uhubctl || echo "uhubctl not available; the USB power check will be skipped"

echo "== Dongle access"
install -m 0644 "$HERE/60-zeroos-rf.rules" /etc/udev/rules.d/60-zeroos-rf.rules
install -m 0644 "$HERE/zeroos-rf-blacklist.conf" /etc/modprobe.d/zeroos-rf-blacklist.conf
usermod -aG audio "$USER_NAME" || true
modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true
udevadm control --reload-rules && udevadm trigger || true

echo "== Programs"
for f in zrfd zrf zrf-sim-sensors zrf-sim-aircraft; do
    [ -f "$HERE/bin/$f" ] && install -m 0755 "$HERE/bin/$f" /usr/local/bin/$f
done
install -m 0755 "$HERE/bringup.sh" /usr/local/bin/zeroos-rf-bringup

if [ ! -f "$HERE/bin/zeroshell" ]; then
    echo "No zeroshell in this kit: radio programs only."
elif [ -f /etc/systemd/system/zeroshell.service ]; then
    # ZeroOS image: swap the home screen for the RF edition, keeping the original.
    [ -f /usr/bin/zeroshell.base ] || cp /usr/bin/zeroshell /usr/bin/zeroshell.base
    install -m 0755 "$HERE/bin/zeroshell" /usr/bin/zeroshell
    systemctl restart zeroshell.service
    echo "ZeroShell replaced with the RF edition and restarted."
else
    install -m 0755 "$HERE/bin/zeroshell" /usr/local/bin/zeroshell-rf
    cat <<MSG
Stock image: the RF home screen is installed as /usr/local/bin/zeroshell-rf.
To try it, stop M5's launcher and start ours on the built-in screen:
    systemctl --user stop APPLaunch 2>/dev/null || sudo systemctl stop APPLaunch
    zeroshell-rf
MSG
fi

echo
echo "Done. Unplug and replug the dongle, then run:"
echo "    zeroos-rf-bringup"

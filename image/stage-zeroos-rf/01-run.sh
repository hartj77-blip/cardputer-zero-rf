#!/bin/bash -e
# ZeroOS RF stage: the radio programs on top of the ZeroOS stage, which has
# already installed our build of ZeroShell as /usr/bin/zeroshell.
# files/ is filled in by image/build.sh.

install -m 0755 files/zrfd files/zrf files/zrf-sim-sensors files/zrf-sim-aircraft files/zrf-sim-aprs "${ROOTFS_DIR}/usr/bin/"
install -m 0755 files/bringup.sh               "${ROOTFS_DIR}/usr/bin/zeroos-rf-bringup"
# Let the normal user open an RTL-SDR dongle, and keep the kernel's TV driver off it.
install -m 0644 files/60-zeroos-rf.rules       "${ROOTFS_DIR}/etc/udev/rules.d/60-zeroos-rf.rules"
install -m 0644 files/zeroos-rf-blacklist.conf "${ROOTFS_DIR}/etc/modprobe.d/zeroos-rf-blacklist.conf"
install -D -m 0644 files/THIRD_PARTY.md        "${ROOTFS_DIR}/usr/share/doc/zeroos-rf/THIRD_PARTY.md"
# Over-the-air updates: the base's Update app installs the plain ZeroOS package
# from cardputer-zero's releases, which would replace this RF home screen. Until
# RF publishes its own update package, look for updates here instead, where
# there are none yet: the Update app then reports no update rather than
# installing the wrong one.
if [ -f "${ROOTFS_DIR}/etc/zeroos/update.conf" ]; then
    sed -i 's|^UPDATE_URL=.*|UPDATE_URL=https://github.com/hartj77-blip/cardputer-zero-rf/releases/latest/download|' "${ROOTFS_DIR}/etc/zeroos/update.conf"
    grep -q 'cardputer-zero-rf/releases' "${ROOTFS_DIR}/etc/zeroos/update.conf"
fi
echo "ZeroOS RF @ZEROOS_RF_VERSION@ (built $(date -u +%Y-%m-%d))" > "${ROOTFS_DIR}/etc/zeroos-rf-release"

on_chroot << CHROOT
# The radio programs must start: a missing library would only show up on the device.
/usr/bin/zrfd --help >/dev/null
/usr/bin/zrf 2>&1 | grep -q usage
# rtl-sdr's own TV-driver blacklist and ours must both be in the early boot image.
update-initramfs -u -k all || true
CHROOT

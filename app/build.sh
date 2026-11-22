#!/usr/bin/env bash
# Pack the RF apps as one ZeroOS app (zeroos-app-rf_<version>_arm64.deb) and
# write zeroos-apps.index, which the Apps app on the device reads from this
# repository's newest release. Uses the base OS's packer (base/image/apps).
#
# Usage: app/build.sh [device-build-dir] [out-dir]
#   device-build-dir: cmake -S shell -B <dir> -DZS_BACKEND=device (default build/device)
# Env:   ZEROOS_RF_VERSION (default: app/version.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD=$(realpath "${1:-$ROOT/build/device}")
OUT=$(realpath -m "${2:-$ROOT/build/app}")
VERSION=${ZEROOS_RF_VERSION:-$("$HERE/version.sh")}
ABI=$(sed -n 's/^#define ZS_APP_ABI \([0-9]*\).*/\1/p' "$ROOT/base/shell/src/ui/app.h")
[ -n "$ABI" ] || { echo "error: no ZS_APP_ABI in base/shell/src/ui/app.h (git submodule update --init base)" >&2; exit 1; }
[ -f "$BUILD/apps/rf.so" ] || { echo "error: no $BUILD/apps/rf.so (a device build makes it)" >&2; exit 1; }
[ -x "$BUILD/rf/zrfd" ] || { echo "error: no $BUILD/rf/zrfd" >&2; exit 1; }
ARCH=${ZEROOS_ARCH:-$(dpkg --print-architecture)}

# The device runs Debian trixie, whose RTL-SDR library is librtlsdr.so.0. A
# program built on Ubuntu wants librtlsdr.so.2 and will not start there.
NEEDED=$(readelf -d "$BUILD/rf/zrfd" | sed -n 's/.*NEEDED.*\[\(librtlsdr[^]]*\)\].*/\1/p')
if [ -n "$NEEDED" ] && [ "$NEEDED" != "librtlsdr.so.0" ]; then
    echo "error: zrfd is linked to $NEEDED, but the device has librtlsdr.so.0. Build on Debian trixie (CI does)." >&2
    exit 1
fi
[ -n "$NEEDED" ] || echo "note: this zrfd has no RTL-SDR support built in" >&2

T=$(mktemp -d)
trap 'rm -rf -- "${T:?}"' EXIT
mkdir -p "$T/bin" "$T/mod" "$OUT"
for p in zrfd zrf zrf-sim-sensors zrf-sim-aircraft zrf-sim-aprs; do
    cp "$BUILD/rf/$p" "$T/bin/"
    strip "$T/bin/$p"
done
cp "$BUILD/apps/rf.so" "$T/mod/"
strip --strip-unneeded "$T/mod/rf.so"
rm -f -- "${OUT:?}"/zeroos-app-rf_*.deb "${OUT:?}/zeroos-apps.index"
{
    echo "# ZeroOS RF apps $VERSION. Read by zeroos-apps on the device."
    echo
    BIN=$T/bin ZEROOS_FILES=$ROOT/device ZEROOS_APP_ABI=$ABI ZEROOS_ARCH=$ARCH \
        "$ROOT/base/image/apps/make-app-deb.sh" "$HERE/rf.pkg" "$OUT" "$VERSION" "$T/mod/rf.so"
} > "$OUT/zeroos-apps.index"
ls -l "$OUT"
cat "$OUT/zeroos-apps.index"

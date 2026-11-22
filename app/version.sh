#!/usr/bin/env bash
# Print the RF app version of this checkout in a form dpkg can compare.
#   tag v1.2.3                 -> 1.2.3
#   3 commits after v1.2.3     -> 1.2.3+3.gabc1234
#   no release tag yet         -> 0.0.0+<commits>.gabc1234
# ".dirty" is added when files are changed but not committed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
d=$(git -C "$ROOT" describe --tags --match 'v[0-9]*' --long --dirty 2>/dev/null || true)
if [ -n "$d" ]; then
    dirty=""; case "$d" in *-dirty) dirty=.dirty; d=${d%-dirty} ;; esac
    sha=${d##*-}; d=${d%-*}; n=${d##*-}; tag=${d%-*}
    v=${tag#v}
    [ "$n" = 0 ] || v="$v+$n.$sha"
    echo "$v$dirty"
else
    n=$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 0)
    sha=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
    dirty=""; [ -z "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] || dirty=.dirty
    echo "0.0.0+$n.g$sha$dirty"
fi

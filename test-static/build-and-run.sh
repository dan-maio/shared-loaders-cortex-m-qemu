#!/usr/bin/env bash
# build-and-run.sh — dispatch to one of the static-loader test apps.
#
#   test-static/build-and-run.sh <target>
#
# Target names are the subdirectory names with the `test-` prefix dropped:
#
#   test-static/test-static-direct  ->  static-direct
#   test-static/test-drv            ->  drv
#
# Each of these projects carries its own platform tree and toolchain file, so it
# owns its build-and-run script; this one only forwards.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Target board (nucleo|weact|qemu) is exported so the per-app build-and-run scripts
# pick the right platform init + runner.
export SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu (got '$SCL_BOARD')" >&2; exit 1 ;; esac

# target -> subdirectory (relative to test-static/)
declare -A TARGETS=(
  [static]="test-static"
  [static-cpp]="test-static-cpp"
  [static-direct]="test-static-direct"
  [drv]="test-drv"
)

usage() {
    echo "Usage: $(basename "$0") <board> <target>" >&2
    echo "Targets (subdirectory name without the test- prefix):" >&2
    for t in $(printf '%s\n' "${!TARGETS[@]}" | sort); do
        printf '  %-16s test-static/%s\n' "$t" "${TARGETS[$t]}" >&2
    done
}

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then usage; exit 1; fi

# Optional leading <board> positional (nucleo|weact|qemu); overrides SCL_BOARD env.
if [[ "$1" == "nucleo" || "$1" == "weact" || "$1" == "qemu" ]]; then
    export SCL_BOARD="$1"
    shift
    [[ $# -ge 1 ]] || { usage; exit 1; }
fi
TARGET="$1"
[[ -v TARGETS[$TARGET] ]] || { echo "error: unknown target '$TARGET'" >&2; usage; exit 1; }

SUBDIR="$HERE/${TARGETS[$TARGET]}"
[[ -d "$SUBDIR" ]] || { echo "error: $SUBDIR does not exist" >&2; exit 1; }

for s in build-and-run.sh build-and-run-cmake.sh; do
    if [[ -x "$SUBDIR/$s" ]]; then
        echo "=== $TARGET -> ${TARGETS[$TARGET]}/$s ==="
        cd "$SUBDIR" && exec "./$s" "${@:2}"
    fi
done
echo "error: no build-and-run script in $SUBDIR" >&2
exit 1

#!/usr/bin/env bash
# build-and-run.sh — configure + build the XIP test tree, flash one app onto the
# WeAct BlackPill STM32F411CE, run it, and capture the semihosting console to a
# logfile (non-interactive: OpenOCD runs the firmware briefly, then shuts down).
#
#   test-xip/build-and-run.sh [<board>] <target>
#
# Target names are the subdirectory names with the `test-xip-` prefix dropped, so
# the option is the path to the test:
#
#   test-xip/test-xip-static/spi-flash  ->  static/spi-flash
#   test-xip/test-xip-interrupt-static  ->  interrupt-static
#
# The captured console is printed and also left in the build tree as run.log.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
ROOT="$HERE/.."                          # repo root
BUILD="$HERE/build"
TOOLCHAIN="cmake/arm-none-eabi.cmake"
EXTRA_CMAKE_OPTS=""
RUN_MS="${RUN_MS:-300000}"                 # 5 minutes (provisioning + tests)

# target -> "cmake-target : subdirectory (relative to test-xip/)"
declare -A TARGETS=(
  [dynamic-nolib]="scl-fw-xip-nolib1:test-xip-dynamic-nolib/test1"
  [dynamic-newlib]="scl-fw-xip-newlib1:test-xip-dynamic-newlib/test1"
  [interrupt]="scl-fw-xip-interrupt1:test-xip-interrupt/test1"
  [static/semihosting]="scl-fw-xip-static-semihosting:test-xip-static/semihosting"
  [static/spi-flash]="scl-fw-xip-static-spi-flash:test-xip-static/spi-flash"
  [static-direct]="scl-fw-xip-static-direct:test-xip-static-direct"
  [interrupt-static]="scl-fw-xip-interrupt-static:test-xip-interrupt-static"
  [interrupt-static-direct]="scl-fw-xip-interrupt-static-direct:test-xip-interrupt-static-direct"
)

usage() {
    echo "Usage: $(basename "$0") <board> <target>" >&2
    echo "Targets (subdirectory name without the test-xip- prefix):" >&2
    for t in $(printf '%s\n' "${!TARGETS[@]}" | sort); do
        printf '  %-24s test-xip/%s\n' "$t" "${TARGETS[$t]#*:}" >&2
    done
}

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then usage; exit 1; fi

# Optional leading <board> positional (nucleo|weact); overrides SCL_BOARD env.
if [[ "$1" == "nucleo" || "$1" == "weact" ]]; then
    export SCL_BOARD="$1"
    shift
    [[ $# -ge 1 ]] || { usage; exit 1; }
fi
TARGET="$1"
[[ -v TARGETS[$TARGET] ]] || { echo "error: unknown target '$TARGET'" >&2; usage; exit 1; }

# Target board selects the OpenOCD flash config: NUCLEO-F411RE = ST-LINK,
# WEACT-F411CE = CMSIS-DAP (see test/platforms/board/openocd-<board>.cfg).
# Resolved AFTER the positional <board> override above.
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo|weact) ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
CFG="$ROOT/test/platforms/board/openocd-${SCL_BOARD}.cfg"

APP="${TARGETS[$TARGET]%%:*}"
SUBDIR="${TARGETS[$TARGET]#*:}"
[[ -d "$HERE/$SUBDIR" ]] || { echo "error: $HERE/$SUBDIR does not exist" >&2; exit 1; }

if [ "$TARGET" = "dynamic-newlib" ]; then
    BUILD="$HERE/build-newlib"
    TOOLCHAIN="cmake/arm-none-eabi-hosted.cmake"
    EXTRA_CMAKE_OPTS="-DSCL_HF=ON -DSCL_HOSTED=ON"
fi
LOG="$BUILD/run.log"

# static/spi-flash builds standalone (like test-drv) — delegate and exit
if [ "$TARGET" = "static/spi-flash" ]; then
    exec "$HERE/test-xip-static/spi-flash/build-and-run.sh"
fi

echo "=== build XIP fixtures (split, single-pic-base) ==="
"$HERE/fixtures-src/build-fixtures.sh"
if [ "$TARGET" = "static/semihosting" ] || [ "$TARGET" = "static-direct" ] || [ "$TARGET" = "interrupt-static" ] || [ "$TARGET" = "interrupt-static-direct" ]; then
    "$HERE/test-xip-static/semihosting/fixtures-src/build.sh"
fi

echo "=== configure (copies baseline .so from test/fixtures) ==="
rm -rf "$BUILD"
mkdir -p "$BUILD"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/$TOOLCHAIN" -DSCL_BOARD="$SCL_BOARD" $EXTRA_CMAKE_OPTS

echo "=== rebuild all libs with XIP split segments (r10-based PIC) ==="
"$HERE/fixtures-src/build-xip-libs.sh"

echo "=== build firmware ==="
cmake --build "$BUILD" --target "$APP" -j

HEX="$(find "$BUILD" -name "$APP.hex" | head -1)"
[[ -n "$HEX" && -f "$HEX" ]] || { echo "error: $APP.hex not built" >&2; exit 1; }

echo "=== flash + run $APP (semihosting console -> $LOG) ==="
killall openocd 2>/dev/null || true
"$OPENOCD" -s "$OPENOCD_SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "flash erase_address 0x08040000 0x40000" \
    -c "program $HEX verify" \
    -c "arm semihosting enable" \
    -c "reset run" \
    -c "sleep $RUN_MS" \
    -c "shutdown" 2>&1 | tee "$LOG"

echo "=== console captured at $LOG ==="

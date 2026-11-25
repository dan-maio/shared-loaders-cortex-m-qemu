#!/usr/bin/env bash
# build-and-run.sh — configure, build, flash and run an overlay test app on
# hardware (WeAct/Nucleo via OpenOCD) or under QEMU.
# Ctrl-C to stop; the semihosting console prints directly to stdout.
#
#   test-overlay/build-and-run.sh [<board>] <target>
#
# Target names are the subdirectory names, so the option is the path to the test:
#
#   test-overlay/test1  ->  test1
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
ROOT="$HERE/.."
export SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu (got '$SCL_BOARD')" >&2; exit 1 ;; esac

# target -> "cmake-target : subdirectory (relative to test-overlay/)"
declare -A TARGETS=(
  [test1]="scl-fw-ovl1:test1"
)

usage() {
    echo "Usage: $(basename "$0") [<board>] <target>" >&2
    echo "Targets (subdirectory name):" >&2
    for t in $(printf '%s\n' "${!TARGETS[@]}" | sort); do
        printf '  %-12s test-overlay/%s\n' "$t" "${TARGETS[$t]#*:}" >&2
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

APP="${TARGETS[$TARGET]%%:*}"
SUBDIR="${TARGETS[$TARGET]#*:}"
[[ -d "$HERE/$SUBDIR" ]] || { echo "error: $HERE/$SUBDIR does not exist" >&2; exit 1; }

IS_QEMU=0
if [[ "$SCL_BOARD" == "qemu" ]]; then
    IS_QEMU=1
    QEMU_RUN="$ROOT/test/tools/qemu-run.sh"
    MACHINE=netduinoplus2
    # Per-app QEMU runner arguments
    declare -A QEMU_ARGS=(
      [test1]="--done 'overlay: done'"
    )
fi

BUILD="$HERE/build"
echo "=== configure ($BUILD) ==="
if [[ $IS_QEMU -eq 1 ]]; then
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
          -DSCL_PLATFORM=qemu-f411re
else
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" -DSCL_PLATFORM=board -DSCL_BOARD="$SCL_BOARD"
fi

echo "=== build $APP ==="
cmake --build "$BUILD" --target "$APP" -j

if [[ $IS_QEMU -eq 1 ]]; then
    ELF="$(find "$BUILD" -type f -name "$APP" | head -1)"
    [[ -n "$ELF" && -f "$ELF" ]] || { echo "error: $APP ELF not found" >&2; exit 1; }
    echo "=== run $APP under QEMU ==="
    eval exec "\"$QEMU_RUN\"" --machine "\"$MACHINE\"" "${QEMU_ARGS[$TARGET]}" "\"$ELF\""
else
    echo "=== flash and run $APP ==="
    exec "$ROOT/test/tools/flash-run.sh" "$APP"
fi

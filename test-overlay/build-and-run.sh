#!/usr/bin/env bash
# build-and-run.sh — configure, build, flash and run an overlay test app on the
# WeAct BlackPill STM32F411CE. Ctrl-C to stop; the semihosting console prints
# directly to stdout.
#
#   test-overlay/build-and-run.sh <target>
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
case "$SCL_BOARD" in nucleo|weact) ;; *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;; esac

# target -> "cmake-target : subdirectory (relative to test-overlay/)"
declare -A TARGETS=(
  [test1]="scl-fw-ovl1:test1"
)

usage() {
    echo "Usage: $(basename "$0") <target>" >&2
    echo "Targets (subdirectory name):" >&2
    for t in $(printf '%s\n' "${!TARGETS[@]}" | sort); do
        printf '  %-12s test-overlay/%s\n' "$t" "${TARGETS[$t]#*:}" >&2
    done
}

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then usage; exit 1; fi
TARGET="$1"
[[ -v TARGETS[$TARGET] ]] || { echo "error: unknown target '$TARGET'" >&2; usage; exit 1; }

APP="${TARGETS[$TARGET]%%:*}"
SUBDIR="${TARGETS[$TARGET]#*:}"
[[ -d "$HERE/$SUBDIR" ]] || { echo "error: $HERE/$SUBDIR does not exist" >&2; exit 1; }

BUILD="$HERE/build"
echo "=== configure ($BUILD) ==="
cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"

echo "=== build $APP ==="
cmake --build "$BUILD" --target "$APP" -j

echo "=== flash and run $APP ==="
exec "$ROOT/test/tools/flash-run.sh" "$APP"

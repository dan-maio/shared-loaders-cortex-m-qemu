#!/usr/bin/env bash
# build-and-run.sh — configure, build, flash and run one dynamic-loader test app
# on the WeAct BlackPill STM32F411CE. Ctrl-C to stop; the semihosting console
# prints directly to stdout.
#
#   test-dynamic/build-and-run.sh <target>
#
# Target names are the subdirectory names with the `test-` prefix dropped, so the
# option is the path to the test:
#
#   test-dynamic/test-dynamic-nolib/test1  ->  dynamic-nolib/test1
#   test-dynamic/test-flatfs-spi           ->  flatfs-spi
#
# The newlib apps build twice, once per float ABI; pick with SCL_ABI=sf|hf
# (default sf). The others are hard-float freestanding.
# SCL_BOARD=nucleo|weact selects the target board (default weact); it is passed
# to CMake (unified board platform) and to flash-run.sh (OpenOCD flash config).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
ROOT="$HERE/.."
export SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact) ;; *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;; esac

# target -> "cmake-target : subdirectory (relative to test-dynamic/)"
declare -A TARGETS=(
  [dynamic-nolib/test1]="scl-fw-dyn1:test-dynamic-nolib/test1"
  [dynamic-nolib/test2]="scl-fw-dyn2:test-dynamic-nolib/test2"
  [dynamic-nolib/test3]="scl-fw-flatfs:test-dynamic-nolib/test3"
  [dynamic-nolib/test4]="scl-fw-gotplt-demo:test-dynamic-nolib/test4"
  [dynamic-newlib/test1]="scl-fw-dynamic-newlib1:test-dynamic-newlib/test1"
  [dynamic-newlib/test2]="scl-fw-dynamic-newlib2:test-dynamic-newlib/test2"
  [flatfs-spi]="scl-fw-flatfs-spi:test-flatfs-spi"
  [flatfs-bitbang-spi]="scl-fw-flatfs-bitbang-spi:test-flatfs-bitbang-spi"
)

usage() {
    echo "Usage: $(basename "$0") <target>" >&2
    echo "Targets (subdirectory name without the test- prefix):" >&2
    for t in $(printf '%s\n' "${!TARGETS[@]}" | sort); do
        printf '  %-24s test-dynamic/%s\n' "$t" "${TARGETS[$t]#*:}" >&2
    done
    echo >&2
    echo "  SCL_ABI=sf|hf selects the float ABI for the dynamic-newlib apps (default sf)." >&2
}

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then usage; exit 1; fi
TARGET="$1"
[[ -v TARGETS[$TARGET] ]] || { echo "error: unknown target '$TARGET'" >&2; usage; exit 1; }

APP="${TARGETS[$TARGET]%%:*}"
SUBDIR="${TARGETS[$TARGET]#*:}"
[[ -d "$HERE/$SUBDIR" ]] || { echo "error: $HERE/$SUBDIR does not exist" >&2; exit 1; }

# The SPI-flash apps need the WEACT board wiring (the NUCLEO-F411RE has no SPI).
if [[ "$SCL_BOARD" == "nucleo" ]] && [[ "$TARGET" == flatfs-spi* ]]; then
    echo "error: $TARGET needs SPI hardware — only the WEACT board has it (got SCL_BOARD=nucleo)" >&2
    exit 1
fi

if [[ "$TARGET" == dynamic-newlib/* ]]; then
    ABI="${SCL_ABI:-sf}"
    case "$ABI" in
      sf) BUILD="$ROOT/test/build-dynamic-newlib";    EXTRA=""            ; RUNNER=flash-run-sf.sh ;;
      hf) BUILD="$ROOT/test/build-dynamic-newlib-hf"; EXTRA="-DSCL_HF=ON" ; RUNNER=flash-run-hf.sh ;;
      *)  echo "error: SCL_ABI must be sf or hf (got '$ABI')" >&2; exit 1 ;;
    esac
    echo "=== configure: hosted newlib, $ABI ($BUILD) ==="
    cmake -S "$HERE/test-dynamic-newlib" -B "$BUILD" \
          -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi-hosted.cmake" -DSCL_BOARD="$SCL_BOARD" $EXTRA
else
    BUILD="$HERE/build"
    RUNNER=flash-run.sh
    echo "=== configure: freestanding ($BUILD) ==="
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"
fi

echo "=== build $APP ==="
cmake --build "$BUILD" --target "$APP" -j

echo "=== flash and run $APP ==="
exec "$ROOT/test/tools/$RUNNER" "$APP"

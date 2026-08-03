#!/usr/bin/env bash
# build_and_run.sh — configure, build, flash, and run relocatable (ET_REL) test apps on WeAct BlackPill STM32F411CE.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
BUILD="$HERE/build"

show_help() {
    cat <<EOF
Usage: $(basename "$0") <board> <target-variant>

Configure, build, flash, and run the selected relocatable (ET_REL) test firmware.
Ctrl-C to stop. The semihosting console prints directly to stdout.

Valid options (variants/apps):
  reloc          - App 17: 9-check battery (relprov + relmod, cyclic c1 + c2 + cpp3)
  classic        - App 18: Classic relocatable demo with flatfs blob embedding
  classic-spi    - App 18b: Classic demo loading .rel.o from SPI flash (W25Q64)
  interrupt      - App 19: Relocatable loader with dynamic RAM vector patching and TIM2
EOF
}

if [[ $# -eq 0 ]]; then
    show_help
    exit 1
elif [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_help
    exit 0
fi


# Optional leading <board> positional (nucleo|weact); overrides SCL_BOARD env.
if [[ "$1" == "nucleo" || "$1" == "weact" ]]; then
    export SCL_BOARD="$1"
    shift
    [[ $# -ge 1 ]] || { usage; exit 1; }
fi
TARGET="$1"

# Target board selects the unified platform boot/init + the OpenOCD flash
# config (ST-LINK for NUCLEO, CMSIS-DAP for WEACT).
export SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact) ;; *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;; esac

case "$TARGET" in
  reloc|classic|classic-spi|interrupt)
    case "$TARGET" in
      reloc)       T_TARGET="scl-fw-reloc" ;;
      classic)     T_TARGET="scl-fw-classic-reloc" ;;
      classic-spi) T_TARGET="scl-fw-classic-spi-reloc" ;;
      interrupt)   T_TARGET="scl-fw-reloc-interrupt" ;;
    esac

    # The classic-spi app needs SPI hardware — only the WEACT board has it.
    if [[ "$SCL_BOARD" == "nucleo" ]] && [[ "$TARGET" == "classic-spi" ]]; then
        echo "error: classic-spi needs SPI hardware — only the WEACT board has it (got SCL_BOARD=nucleo)" >&2
        exit 1
    fi

    echo "=== configure + build (CMake also builds the ET_REL fixtures) ==="
    cmake -S "$HERE" -B "$BUILD" \
          -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"
    cmake --build "$BUILD" --target "$T_TARGET" -j

    case "$TARGET" in
      reloc)   HEX="$BUILD/scl-fw-reloc.hex" ;;
      classic) HEX="$BUILD/scl-fw-classic-reloc.hex" ;;
      classic-spi) HEX="$BUILD/scl-fw-classic-spi-reloc.hex" ;;
      interrupt) HEX="$BUILD/scl-fw-reloc-interrupt.hex" ;;
    esac

    [[ -f "$HEX" ]] || { echo "error: $HEX not built" >&2; exit 1; }

    echo "=== flash + run $TARGET (semihosting console follows) ==="
    exec "$ROOT/test/tools/flash-run.sh" "$HEX"
    ;;
  *)
    echo "error: unknown target variant '$TARGET'" >&2
    show_help
    exit 1
    ;;
esac

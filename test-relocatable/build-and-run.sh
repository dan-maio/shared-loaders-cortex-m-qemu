#!/usr/bin/env bash
# build_and_run.sh — configure, build, flash and run relocatable (ET_REL) test
# apps on hardware (WeAct/Nucleo via OpenOCD) or under QEMU.
# Ctrl-C to stop. The semihosting console prints directly to stdout.
#
#   test-relocatable/build_and_run.sh [<board>] <target-variant>
#
# Valid options (variants/apps):
#   reloc          - App 17: 9-check battery (relprov + relmod, cyclic c1 + c2 + cpp3)
#   classic        - App 18: Classic relocatable demo with flatfs blob embedding
#   classic-spi    - App 18b: Classic demo loading .rel.o from SPI flash (W25Q64) [WEACT only]
#   interrupt      - App 19: Relocatable loader with dynamic RAM vector patching and TIM2
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

ROOT="$HERE/.."
BUILD="$HERE/build"
export SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu (got '$SCL_BOARD')" >&2; exit 1 ;; esac

show_help() {
    cat <<EOF
Usage: $(basename "$0") [<board>] <target-variant>

Configure, build, flash, and run the selected relocatable (ET_REL) test firmware.
Ctrl-C to stop. The semihosting console prints directly to stdout.

Valid options (variants/apps):
  reloc          - App 17: 9-check battery (relprov + relmod, cyclic c1 + c2 + cpp3)
  classic        - App 18: Classic relocatable demo with flatfs blob embedding
  classic-spi    - App 18b: Classic demo loading .rel.o from SPI flash (W25Q64) [WEACT only]
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

# Optional leading <board> positional (nucleo|weact|qemu); overrides SCL_BOARD env.
if [[ "$1" == "nucleo" || "$1" == "weact" || "$1" == "qemu" ]]; then
    export SCL_BOARD="$1"
    shift
    [[ $# -ge 1 ]] || { show_help; exit 1; }
fi

TARGET="$1"

case "$TARGET" in
  reloc|classic|classic-spi|interrupt)
    case "$TARGET" in
      reloc)       T_TARGET="scl-fw-reloc" ;;
      classic)     T_TARGET="scl-fw-classic-reloc" ;;
      classic-spi) T_TARGET="scl-fw-classic-spi-reloc" ;;
      interrupt)   T_TARGET="scl-fw-reloc-interrupt" ;;
    esac

    # classic-spi needs WEACT SPI wiring
    if [[ "$TARGET" == "classic-spi" && "$SCL_BOARD" != "weact" ]]; then
        echo "error: classic-spi needs SPI hardware — only the WEACT board has it (board=$SCL_BOARD)" >&2
        exit 1
    fi

    IS_QEMU=0
    if [[ "$SCL_BOARD" == "qemu" ]]; then
        IS_QEMU=1
        QEMU_RUN="$ROOT/test/tools/qemu-run.sh"
        MACHINE=netduinoplus2
        declare -A QEMU_ARGS=(
          [reloc]="--done 'BATTERY SUMMARY'"
          [classic]="--done '==== classic demo complete' --allow 'load_set FAILED'"
          [interrupt]="--done 'RESULT: ALL PASS'"
        )
    fi

    echo "=== configure + build (CMake also builds the ET_REL fixtures) ==="
    if [[ $IS_QEMU -eq 1 ]]; then
        cmake -S "$HERE" -B "$BUILD" \
              -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
              -DSCL_PLATFORM=qemu-f411re
    else
        cmake -S "$HERE" -B "$BUILD" \
              -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"
    fi
    cmake --build "$BUILD" --target "$T_TARGET" -j

    if [[ $IS_QEMU -eq 1 ]]; then
        ELF="$BUILD/$T_TARGET"
        [[ -f "$ELF" ]] || { echo "error: $ELF not built" >&2; exit 1; }
        echo "=== run $TARGET under QEMU ==="
        eval exec "\"$QEMU_RUN\"" --machine "\"$MACHINE\"" "${QEMU_ARGS[$TARGET]}" "\"$ELF\""
    else
        case "$TARGET" in
          reloc)   HEX="$BUILD/scl-fw-reloc.hex" ;;
          classic) HEX="$BUILD/scl-fw-classic-reloc.hex" ;;
          classic-spi) HEX="$BUILD/scl-fw-classic-spi-reloc.hex" ;;
          interrupt) HEX="$BUILD/scl-fw-reloc-interrupt.hex" ;;
        esac

        [[ -f "$HEX" ]] || { echo "error: $HEX not built" >&2; exit 1; }

        echo "=== flash + run $TARGET (semihosting console follows) ==="
        exec "$ROOT/test/tools/flash-run.sh" "$HEX"
    fi
    ;;
  *)
    echo "error: unknown target variant '$TARGET'" >&2
    show_help
    exit 1
    ;;
esac

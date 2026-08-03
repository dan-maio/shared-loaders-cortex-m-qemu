#!/usr/bin/env bash
# build-and-run.sh — configure, build, flash and run ONE SCL test app on the
# selected board, from a single unified codebase.
#
# Usage:
#   ./build-and-run.sh board=<nucleo|weact> test=<name> [abi=sf|hf]
#   ./build-and-run.sh <board> <name>                  # positional form
#
# Options
#   board=   nucleo  -> NUCLEO-F411RE (ST-LINK probe, HSE 16 MHz -> 84 MHz,
#                                       LED = PA5, no SPI)
#            weact   -> WEACT-F411CE  (CMSIS-DAP probe, HSE 25 MHz -> 100 MHz,
#                                       LED = PC13, USART1 + SPI1)
#                     Only board boot/init differs; everything else is shared.
#   test=    which firmware app to build + flash + run (names below).
#   abi=     sf|hf float ABI for the hosted dynamic-newlib apps (default sf).
#
# SPI apps (flatfs-spi, flatfs-bitbang-spi, static-drv, classic-spi,
# xip-static/spi-flash) are WEACT-only — the NUCLEO has no SPI wiring — and the
# script refuses board=nucleo for them.
#
# Test names (the per-tree build-and-run scripts keep their own targets):
#   dyn1  dyn2  flatfs  gotplt-demo            dynamic-nolib  (freestanding)
#   newlib1-sf/hf  newlib2-sf/hf               dynamic-newlib (hosted)
#   ovl1                                       overlay
#   static  static-cpp  static-direct  drv     static-loader apps (drv=weact SPI)
#   reloc  classic  classic-spi  interrupt     ET_REL modules (classic-spi=weact SPI)
#   xip-dynamic-nolib  xip-dynamic-newlib  xip-interrupt
#   xip-static/semihosting  xip-static/spi-flash  xip-static-direct
#   xip-interrupt-static  xip-interrupt-static-direct
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ------------------------------------------------------------------ parse args
BOARD=""
TEST=""
ABI=""
HELP=""
POS=()
for a in "$@"; do
    case "$a" in
      board=*)   BOARD="${a#board=}" ;;
      test=*)    TEST="${a#test=}" ;;
      abi=*)     ABI="${a#abi=}" ;;
      -b)        echo "use board=<nucleo|weact> instead of -b" >&2 ;;
      -h|--help) HELP=1 ;;
      *)         POS+=("$a") ;;
    esac
done
if [[ -z "$BOARD" && ${#POS[@]} -ge 1 ]]; then BOARD="${POS[0]}"; fi
if [[ -z "$TEST" && ${#POS[@]} -ge 2 ]]; then TEST="${POS[1]}"; fi
if [[ -n "$HELP" || -z "$BOARD" || -z "$TEST" ]]; then
    sed -n '2,35p' "$0"
    echo
    echo "Example:"
    echo "  ./build-and-run.sh board=weact  test=dyn2"
    echo "  ./build-and-run.sh board=nucleo test=newlib2-sf abi=sf"
    exit 1
fi

case "$BOARD" in nucleo|weact) ;; *) echo "board must be nucleo or weact (got '$BOARD')" >&2; exit 1 ;; esac
export SCL_BOARD="$BOARD"

# SPI apps are WEACT-only (the NUCLEO-F411RE has no SPI wiring).
SPI_TESTS="flatfs-spi flatfs-bitbang-spi drv classic-spi xip-static/spi-flash"
if [[ "$BOARD" == "nucleo" ]]; then
    for t in $SPI_TESTS; do
        [[ "$TEST" == "$t" ]] && { echo "error: '$TEST' needs SPI hardware — only the WEACT board has it (board=nucleo)" >&2; exit 1; }
    done
fi

# ------------------------------------------------------------- dispatch table
# test-name -> "tree-script : argument"
declare -A ROUTE=(
  # dynamic-nolib (freestanding, hard-float)
  [dyn1]="test-dynamic/build-and-run.sh:dynamic-nolib/test1"
  [dyn2]="test-dynamic/build-and-run.sh:dynamic-nolib/test2"
  [flatfs]="test-dynamic/build-and-run.sh:dynamic-nolib/test3"
  [gotplt-demo]="test-dynamic/build-and-run.sh:dynamic-nolib/test4"
  # dynamic-newlib (hosted, sf|hf)
  [newlib1-sf]="test-dynamic/build-and-run.sh:dynamic-newlib/test1"
  [newlib1-hf]="test-dynamic/build-and-run.sh:dynamic-newlib/test1"
  [newlib2-sf]="test-dynamic/build-and-run.sh:dynamic-newlib/test2"
  [newlib2-hf]="test-dynamic/build-and-run.sh:dynamic-newlib/test2"
  # SPI flatfs (weact only)
  [flatfs-spi]="test-dynamic/build-and-run.sh:flatfs-spi"
  [flatfs-bitbang-spi]="test-dynamic/build-and-run.sh:flatfs-bitbang-spi"
  # overlay
  [ovl1]="test-overlay/build-and-run.sh:test1"
  # static-loader apps
  [static]="test-static/build-and-run.sh:static"
  [static-cpp]="test-static/build-and-run.sh:static-cpp"
  [static-direct]="test-static/build-and-run.sh:static-direct"
  [drv]="test-static/build-and-run.sh:drv"
  # relocatable
  [reloc]="test-relocatable/build-and-run.sh:reloc"
  [classic]="test-relocatable/build-and-run.sh:classic"
  [classic-spi]="test-relocatable/build-and-run.sh:classic-spi"
  [interrupt]="test-relocatable/build-and-run.sh:interrupt"
  # XIP
  [xip-dynamic-nolib]="test-xip/build-and-run.sh:dynamic-nolib"
  [xip-dynamic-newlib]="test-xip/build-and-run.sh:dynamic-newlib"
  [xip-interrupt]="test-xip/build-and-run.sh:interrupt"
  [xip-static/semihosting]="test-xip/build-and-run.sh:static/semihosting"
  [xip-static/spi-flash]="test-xip/build-and-run.sh:static/spi-flash"
  [xip-static-direct]="test-xip/build-and-run.sh:static-direct"
  [xip-interrupt-static]="test-xip/build-and-run.sh:interrupt-static"
  [xip-interrupt-static-direct]="test-xip/build-and-run.sh:interrupt-static-direct"
)

[[ -n "${ROUTE[$TEST]:-}" ]] || { echo "error: unknown test '$TEST'" >&2; exit 1; }
SCRIPT="${ROUTE[$TEST]%%:*}"
ARG="${ROUTE[$TEST]#*:}"

# float ABI for the hosted newlib apps
if [[ "$TEST" == newlib*-hf ]]; then export SCL_ABI=hf; fi
if [[ "$TEST" == newlib*-sf ]]; then export SCL_ABI=sf; fi
[[ -z "$ABI" ]] || export SCL_ABI="$ABI"

echo "=== board=$SCL_BOARD test=$TEST ($SCRIPT $ARG) ==="
exec "$HERE/$SCRIPT" "$ARG"

#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
ROOT="$HERE/../../.."
BUILD="$HERE/build"
FIXTURES="$ROOT/fixtures"

echo "=== build XIP-compatible fixtures (split R-X|RW segments) ==="
"$HERE/fixtures-src/build-xip-drv.sh"

echo "=== configure xip-static spi-flash ==="
SCL_BOARD="${SCL_BOARD:-weact}"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"

echo "=== build firmware ==="
cmake --build "$BUILD" -j

HEX="$BUILD/scl-fw-xip-static-spi-flash.hex"
[[ -f "$HEX" ]] || { echo "error: $HEX not found" >&2; exit 1; }

echo "=== flash + run ($SCL_BOARD) ==="
case "$SCL_BOARD" in
  nucleo) CFG="$ROOT/test/platforms/board/openocd-nucleo.cfg" ;;
  weact)  CFG="$ROOT/test/platforms/board/openocd-weact.cfg"  ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
RUN_MS="${RUN_MS:-1200000}"  # 20 minutes (provisioning + SPI + tests)
killall -s 9 openocd 2>/dev/null || true
"$OPENOCD" -s "$OPENOCD_SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "flash erase_address 0x08040000 0x40000" \
    -c "program $HEX verify" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline -c $FIXTURES/libspi.so $FIXTURES/libuart.so $FIXTURES/libled.so $FIXTURES/libcpp_features.so" \
    -c "reset run"

echo "=== console captured at $BUILD/run.log ==="

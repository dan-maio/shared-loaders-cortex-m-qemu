#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
ROOT="$HERE/../.."
BUILD="$HERE/build"

echo "=== configure ==="
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo|weact) ;;
  qemu)  echo "error: drv is a WEACT-only SPI test — not available under QEMU" >&2; exit 1 ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/arm-none-eabi.cmake" -DSCL_BOARD="$SCL_BOARD"

echo "=== build firmware + fixtures ==="
cmake --build "$BUILD" -j

HEX="$BUILD/scl-fw-drv.hex"
[[ -f "$HEX" ]] || { echo "error: $HEX not found" >&2; exit 1; }

echo "=== flash + run ($SCL_BOARD) ==="
case "$SCL_BOARD" in
  nucleo) CFG="$ROOT/test/platforms/board/openocd-nucleo.cfg" ;;
  weact)  CFG="$ROOT/test/platforms/board/openocd-weact.cfg"  ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
cd "$HERE"
echo "CWD: $(pwd)"
ls -la fixtures/libspi.so
exec "$OPENOCD" -s "$OPENOCD_SCRIPTS" \
    -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $HEX" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline -c $HERE/fixtures/libspi.so $HERE/fixtures/libuart.so $HERE/fixtures/libled.so $HERE/fixtures/libcpp_features.so" \
    -c "reset run"

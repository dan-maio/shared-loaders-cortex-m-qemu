#!/bin/bash
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo) CFG="$HERE/../../test/platforms/board/openocd-nucleo.cfg" ;;
  weact)  CFG="$HERE/../../test/platforms/board/openocd-weact.cfg"  ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac

export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

BUILD_DIR="build"
ELF="$BUILD_DIR/scl-fw-static-direct.elf"

echo "=== Configuring (CMake) ==="
cmake -S . -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DSCL_BOARD="$SCL_BOARD"

echo "=== Building (CMake) ==="
cmake --build "$BUILD_DIR"

echo "=== Flash + run (no GDB, semihosting to terminal) ==="
exec "$OPENOCD" -s "$OPENOCD_SCRIPTS" \
    -f "$CFG" \
    -c "program $ELF" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline scl-fw-static-direct libsh1/libsh1.so libsh2/libsh2.so new-lib/libcpp_features.so" \
    -c "reset run"

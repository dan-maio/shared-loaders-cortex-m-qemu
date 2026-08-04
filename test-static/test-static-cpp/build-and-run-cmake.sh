#!/bin/bash
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo|weact|qemu) ;;
  *) echo "SCL_BOARD must be nucleo, weact or qemu (got '$SCL_BOARD')" >&2; exit 1 ;;
esac

export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

BUILD_DIR="build"
ELF="$BUILD_DIR/f411ce-sh.elf"

echo "=== Configuring (CMake) ==="
if [[ "$SCL_BOARD" == "qemu" ]]; then
    cmake -S . -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
          -DSCL_PLATFORM=qemu-f411re
else
    cmake -S . -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DSCL_BOARD="$SCL_BOARD"
fi

echo "=== Building (CMake) ==="
cmake --build "$BUILD_DIR"

if [[ "$SCL_BOARD" == "qemu" ]]; then
    echo "=== Run under QEMU ==="
    exec "$HERE/../../test/tools/qemu-run.sh" --machine netduinoplus2 \
        --arg f411ce-sh --arg new-lib/libcpp_features.so \
        --done "Second 1" "$ELF"
fi

echo "=== Flash + run (no GDB, semihosting to terminal) ==="
CFG="$HERE/../../test/platforms/board/openocd-${SCL_BOARD}.cfg"
exec "$OPENOCD" -s "$OPENOCD_SCRIPTS" \
    -f "$CFG" \
    -c "program $ELF" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline f411ce-sh new-lib/libcpp_features.so" \
    -c "reset run"

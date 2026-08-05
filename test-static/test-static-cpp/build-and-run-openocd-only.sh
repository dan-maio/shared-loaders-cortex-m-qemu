#!/bin/bash

# Exit on error
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo) CFG="$HERE/../../test/platforms/board/openocd-nucleo.cfg" ;;
  weact)  CFG="$HERE/../../test/platforms/board/openocd-weact.cfg"  ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac


echo "=== Building project ==="
make clean
make

echo "=== Checking for running OpenOCD ==="
# Kill any existing openocd instance to prevent port conflicts
killall -s 9 openocd 2>/dev/null || true

echo "=== Running with OpenOCD Only ==="
$OPENOCD -f "$CFG" \
    -c "adapter speed 1000" \
    -c "init" \
    -c "reset init" \
    -c "flash write_image erase f411ce-sh.elf" \
    -c "reset init" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline \"myprogram new-lib/libcpp_features.so\"" \
    -c "resume"

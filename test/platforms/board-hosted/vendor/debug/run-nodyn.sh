#!/bin/bash
# Flash + run + capture semihosting output (NO_DYN variant with lib paths) — OpenOCD only (no GDB)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
export PATH="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin:/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
FIRMWARE="${1:-$ROOT/build_no_dyn/f411ce-no_dyn.elf}"
LOG="${2:-nodyn_output.log}"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"

SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"
killall -s 9 -q openocd 2>/dev/null || true; sleep 1

echo "Flashing and running $FIRMWARE..."
openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $FIRMWARE" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline f411ce-no_dyn /tmp/mylib1.so /tmp/mylib2.so /tmp/g.so" \
    -c "reset run"

echo "Output saved to $LOG"

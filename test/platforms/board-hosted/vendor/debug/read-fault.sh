#!/bin/bash
# Flash + run firmware, capture semihosting output for fault analysis — OpenOCD only (no GDB)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
FIRMWARE="${1:-$PROJECTS/dyn/build/f411ce-dynloader.elf}"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
LOG="${2:-fault_output.log}"

SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"
killall -s 9 -q openocd 2>/dev/null || true; sleep 1

echo "Flashing and running $FIRMWARE (capturing output for fault analysis)..."
openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $FIRMWARE" \
    -c "arm semihosting enable" \
    -c "reset run"

echo "Output saved to $LOG"

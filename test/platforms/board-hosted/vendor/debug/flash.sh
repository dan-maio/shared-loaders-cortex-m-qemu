#!/bin/bash
# Flash firmware only (no execute) — OpenOCD only
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
FIRMWARE="${1:-$PROJECTS/dyn/build/f411ce-dynloader.elf}"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"

openocd -s "$SCRIPTS" -f "$CFG" \
    -c "program $FIRMWARE verify reset exit"

#!/bin/bash
# Flash + run DYN hard-float firmware (HF=1) — OpenOCD only (no GDB)
export PATH="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin:/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
FIRMWARE="${1:-build_dyn/f411ce-dyn_hf.elf}"

killall -s 9 -q openocd 2>/dev/null; sleep 1
exec openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $FIRMWARE" \
    -c "arm semihosting enable" \
    -c "reset run"

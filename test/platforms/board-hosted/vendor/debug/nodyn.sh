#!/bin/bash
# Flash + run NO_DYN firmware (mylib1+mylib2+gotplt) — OpenOCD only (no GDB)
export PATH="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin:/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"

killall -s 9 -q openocd 2>/dev/null; sleep 1
exec openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program build_no_dyn/f411ce-no_dyn.elf" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline f411ce-no_dyn /tmp/mylib1.so /tmp/mylib2.so /tmp/g.so" \
    -c "reset run"

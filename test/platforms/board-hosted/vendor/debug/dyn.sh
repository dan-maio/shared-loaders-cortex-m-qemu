#!/bin/bash
# Flash + run DYN firmware
export PATH="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin:/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
killall -s 9 -q openocd 2>/dev/null; sleep 1
bash debug/run-dyn.sh 2>&1

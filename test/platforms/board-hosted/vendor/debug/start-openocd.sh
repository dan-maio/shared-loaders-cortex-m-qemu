#!/bin/bash
# Start OpenOCD with WeAct BlackPill CMSIS-DAP config
killall -s 9 -q openocd 2>/dev/null
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
exec /home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin/openocd \
    -s /home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts \
    -f "$CFG"

#!/bin/bash
# Build all test libraries + deploy + run firmware with new libs — OpenOCD only (no GDB)
# Usage: ./run-new-libs-test.sh [dyn|no_dyn] [--flash-only]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
VARIANT="${1:-dyn}"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"

echo "=== 1/5 Build test libraries ==="
make -C "$SCRIPT_DIR/test_libs" clean
make -C "$SCRIPT_DIR/test_libs" all

echo "=== 2/5 Deploy to /tmp/ ==="
cp "$PROJECTS/libs/build/c_test1.so" \
   "$PROJECTS/libs/build/c_test2.so" \
   "$PROJECTS/libs/build/cpp_test1.so" \
   "$PROJECTS/libs/build/cpp_test2.so" /tmp/
echo "Deployed c_test1.so c_test2.so cpp_test1.so cpp_test2.so"

echo "=== 3/5 Verify ELF structure ==="
for lib in c_test1 c_test2 cpp_test1 cpp_test2; do
    echo "--- $lib ---"
    arm-none-eabi-readelf -h "/tmp/$lib.so" 2>/dev/null | grep -E 'Class|Machine|Type'
    arm-none-eabi-readelf -r "/tmp/$lib.so" 2>/dev/null | grep -E 'RELATIVE|GLOB_DAT|JUMP_SLOT|ABS32|COPY' | \
        awk '{print $2}' | sort | uniq -c | sort -rn
done

if [ "$2" = "--flash-only" ]; then
    echo "=== Flash only (no run) ==="
    "$SCRIPT_DIR/flash.sh" "build_$VARIANT/f411ce-$VARIANT.elf"
    exit 0
fi

echo "=== 4/5 Flash + run $VARIANT firmware ==="
FIRMWARE="$PROJECTS/$VARIANT/build/f411ce-$VARIANT.elf"
CMDLINE=""
if [ "$VARIANT" = "no_dyn" ]; then
    CMDLINE="-c arm semihosting_cmdline f411ce-no_dyn /tmp/c_test1.so /tmp/c_test2.so /tmp/cpp_test1.so /tmp/cpp_test2.so"
fi

killall -s 9 -q openocd 2>/dev/null || true; sleep 1
openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $FIRMWARE" \
    -c "arm semihosting enable" \
    $CMDLINE \
    -c "reset run"

echo "=== Done. Output in test_output.log ==="
grep -E 'PASS|FAIL|HardFault|test' test_output.log 2>/dev/null || true

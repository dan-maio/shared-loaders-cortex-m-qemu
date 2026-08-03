#!/bin/bash
# Verify all test libraries' ELF structure and relocations (host-side, no hardware)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBS="/tmp"

# Ensure libs are deployed
if [ ! -f "$LIBS/c_test1.so" ]; then
    echo "Deploying test libraries first..."
    make -C "$SCRIPT_DIR/test_libs" all
    cp "$(dirname "$SCRIPT_DIR")/libs/build/"*.so /tmp/
fi

echo "=========================================="
echo "  Test Library Verification (Host Side)"
echo "=========================================="
echo ""

PASS=0
FAIL=0

for lib in c_test1 c_test2 cpp_test1 cpp_test2; do
    SO="$LIBS/$lib.so"
    echo "--- $lib ---"
    
    # 1. Check ELF type
    TYPE=$(arm-none-eabi-readelf -h "$SO" 2>/dev/null | grep 'Type:' | awk '{print $2}')
    if [ "$TYPE" = "DYN" ]; then
        echo "  [PASS] Type = DYN (shared object)"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] Type = $TYPE (expected DYN)"
        FAIL=$((FAIL+1))
    fi
    
    # 2. Check ARM 32-bit
    CLASS=$(arm-none-eabi-readelf -h "$SO" 2>/dev/null | grep 'Class:' | awk '{print $2}')
    MACHINE=$(arm-none-eabi-readelf -h "$SO" 2>/dev/null | grep 'Machine:' | awk '{print $2}')
    if [ "$CLASS" = "ELF32" ] && [ "$MACHINE" = "ARM" ]; then
        echo "  [PASS] ELF32 ARM"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] Class=$CLASS Machine=$MACHINE"
        FAIL=$((FAIL+1))
    fi
    
    # 3. Count relocation types
    echo "  Relocations:"
    arm-none-eabi-readelf -r "$SO" 2>/dev/null | grep -E 'RELATIVE|GLOB_DAT|JUMP_SLOT|ABS32' | \
        awk '{print $2}' | sort | uniq -c | sort -rn | while read count type; do
        echo "    $count x $type"
    done
    
    # 4. Check for unresolved symbols
    UNDEF=$(arm-none-eabi-readelf -s "$SO" 2>/dev/null | grep 'UND' | grep -v 'fprintf\|printf\|puts\|malloc\|free\|AA\|BB\|s1\|f(\|w(\|__cxa\|_Znwj\|_ZdlPv\|_impure\|__gnu_Unwind\|__register\|__deregister\|getenv\|fputc\|fputs\|fwrite\|strcpy\|strlen\|strcat\|strcmp\|strchr\|sprintf\|abort\|realloc\|strtoul\|memcpy\|memset\|memcmp' | grep -v 'c_test2_func' | wc -l)
    if [ "$UNDEF" -eq 0 ]; then
        echo "  [PASS] No unexpected undefined symbols"
        PASS=$((PASS+1))
    else
        echo "  [WARN] $UNDEF undefined symbols (may be resolved at runtime)"
        arm-none-eabi-readelf -s "$SO" 2>/dev/null | grep 'UND' | grep -v 'fprintf\|printf\|puts\|malloc\|free\|AA\|BB\|s1\|__cxa\|_Znwj\|_ZdlPv\|_impure' | awk '{print $8}' | head -5
    fi
    
    # 5. Get symbol exports
    EXPORTS=$(arm-none-eabi-readelf -s "$SO" 2>/dev/null | grep 'GLOBAL.*DEFAULT' | grep -v 'UND' | wc -l)
    echo "  [INFO] $EXPORTS exported symbols"
    
    echo ""
done

echo "=========================================="
echo "  Results: $PASS passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

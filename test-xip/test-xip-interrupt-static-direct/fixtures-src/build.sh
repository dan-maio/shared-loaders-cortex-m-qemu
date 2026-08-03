#!/usr/bin/env bash
# Build the static-test XIP fixture: slib.so, single-pic-base + split (R-X | RW),
# VA==file_offset, so its R-X segment can execute in place from flash.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../../fixtures"
LD="$HERE/ldscripts/xip-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
OPT="-O0 -ffunction-sections -fdata-sections -fno-inline-functions"
LDFLAGS="-Wl,-E -Wl,-Bsymbolic"

echo "=== slib.so (single-pic-base, split R-X|RW) -> test-static/fixtures ==="
$CC $MCU $PIC $OPT -shared -nostartfiles $LDFLAGS -Wl,--gc-sections \
    -T "$LD" -o "$OUT/slib.so" "$HERE/slib.c"
"$TC/arm-none-eabi-readelf" -l "$OUT/slib.so" | awk '/LOAD/{print "  "$0}'
echo "done -> $OUT/slib.so"

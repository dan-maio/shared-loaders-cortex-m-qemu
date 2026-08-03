#!/usr/bin/env bash
# build-fixtures.sh — build the XIP-eligible split fixtures into test-xip/fixtures/.
#
# XIP requires a library whose GOT is addressed via r10, not PC-relative
# (-msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative), linked
# into two segments (R-X | RW) by xip-split.ld with VA == file_offset. Such a
# library can execute its R-X segment straight from flash while its RW segment
# lives in RAM. (-fPIC C/C++ libraries use PC-relative GOT and cannot XIP; they
# load copy-whole from test/fixtures via the stock loader.)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."                       # repo root
OUT="$HERE/../fixtures"                   # test-xip/fixtures
LD="$HERE/ldscripts/xip-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
OPT="-O0 -ffunction-sections -fdata-sections -fno-inline-functions"
# -Bsymbolic binds intra-library calls locally (direct BL, no PLT), so only genuine
# external (main) calls remain to route through the GOT. Linker PLT stubs use
# PC-relative GOT addressing, which is wrong once text is in flash and GOT in RAM.
LDFLAGS="-Wl,-E -Wl,-Bsymbolic"

build_one() {  # $1=name (mylibN), $2=src-include-dir
    echo "=== $1.so (single-pic-base, split R-X|RW) -> test-xip/fixtures ==="
    $CC $MCU $PIC $OPT -shared -nostartfiles $LDFLAGS -T "$LD" \
        -I "$2" -I "$ROOT/test/fixtures-src/include" \
        -o "$OUT/$1.so" "$HERE/$1.c"
    "$TC/arm-none-eabi-readelf" -l "$OUT/$1.so" | awk '/LOAD/{print "  "$0}'
}

build_one mylib1 "$ROOT/test/fixtures-src/mylib1"
build_one mylib2 "$ROOT/test/fixtures-src/mylib2"
echo "done -> $OUT"

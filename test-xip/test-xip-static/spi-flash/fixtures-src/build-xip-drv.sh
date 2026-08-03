#!/usr/bin/env bash
# Build test-drv fixtures (spi.so, uart.so, led.so, cpp_features.so) with XIP-compatible flags.
#
# These modules are built with:
#   -fpic -msingle-pic-base -mpic-register=r10  (single PIC base register)
#   -T xip-split.ld                             (split R-X | RW segments)
#
# This allows them to be loaded with XIP (text in flash, data in RAM).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$HERE/../../../.."
DRV_DIR="$REPO_ROOT/test-static/test-drv"
OUT="$REPO_ROOT/fixtures"
LD="$HERE/ldscripts/xip-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"
CXX="$TC/arm-none-eabi-g++"

# XIP-compatible flags
MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=soft"
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
OPT="-O0 -ffunction-sections -fdata-sections -fno-inline-functions"
LDFLAGS="-Wl,-E -Wl,-Bsymbolic -shared -nostartfiles"

echo "=== Building XIP-compatible fixtures from test-drv ==="

# Build spi.so
echo "Building spi.so (XIP)..."
$CC $MCU $PIC $OPT -I"$DRV_DIR/fixtures/include" \
    -c "$DRV_DIR/fixtures/src/spi.c" -o "$DRV_DIR/fixtures/src/spi.o"
$CC $MCU $PIC $OPT $LDFLAGS \
    "$DRV_DIR/fixtures/src/spi.o" -T "$LD" -o "$OUT/libspi.so"
"$TC/arm-none-eabi-readelf" -l "$OUT/libspi.so" | awk '/LOAD/{print "  "$0}'

# Build uart.so
echo "Building uart.so (XIP)..."
$CC $MCU $PIC $OPT -I"$DRV_DIR/fixtures/include" \
    -c "$DRV_DIR/fixtures/src/uart.c" -o "$DRV_DIR/fixtures/src/uart.o"
$CC $MCU $PIC $OPT $LDFLAGS \
    "$DRV_DIR/fixtures/src/uart.o" -T "$LD" -o "$OUT/libuart.so"
"$TC/arm-none-eabi-readelf" -l "$OUT/libuart.so" | awk '/LOAD/{print "  "$0}'

# Build led.so
echo "Building led.so (XIP)..."
$CC $MCU $PIC $OPT -I"$DRV_DIR/fixtures/include" \
    -c "$DRV_DIR/fixtures/src/led.c" -o "$DRV_DIR/fixtures/src/led.o"
$CC $MCU $PIC $OPT $LDFLAGS \
    "$DRV_DIR/fixtures/src/led.o" -T "$LD" -o "$OUT/libled.so"
"$TC/arm-none-eabi-readelf" -l "$OUT/libled.so" | awk '/LOAD/{print "  "$0}'

# Build cpp_features.so
echo "Building cpp_features.so (XIP)..."
CXXFLAGS="$MCU $PIC $OPT -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib"
$CXX $CXXFLAGS -I"$DRV_DIR/fixtures/include" \
    -c "$DRV_DIR/fixtures/src/cpp_features.cpp" -o "$DRV_DIR/fixtures/src/cpp_features.o"
$CXX $CXXFLAGS $LDFLAGS \
    "$DRV_DIR/fixtures/src/cpp_features.o" -T "$LD" -o "$OUT/libcpp_features.so"
"$TC/arm-none-eabi-readelf" -l "$OUT/libcpp_features.so" | awk '/LOAD/{print "  "$0}'

echo ""
echo "=== XIP-compatible fixtures built ==="
echo "  $OUT/libspi.so"
echo "  $OUT/libuart.so"
echo "  $OUT/libled.so"
echo "  $OUT/libcpp_features.so"
echo ""
echo "=== Verification ==="
ls -la "$OUT"/lib{spi,uart,led,cpp_features}.so

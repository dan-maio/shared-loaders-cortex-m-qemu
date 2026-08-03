#!/usr/bin/env bash
# build-xip-libs.sh — Rebuild all standard PIC libraries with XIP split segments.
# Output goes to test-xip/fixtures/ (overwrites cmake copies from test/fixtures/).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."                          # repo root
OUT="$HERE/../fixtures"                      # test-xip/fixtures
LD="$HERE/ldscripts/xip-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"
CXX="$TC/arm-none-eabi-g++"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
# r10-based PIC — required when text executes from flash but GOT lives in RAM.
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
OPT="-O0 -ffunction-sections -fdata-sections"
CXXSTD="-std=c++20"
# -Bsymbolic binds intra-lib calls directly (no PLT), so only genuine external
# calls route through the GOT.  Without it, local function calls would go
# through the PLT which reads from the flash-resident GOT — correct for XIP.
LDFLAGS="-Wl,-E -Wl,-Bsymbolic"
INC="-I $ROOT/test/fixtures-src/include"

echo "=== Building XIP-eligible fixtures -> $OUT ==="

# ---- C libraries ----
build_c() {  # $1=name, $2=src, $3=inc
    echo "  $1.so (XIP split, r10-based PIC)"
    $CC $MCU $PIC $OPT -shared -nostartfiles $LDFLAGS -Wl,--gc-sections \
        -T "$LD" -I "$3" $INC -o "$OUT/$1.so" "$2"
    $TC/arm-none-eabi-readelf -l "$OUT/$1.so" | awk '/LOAD/{print "    "$0}'
}

build_c "c1"     "$ROOT/test/fixtures-src/c1/c1.c"              "$ROOT/test/fixtures-src/c1"
build_c "c2"     "$ROOT/test/fixtures-src/c2/c2.c"              "$ROOT/test/fixtures-src/c2"
build_c "libprov" "$ROOT/test/fixtures-src/libprov/libprov.c"   "$ROOT/test/fixtures-src/libprov"
build_c "libcons" "$ROOT/test/fixtures-src/libcons/libcons.c"   "$ROOT/test/fixtures-src/libcons"

# ---- C++ libraries (no EH) ----
build_cxx_noeh() {  # $1=name, $2=src, $3=inc
    echo "  $1.so (XIP split, r10-based PIC, no-exceptions)"
    $CXX $MCU $PIC $OPT $CXXSTD -shared -nostartfiles -nostdlib -nodefaultlibs \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        $LDFLAGS -Wl,--gc-sections -T "$LD" -I "$3" $INC -o "$OUT/$1.so" "$2"
    $TC/arm-none-eabi-readelf -l "$OUT/$1.so" | awk '/LOAD/{print "    "$0}'
}

build_cxx_noeh "cpp3"    "$ROOT/test/fixtures-src/cpp3/cpp3.cpp"                 "$ROOT/test/fixtures-src/cpp3"
build_cxx_noeh "libcppsh" "$ROOT/test/fixtures-src/libcppsh/src/cppsh-api.cpp"   "$ROOT/test/fixtures-src/libcppsh/include"

# libcppsh has two source files — handle separately
echo "  libcppsh.so widget (XIP split, r10-based PIC, no-exceptions)"
$CXX $MCU $PIC $OPT $CXXSTD -shared -nostartfiles -nostdlib -nodefaultlibs \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    $LDFLAGS -Wl,--gc-sections -T "$LD" -I "$ROOT/test/fixtures-src/libcppsh/include" $INC \
    -o "$OUT/libcppsh.so" \
    "$ROOT/test/fixtures-src/libcppsh/src/cppsh-api.cpp" \
    "$ROOT/test/fixtures-src/libcppsh/src/widget.cpp"
$TC/arm-none-eabi-readelf -l "$OUT/libcppsh.so" | awk '/LOAD/{print "    "$0}'

# ---- C++ libraries (with EH/RTTI) ----
build_cxx_eh() {  # $1=name, $2=src, $3=inc
    echo "  $1.so (XIP split, r10-based PIC, exceptions ON)"
    $CXX $MCU $PIC $OPT $CXXSTD -shared -nostartfiles -nostdlib -nodefaultlibs \
        $LDFLAGS -Wl,--gc-sections -T "$LD" -I "$3" $INC -o "$OUT/$1.so" "$2"
    $TC/arm-none-eabi-readelf -l "$OUT/$1.so" | awk '/LOAD/{print "    "$0}'
}

build_cxx_eh "libcppeh" "$ROOT/test/fixtures-src/libcppeh/src/cppeh.cpp" \
    "$ROOT/test/fixtures-src/libcppeh/include"
build_cxx_eh "ehx_catch" "$ROOT/test/fixtures-src/ehx/ehx-catch.cpp" \
    "$ROOT/test/fixtures-src/libcppeh/include"

echo "=== done -> $OUT ==="

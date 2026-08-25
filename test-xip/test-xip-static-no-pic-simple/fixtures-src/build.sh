#!/usr/bin/env bash
# Build the no-pic fixtures for the SIMPLE test — the same recipe as the parent
# (test-xip-static-no-pic) but for the shared library set the user listed:
#   c1 c2 cpp3 ehx_catch libcons libcppsh libprov slib
# (plus libcppeh, which ehx_catch depends on for eh_throw).
#
# All modules: -fno-pic -mlong-calls, linked at FIXED addresses (sub-slots of
# the app's _sxip_text_* / _sxip_data_* windows) so main calls them with plain
# function pointers — no r10, no GOT, no PLT.
#
# Sub-slot addresses are NOT hand-picked: the script builds each module once at
# a placeholder base, measures its two PT_LOAD sizes (text_memsz / data_memsz)
# with readelf, then PACKS every module contiguously inside the fixed windows
# and re-links with the real addresses. Overlap is impossible by construction;
# an overflow of the windows fails the build.
#
# Sources come from the shared test/fixtures-src tree (c1/c2/cpp3/libprov/
# libcons/libcppsh/libcppeh/ehx) and from the r10 XIP test's slib.c.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../fixtures"
ROOT="$HERE/../../.."                      # repo root
FSRC="$ROOT/test/fixtures-src"             # shared fixture sources
LD_TMPL="$HERE/ldscripts/nopic-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"
CXX="$TC/arm-none-eabi-g++"
READELF="$TC/arm-none-eabi-readelf"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
NOPIC="-fno-pic -mlong-calls"
OPT="-O0 -ffunction-sections -fdata-sections"
# max-page-size=0x100: the loader copies by p_offset/p_vaddr and never needs
# 4K-aligned segments; small pages shrink the .so files so all 9 fit the 64K
# (QEMU) flat_fs archive.
LDFLAGS="-Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all"
FMINC="-I $FSRC/include"                   # fm-iface.h (trace/fm_var/...)

# Fixed windows: read from the APP linker script (the single source of
# truth) so this script always matches what the firmware reserves.
APP_LD="$HERE/../src/sections-flatfs-hosted-nopic.ld"
READ_SYM() { grep -oE "$1[[:space:]]*=[[:space:]]*0x[0-9A-Fa-f]+" "$APP_LD" | grep -oE "0x[0-9A-Fa-f]+" | head -1; }
TEXT_BASE=$(READ_SYM _sxip_text_start)
TEXT_END=$(READ_SYM _sxip_text_end)
DATA_BASE=$(READ_SYM _sxip_data_start)
DATA_END=$(READ_SYM _sxip_data_end)
if [ -z "$TEXT_BASE" ] || [ -z "$TEXT_END" ] || [ -z "$DATA_BASE" ] || [ -z "$DATA_END" ]; then
    echo "  ERROR: could not read _sxip_* window symbols from $APP_LD" >&2; exit 1
fi
SLOT_ALIGN=0x100

# $1=name $2=text_base $3=data_base $4=compiler; rest = extra flags + sources
link_mod() {
    local name="$1" tb="$2" db="$3" cc="$4"; shift 4
    local ld="$OUT/$name.ld"
    sed -e "s/__TEXT_BASE__/$tb/g" -e "s/__DATA_BASE__/$db/g" "$LD_TMPL" > "$ld"
    echo "  $name.so (text $tb data $db)"
    "$cc" $MCU $NOPIC $OPT -shared -nostartfiles $LDFLAGS -Wl,--gc-sections \
        -T "$ld" -o "$OUT/$name.so" "$@"
}
build_c()   { link_mod "$1" "$2" "$3" "$CC"  "${@:4}"; }
build_noeh(){ link_mod "$1" "$2" "$3" "$CXX" -std=c++20 -nostdlib -nodefaultlibs \
                          -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit "${@:4}"; }
build_eh()  { link_mod "$1" "$2" "$3" "$CXX" -std=c++20 -nostdlib -nodefaultlibs \
                          -fno-threadsafe-statics -fno-use-cxa-atexit "${@:4}"; }

# Build every fixture at its current TADDR[$n] / DADDR[$n] slot.
build_fixtures() {
    build_c    slib     "${TADDR[slib]}"     "${DADDR[slib]}"     "$ROOT/test-xip/test-xip-static/semihosting/fixtures-src/slib.c"
    build_c    libprov  "${TADDR[libprov]}"  "${DADDR[libprov]}"  -I "$FSRC/libprov" $FMINC "$FSRC/libprov/libprov.c"
    build_c    libcons  "${TADDR[libcons]}"  "${DADDR[libcons]}"  -I "$FSRC/libcons" $FMINC "$FSRC/libcons/libcons.c"
    build_noeh libcppsh "${TADDR[libcppsh]}" "${DADDR[libcppsh]}" -I "$FSRC/libcppsh/include" \
                          "$FSRC/libcppsh/src/cppsh-api.cpp" "$FSRC/libcppsh/src/widget.cpp"
    build_eh   libcppeh "${TADDR[libcppeh]}" "${DADDR[libcppeh]}" -I "$FSRC/libcppeh/include" "$FSRC/libcppeh/src/cppeh.cpp"
    build_eh   ehx_catch "${TADDR[ehx_catch]}" "${DADDR[ehx_catch]}" -I "$FSRC/libcppeh/include" "$FSRC/ehx/ehx-catch.cpp"
    build_noeh cpp3     "${TADDR[cpp3]}"     "${DADDR[cpp3]}"     -I "$FSRC/cpp3" $FMINC "$FSRC/cpp3/cpp3.cpp"
    build_c    c2       "${TADDR[c2]}"       "${DADDR[c2]}"       -I "$FSRC/c2" $FMINC "$FSRC/c2/c2.c"
    build_c    c1       "${TADDR[c1]}"       "${DADDR[c1]}"       -I "$FSRC/c1" $FMINC "$FSRC/c1/c1.c"
}

# Measure a module's two PT_LOAD sizes (sets TXT_SZ / DAT_SZ, hex).
measure() {
    local f="$OUT/$1.so"
    TXT_SZ=$("$READELF" -l "$f" | awk '$1=="LOAD" && $7=="R" {print $6; exit}')
    DAT_SZ=$("$READELF" -l "$f" | awk '$1=="LOAD" && $7 ~ /W/ {print $6; exit}')
    if [ -z "${TXT_SZ:-}" ] || [ -z "${DAT_SZ:-}" ]; then
        echo "  ERROR: could not read segment sizes from $f" >&2; exit 1
    fi
}

MODULES="slib libprov libcons libcppsh libcppeh ehx_catch cpp3 c2 c1"
declare -A TADDR DADDR

echo "=== pass 1: build at a placeholder base + measure sizes"
for n in $MODULES; do TADDR[$n]=$TEXT_BASE; DADDR[$n]=$DATA_BASE; done
build_fixtures

echo "=== pass 2: pack addresses contiguously inside the fixed windows"
cur_t=$TEXT_BASE; cur_d=$DATA_BASE
for n in $MODULES; do
    measure "$n"
    TADDR[$n]=$(printf '0x%x' "$cur_t")
    DADDR[$n]=$(printf '0x%x' "$cur_d")
    printf '  %-10s text %-10s (%s)   data %-10s (%s)\n' \
        "$n" "${TADDR[$n]}" "$TXT_SZ" "${DADDR[$n]}" "$DAT_SZ"
    cur_t=$(( cur_t + ((TXT_SZ + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1)) ))
    cur_d=$(( cur_d + ((DAT_SZ + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1)) ))
done
if (( cur_t > TEXT_END || cur_d > DATA_END )); then
    echo "  ERROR: packed fixtures overflow the windows (text end $(printf '0x%x' $cur_t)," \
         " data end $(printf '0x%x' $cur_d)); enlarge _sxip_* in the app linker script" >&2
    exit 1
fi

echo "=== pass 3: re-link with the real (packed) addresses"
build_fixtures

echo "--- verify: two PT_LOAD, fixed VAs, no PIC, no overlap ---"
for f in $MODULES; do
    echo "  $f.so"
    "$READELF" -l "$OUT/$f.so" | awk '/LOAD/{print "    "$0}'
done
echo "done -> $OUT"

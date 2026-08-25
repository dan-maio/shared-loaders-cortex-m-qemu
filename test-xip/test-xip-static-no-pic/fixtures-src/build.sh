#!/usr/bin/env bash
# Build the no-pic XIP/static fixtures: -fno-pic -mlong-calls, linked at fixed
# addresses. Output goes to test-xip-static-no-pic/fixtures/, which the app
# provisions over semihosting.
#
# Why these flags:
#   -fno-pic          no GOT, no PLT, no PIC base register (r10). Every address
#                     is absolute, baked by the linker against the fixed VAs.
#   -mlong-calls      calls reach across the flash<->RAM gap (>16MB) and between
#                     module sub-slots via literal-pool + blx (veneur-safe).
#   -Wl,-E            export all globals to .dynsym so the loader's resolve()
#                     and cross-module references can find them.
#   --unresolved-symbols=ignore-all
#                     imports from main / other modules stay UNDEFINED; the
#                     loader patches their R_ARM_ABS32 slots at load.
#
# Sub-slot addresses are NOT hand-picked: the script builds each module once at
# a placeholder base, measures its two PT_LOAD sizes, then PACKS every module
# contiguously inside the fixed windows and re-links with the real addresses.
# No two modules can overlap; an overflow of the windows fails the build.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../fixtures"
LD_TMPL="$HERE/ldscripts/nopic-split.ld"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"
READELF="$TC/arm-none-eabi-readelf"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
NOPIC="-fno-pic -mlong-calls"
OPT="-O0 -ffunction-sections -fdata-sections"
# max-page-size=0x100 shrinks the .so files (the loader copies by offset/vaddr
# and never needs 4K-aligned segments).
LDFLAGS="-Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all"

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

# $1=name $2=text_base $3=data_base; rest = extra ld args
build() {
    local name="$1" tb="$2" db="$3"; shift 3
    local ld="$OUT/$name.ld"
    sed -e "s/__TEXT_BASE__/$tb/g" -e "s/__DATA_BASE__/$db/g" "$LD_TMPL" > "$ld"
    echo "  $name.so (text $tb data $db)"
    "$CC" $MCU $NOPIC $OPT -shared -nostartfiles $LDFLAGS -Wl,--gc-sections \
        -T "$ld" -o "$OUT/$name.so" "$HERE/$name.c" "$@"
}

# Build every fixture at its current TADDR[$n] / DADDR[$n] slot. nprov is built
# before ncons because ncons links against -l:nprov.so.
build_fixtures() {
    build "nslib"   "${TADDR[nslib]}"   "${DADDR[nslib]}"
    build "nmylib1" "${TADDR[nmylib1]}" "${DADDR[nmylib1]}"
    build "nprov"   "${TADDR[nprov]}"   "${DADDR[nprov]}"
    # ncons imports p_fn from nprov: link against it so p_fn is a known symbol.
    build "ncons"   "${TADDR[ncons]}"   "${DADDR[ncons]}" -L "$OUT" -l:nprov.so
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

MODULES="nslib nmylib1 nprov ncons"
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
    printf '  %-8s text %-10s (%s)   data %-10s (%s)\n' \
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

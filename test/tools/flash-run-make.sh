#!/usr/bin/env bash
# flash-run-make.sh — flash a .hex/.elf built with Makefiles and run via semihosting.
# Resolves hex paths relative to the SCL root (parent of test/).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

SCLROOT="$HERE/../.."
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo|weact) ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
CFG="$SCLROOT/test/platforms/board/openocd-${SCL_BOARD}.cfg"

find_hex() {
    find "$SCLROOT" -type f -name "$1" -path '*/build/*' \
        -printf '%T@\t%p\n' 2>/dev/null | sort -rn | head -1 | cut -f2-
}

show_help() {
    cat <<EOF
Usage: $(basename "$0") <app-name|path>

App names (searched under */build/):
  scl-fw-dyn1  scl-fw-dyn2  scl-fw-flatfs  scl-fw-gotplt-demo
  scl-fw-ovl1  scl-fw-dynamic-newlib1  scl-fw-dynamic-newlib2
  scl-fw-reloc  scl-fw-classic-reloc  scl-fw-reloc-interrupt
  scl-fw-xip-nolib1  scl-fw-xip-newlib1  scl-fw-xip-static
  scl-fw-xip-interrupt1  scl-fw-xip-interrupt-static
EOF
}

[[ $# -eq 0 ]] && { show_help; exit 1; }
[[ "$1" == "-h" || "$1" == "--help" ]] && { show_help; exit 0; }

if [[ "$1" == */* || "$1" == *.hex || "$1" == *.elf ]]; then
    HEX="$1"
else
    HEX="$(find_hex "$1.hex")"
    [[ -z "$HEX" ]] && HEX="$(find_hex "$1.elf")"
fi

[[ -n "$HEX" && -f "$HEX" ]] || { echo "error: $1 not found (build it with make first)" >&2; exit 1; }
echo "flashing: $HEX"

cd "$SCLROOT"
exec "$OPENOCD" -s "$OPENOCD_SCRIPTS" -f "$CFG" \
    -c "init" -c "reset halt" -c "program $HEX" \
    -c "arm semihosting enable" -c "reset run"

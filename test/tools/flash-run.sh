#!/usr/bin/env bash
# flash-run.sh — flash a test firmware .hex and run it with semihosting console.
#
# Usage:  flash-run.sh [--timeout <ms>] [--log <file>] <path/to/app.hex>
#
# Options:
#   --timeout <ms>   auto-shutdown OpenOCD after <ms> milliseconds (default:
#                    interactive, Ctrl-C to stop). Also settable via RUN_MS env.
#   --log <file>     tee the semihosting console to <file> in addition to stdout.
#   -h|--help        this text
#
# Without --timeout the script blocks on OpenOCD's stdout until Ctrl-C.
# With --timeout, OpenOCD runs the firmware for the specified duration then
# shuts down cleanly (the XIP build-and-run.sh uses this mode with RUN_MS).
#
# Semihosting MUST be enabled before "reset run" (a BKPT 0xAB with semihosting
# off faults into a double fault / lockup). No "verify": its target-side CRC
# algorithm double-faults on this core.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

# --- parse options ---
TIMEOUT="${RUN_MS:-}"    # default: empty = interactive (no auto-shutdown)
LOGFILE=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --log)     LOGFILE="$2"; shift 2 ;;
        -h|--help) show_help; exit 0 ;;
        *)         POSITIONAL+=("$1"); shift ;;
    esac
done

# Target board selects the OpenOCD flash config: NUCLEO-F411RE uses its on-board
# ST-LINK (openocd-nucleo.cfg), WEACT-F411CE uses its CMSIS-DAP (openocd-weact.cfg).
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in
  nucleo|weact) ;;
  *) echo "SCL_BOARD must be nucleo or weact (got '$SCL_BOARD')" >&2; exit 1 ;;
esac
CFG="$HERE/../platforms/board/openocd-${SCL_BOARD}.cfg"

# Accept either a .hex path or a test-app NAME. A NAME is located by searching
# the build trees under the SCL root, so it works whether you configured at the
# repo root (build-fw / build-dynamic-newlib[-hf]) or under any test-* tree. Newest match wins.
#
# The newlib static apps come in two float-ABI variants; SCL_DYNAMIC_NEWLIB_VARIANT (sf|hf)
# selects which, defaulting to sf. Use the flash-run-sf.sh / flash-run-hf.sh
# wrappers, or set the variable directly:
#   SCL_DYNAMIC_NEWLIB_VARIANT=hf  flash-run.sh scl-fw-dynamic-newlib2
#   flash-run.sh scl-fw-dyn1|dyn2|flatfs|gotplt-demo | scl-fw-ovl1 | scl-fw-dynamic-newlib1|2
#   flash-run.sh path/to/app.hex
# Set SCL_BOARD=nucleo|weact (default weact) to pick the OpenOCD flash config
# (ST-LINK vs CMSIS-DAP) for the board the firmware was built for.
TESTDIR="$HERE/.."
SCLROOT="$HERE/../.."   # the SCL root (parent of test/)
VARIANT="${SCL_DYNAMIC_NEWLIB_VARIANT:-sf}"          # sf|hf — only affects the newlib static apps
case "$VARIANT" in sf) STREE=build-dynamic-newlib; SLIB=build ;;
                   hf) STREE=build-dynamic-newlib-hf; SLIB=build_hf ;;
                   *) echo "SCL_DYNAMIC_NEWLIB_VARIANT must be sf or hf (got '$VARIANT')" >&2; exit 1 ;; esac

# newest <name>.hex found under the SCL root, optionally constrained to a
# specific build-tree component (so build-dynamic-newlib vs build-dynamic-newlib-hf don't mix).
find_hex() {   # $1=hexname  [$2=required build-tree component]
    local pat='*build*'; [[ -n "${2:-}" ]] && pat="*/$2/*"
    find "$SCLROOT" -type f -name "$1" -path "$pat" -printf '%T@\t%p\n' 2>/dev/null \
        | sort -rn | head -1 | cut -f2-
}

VALID="scl-fw-dyn1 scl-fw-dyn2 scl-fw-flatfs scl-fw-ovl1 scl-fw-gotplt-demo scl-fw-dynamic-newlib1 scl-fw-dynamic-newlib2 scl-fw-flatfs-spi scl-fw-flatfs-bitbang-spi"

show_help() {
    local name="$(basename "$0")"
    if [[ -n "${SCL_DYNAMIC_NEWLIB_VARIANT:-}" ]]; then
        name="flash-run-${SCL_DYNAMIC_NEWLIB_VARIANT}.sh"
    fi
    cat <<EOF
Usage: $name [--timeout <ms>] [--log <file>] <app-name|path/to/app.hex>

Flashes a test firmware .hex and runs it with semihosting console.

Options:
  --timeout <ms>  auto-shutdown after <ms> ms (default: interactive, Ctrl-C to stop)
  --log <file>    tee console output to file

Without --timeout the firmware runs until Ctrl-C.
With --timeout, OpenOCD shuts down automatically after the specified duration.

Valid app names:
  scl-fw-dyn1
  scl-fw-dyn2
  scl-fw-flatfs
  scl-fw-ovl1
  scl-fw-gotplt-demo
  scl-fw-dynamic-newlib1
  scl-fw-dynamic-newlib2

Or specify a path to any custom compiled .hex file.

Static app newlib variants can be controlled via wrappers:
  flash-run-sf.sh <app-name|hex>   - soft-float variant (sf)
  flash-run-hf.sh <app-name|hex>   - hard-float variant (hf)

Environment:
  RUN_MS          default timeout in ms (overridden by --timeout)
  SCL_BOARD       nucleo|weact (default weact)
EOF
}

if [[ ${#POSITIONAL[@]} -eq 0 ]]; then
    show_help
    exit 1
fi

set -- "${POSITIONAL[@]}"

case "$1" in
  scl-fw-dyn1|scl-fw-dyn2|scl-fw-ovl1|scl-fw-flatfs|scl-fw-gotplt-demo|scl-fw-flatfs-spi|scl-fw-flatfs-bitbang-spi)
                  HEX="$(find_hex "$1.hex")" ;;
  scl-fw-dynamic-newlib1) HEX="$(find_hex "$1.hex" "$STREE")" ;;
  scl-fw-dynamic-newlib2) HEX="$(find_hex "$1.hex" "$STREE")"
                  STAGE_DIR="$TESTDIR/platforms/board-hosted/vendor/libs/$SLIB" ;;
  *)              HEX="$1"
                  # a bare word that is not a file is almost certainly a typo'd app name
                  if [[ "$HEX" != */* && ! -f "$HEX" ]]; then
                    echo "error: unknown app name '$HEX'" >&2
                    echo "valid names: $VALID" >&2
                    exit 1
                  fi ;;
esac
[[ "$1" == scl-fw-dynamic-newlib* ]] && echo "static variant: $VARIANT (tree $STREE, fixtures libs/$SLIB)"
[[ -n "$HEX" && -f "$HEX" ]] || { echo "error: firmware .hex not found for '$1'" >&2
  echo "build it first (from the SCL root):" >&2
  echo "  freestanding (dyn1/dyn2/flatfs/ovl1): cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake && cmake --build test-dynamic/build -j" >&2
  echo "  hosted (newlib1/newlib2):             cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake && cmake --build test/build-dynamic-newlib -j" >&2
  exit 1; }
echo "flashing: $HEX"

# Some suites (static-unify DYN) dlopen hardcoded /tmp/*.so paths: stage their
# fixture libraries first, from the build (sf) or build_hf (hf) fixture dir.
#
# ABI GUARD: the fixtures' float ABI MUST match the firmware. A hard-float .so
# (VFP) run by a soft-float firmware (FPU off) faults with NOCP the instant it
# executes a VFP instruction (e.g. get_pi). We stage the right set here, then
# verify /tmp actually matches the variant, so a stale /tmp can't reach the board.
READELF="${ARM_READELF:-arm-none-eabi-readelf}"   # on PATH, or set ARM_READELF
fixture_is_hard() {   # 0 (true) if $1 is a hard-float (VFP) object
    "$READELF" -A "$1" 2>/dev/null | grep -q 'Tag_ABI_VFP_args'
}
if [[ -n "${STAGE_DIR:-}" ]]; then
  [[ -d "$STAGE_DIR" ]] || { echo "error: fixture dir not found: $STAGE_DIR (build the $VARIANT fixtures first)" >&2; exit 1; }
  echo "staging $(ls "$STAGE_DIR"/*.so | wc -l) fixture .so files from $STAGE_DIR to /tmp"
  cp -f "$STAGE_DIR"/*.so /tmp/
  # verify the staged ABI matches the variant (belt-and-suspenders against a
  # stale /tmp or a failed copy); skip quietly if readelf isn't available
  probe=/tmp/c_test1.so
  if [[ -f $probe ]] && command -v "$READELF" >/dev/null 2>&1; then
    if fixture_is_hard "$probe"; then have=hf; else have=sf; fi
    if [[ "$have" != "$VARIANT" ]]; then
      echo "ABI MISMATCH: /tmp fixtures are $have but you are flashing the $VARIANT firmware." >&2
      echo "  a $have .so run by the $VARIANT firmware would HardFault (NOCP) on the first float call." >&2
      echo "  fix: run the matching wrapper -> flash-run-$VARIANT.sh $1" >&2
      exit 1
    fi
    echo "ABI check: /tmp fixtures are $have, matches $VARIANT firmware — OK"
  fi
fi

CMDLINE_ARGS=()
if [[ "$1" == *flatfs* ]]; then
  CMDLINE_ARGS=(-c "arm semihosting_cmdline -c c1.so c2.so cpp3.so libcppeh.so ehx_catch.so notelf.bin")
fi

# Kill any stale OpenOCD from a previous run (holds the debug probe locked).
killall -s 9 openocd 2>/dev/null || true

# Build the OpenOCD command line.
OPENOCD_CMD=(
  "$OPENOCD" -s "$OPENOCD_SCRIPTS"
  -f "$CFG"
  -c "init"
  -c "reset halt"
  -c "program $HEX"
  -c "arm semihosting enable"
  "${CMDLINE_ARGS[@]}"
  -c "reset run"
)

# With --timeout (or RUN_MS), append sleep + shutdown so OpenOCD exits cleanly
# after the specified duration. Without it, OpenOCD runs until Ctrl-C.
if [[ -n "$TIMEOUT" ]]; then
    echo "timeout: ${TIMEOUT}ms (auto-shutdown)"
    OPENOCD_CMD+=(-c "sleep $TIMEOUT" -c "shutdown")
fi

# With --log, tee the semihosting console to the log file.
if [[ -n "$LOGFILE" ]]; then
    echo "logging to: $LOGFILE"
    exec "${OPENOCD_CMD[@]}" 2>&1 | tee "$LOGFILE"
else
    exec "${OPENOCD_CMD[@]}"
fi

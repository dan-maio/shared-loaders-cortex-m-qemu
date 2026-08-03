#!/usr/bin/env bash
# flash_reloc_run.sh — build the scl::reloc test app + its ET_REL fixtures, then
# flash it to a WeAct BlackPill STM32F411CE and run it over the semihosting console.
#
#   test-relocatable/tools/flash_reloc_run.sh [reloc|classic] # configure + build + flash + run
#   Ctrl-C to stop. The output prints on the semihosting console.
#
# The app opens the fixtures (relprov/relmod .rel.o) from RELOC_LIB_DIR — an
# ABSOLUTE path baked in at build time — so, unlike the ET_DYN static suite,
# there is no /tmp fixture staging and no float-ABI dance. Reuses the shared
# flash-run.sh for the proven OpenOCD program+semihosting flow.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTRELOC="$HERE/.."                 # test-relocatable/
ROOT="$HERE/../.."                   # repo root
BUILD="$TESTRELOC/build"

echo "=== configure + build (CMake also builds the ET_REL fixtures) ==="
cmake -S "$TESTRELOC" -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake"
cmake --build "$BUILD" -j

TARGET="${1:-reloc}"
case "$TARGET" in
  reloc)   HEX="$BUILD/scl-fw-reloc.hex" ;;
  classic) HEX="$BUILD/scl-fw-classic-reloc.hex" ;;
  *)       echo "error: unknown target '$TARGET' (valid: reloc, classic)" >&2; exit 1 ;;
esac

[[ -f "$HEX" ]] || { echo "error: $HEX not built" >&2; exit 1; }

echo "=== flash + run $TARGET (semihosting console follows) ==="
exec "$ROOT/test/tools/flash-run.sh" "$HEX"

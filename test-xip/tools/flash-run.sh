set -euo pipefail

# Configure + build the XIP test tree, then flash + run one app on the board
# (delegating the OpenOCD launch to the shared test/tools/flash-run.sh).
#
#   test-xip/tools/flash-run.sh [nolib1|newlib1|static]

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTXIP="$HERE/.."                   # test-xip/
ROOT="$HERE/../.."                   # repo root
BUILD="$TESTXIP/build"

echo "=== configure + build test-xip ==="
cmake -S "$TESTXIP" -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake"
cmake --build "$BUILD" -j

TARGET="${1:-nolib1}"
case "$TARGET" in
  nolib1)  HEX="$(find "$BUILD" -name scl-fw-xip-nolib1.hex  | head -1)" ;;
  newlib1) HEX="$(find "$BUILD" -name scl-fw-xip-newlib1.hex | head -1)" ;;
  static)  HEX="$(find "$BUILD" -name scl-fw-xip-static.hex  | head -1)" ;;
  *)       echo "error: unknown target '$TARGET' (valid: nolib1, newlib1, static)" >&2; exit 1 ;;
esac

[[ -n "$HEX" && -f "$HEX" ]] || { echo "error: firmware .hex for '$TARGET' not built" >&2; exit 1; }

echo "=== flash + run $TARGET (semihosting console follows) ==="
exec "$ROOT/test/tools/flash-run.sh" "$HEX"

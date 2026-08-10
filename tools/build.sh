#!/usr/bin/env bash
# Compile C++20 host tools using common-elf-reader.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

COMMON="../common-elf-reader"
CPPFLAGS="-std=c++20 -I${COMMON}/include ${COMMON}/src/elf-image.cpp"

echo "=== Building C++20 gen-symtab host tool ==="
g++ $CPPFLAGS gen-symtab.cpp -o gen-symtab
echo "=== Done: tools/gen-symtab ==="

echo "=== Building C++20 elf2json host tool ==="
g++ $CPPFLAGS elf2json.cpp -o elf2json
echo "=== Done: tools/elf2json ==="

echo ""
echo "Usage:"
echo "  ./gen-symtab <firmware.elf> <out.bin>"
echo "  ./elf2json <input.elf> [output.json]   # export ELF for AetherELF GUI"

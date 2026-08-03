#!/usr/bin/env bash
# Compile the C++20 host symbol table generator using common-elf-reader.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

echo "=== Building C++20 gen-symtab host tool ==="
g++ -std=c++20 gen-symtab.cpp ../common-elf-reader/src/elf-image.cpp \
    -I../common-elf-reader/include \
    -o gen-symtab

echo "=== Done: tools/gen-symtab compiled successfully ==="

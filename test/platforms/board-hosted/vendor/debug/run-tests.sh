#!/bin/bash
# Run host test suite (build + test)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"

make -C "$PROJECTS/test" test

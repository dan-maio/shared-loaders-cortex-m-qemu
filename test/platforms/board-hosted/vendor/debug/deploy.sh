#!/bin/bash
# Deploy .so files to /tmp/ for semihosting access
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"

cp "$PROJECTS/libs/build/mylib1.so" \
   "$PROJECTS/libs/build/mylib2.so" \
   "$PROJECTS/libs/build/libcppsh.so" /tmp/ 2>/dev/null || true
echo "Deployed .so files to /tmp/:"
ls -lh /tmp/mylib*.so /tmp/libcppsh.so 2>/dev/null

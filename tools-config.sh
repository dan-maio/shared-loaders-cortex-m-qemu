#!/bin/bash
# tools-config.sh — Central configuration for ARM toolchain and OpenOCD paths.
# Source this file from any script that needs these tools.
#
# Usage:
#   source "$(dirname "$0")/tools-config.sh"
#   # or
#   source "$REPO_ROOT/tools-config.sh"
#
# Override any variable by setting it before sourcing this file, or by
# exporting the corresponding environment variable.

# ARM GCC Toolchain
: "${ARM_TOOLCHAIN_BIN:=$(dirname "$(which arm-none-eabi-gcc 2>/dev/null)" 2>/dev/null)}"
: "${ARM_TOOLCHAIN_BIN:=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"

# OpenOCD
: "${OPENOCD:=$(which openocd 2>/dev/null)}"
: "${OPENOCD:=/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin/openocd}"

# OpenOCD Scripts
: "${OPENOCD_SCRIPTS:=/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts}"

# QEMU (ARM system emulator, Cortex-M4)
: "${QEMU:=$(which qemu-system-arm 2>/dev/null)}"
: "${QEMU:=qemu-system-arm}"

# arm-none-eabi-gdb (sits beside arm-none-eabi-gcc in the xPacks tree)
: "${ARM_GDB:=$ARM_TOOLCHAIN_BIN/arm-none-eabi-gdb}"
if ! command -v "$ARM_GDB" >/dev/null 2>&1; then
    : "${ARM_GDB:=$(which arm-none-eabi-gdb 2>/dev/null)}"
fi

export ARM_TOOLCHAIN_BIN OPENOCD OPENOCD_SCRIPTS QEMU ARM_GDB

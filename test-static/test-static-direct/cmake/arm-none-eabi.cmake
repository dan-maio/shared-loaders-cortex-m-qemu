# Cross-compilation toolchain for the STM32F411RE (Cortex-M4F, soft-float).
#
# Use:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# Override the toolchain location with -DTOOLCHAIN_PATH=/path/to/bin (or put
# arm-none-eabi-gcc on PATH and set the compilers directly).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PATH
    "/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin"
    CACHE PATH "arm-none-eabi toolchain bin directory")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PATH}/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/arm-none-eabi-g++")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PATH}/arm-none-eabi-objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${TOOLCHAIN_PATH}/arm-none-eabi-objdump" CACHE FILEPATH "")
set(CMAKE_SIZE_UTIL    "${TOOLCHAIN_PATH}/arm-none-eabi-size"    CACHE FILEPATH "")

# The firmware has no hosted runtime, so a normal link link test would fail; build a
# static library for compiler feature probing instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M4, Thumb, soft float — applied to every target.
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=soft")
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")

# Only search the target sysroot for libraries/headers, the host for programs.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

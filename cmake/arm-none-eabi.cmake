# Toolchain file for the STM32F411CE (Cortex-M4F) test applications.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=<scl>/cmake/arm-none-eabi.cmake ...
# Override the toolchain location with -DSCL_ARM_TOOLCHAIN_BIN=... or by having
# arm-none-eabi-gcc on PATH.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Try to find arm-none-eabi-gcc on PATH first, fall back to default
find_program(_ARM_GCC arm-none-eabi-gcc)
if(_ARM_GCC)
    get_filename_component(SCL_ARM_TOOLCHAIN_BIN "${_ARM_GCC}" DIRECTORY)
else()
    set(SCL_ARM_TOOLCHAIN_BIN
        "/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin"
        CACHE PATH "Directory containing arm-none-eabi-gcc/g++/objcopy")
endif()

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc     HINTS ${SCL_ARM_TOOLCHAIN_BIN} REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++     HINTS ${SCL_ARM_TOOLCHAIN_BIN} REQUIRED)
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy HINTS ${SCL_ARM_TOOLCHAIN_BIN} REQUIRED)

# Firmware executables link -nostdlib against custom linker scripts, so a
# try-compile of a full executable would fail; compile to a static lib instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Same verified flag set as the original Makefile firmware build:
#  - hard-float Cortex-M4, -fpic (PC-relative GOT, no r10 in main),
#  - -fno-tree-loop-distribute-patterns: stop gcc turning libc_shim loops back
#    into memcpy/memset calls (we link -nostdlib),
#  - exceptions/RTTI stay ON: main provides the C++ EH runtime to loaded libs.
set(SCL_MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(SCL_FW_FLAGS  "${SCL_MCU_FLAGS} -fpic -O0 -g -ffunction-sections -fno-common -ffreestanding -fno-tree-loop-distribute-patterns")
set(CMAKE_C_FLAGS_INIT   "${SCL_FW_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SCL_FW_FLAGS} -fno-threadsafe-statics")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

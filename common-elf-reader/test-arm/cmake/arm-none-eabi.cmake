# Toolchain file for cross-compiling the common-elf-reader test with ARM GCC.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -S . -B build
#
# Finds arm-none-eabi-g++ from PATH first, falls back to xPacks location.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Try PATH first, then xPacks default.
find_program(_ARM_GCC arm-none-eabi-gcc)
if(_ARM_GCC)
    get_filename_component(_ARM_BIN "${_ARM_GCC}" DIRECTORY)
else()
    set(_ARM_BIN
        "$ENV{HOME}/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin")
endif()

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc     HINTS ${_ARM_BIN} REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++     HINTS ${_ARM_BIN} REQUIRED)
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy HINTS ${_ARM_BIN} REQUIRED)

# Cross-compiling: skip link tests (custom linker scripts + -nostdlib).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M4 hard-float, debug symbols.
set(_MCU "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(_FW  "${_MCU} -O0 -g -ffunction-sections -fdata-sections -fno-common")

set(CMAKE_C_FLAGS_INIT   "${_FW}")
set(CMAKE_CXX_FLAGS_INIT "${_FW} -fno-threadsafe-statics -fno-exceptions -fno-rtti -D_GLIBCXX_NO_ASSERTIONS")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

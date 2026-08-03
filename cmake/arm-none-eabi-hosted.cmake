# Toolchain file for HOSTED Cortex-M4 firmware (newlib/nano, CMSIS/HAL style —
# e.g. static-unify_code): non-PIC, not freestanding, heap available. This is
# the environment the SCL's hosted parts (SCL_HOSTED: relocator, scl-static)
# are meant for. Float ABI: -DSCL_HF=ON for hard float (default soft, matching
# the original Makefile_DYN/NO_DYN default).
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

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

option(SCL_HF "Hard-float ABI (fpv4-sp-d16); OFF = soft float" OFF)
if(SCL_HF)
    set(SCL_MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
else()
    set(SCL_MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=soft")
endif()

# Same verified flag set as Makefile_DYN/Makefile_NO_DYN (ARM_OPT + -g3).
set(CMAKE_C_FLAGS_INIT   "${SCL_MCU_FLAGS} -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3")
set(CMAKE_CXX_FLAGS_INIT "${SCL_MCU_FLAGS} -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -fno-move-loop-invariants")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

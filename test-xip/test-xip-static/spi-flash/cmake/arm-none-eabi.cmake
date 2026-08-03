set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PATH
    /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
    CACHE PATH "arm-none-eabi toolchain bin directory")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PATH}/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/arm-none-eabi-g++")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PATH}/arm-none-eabi-objcopy" CACHE FILEPATH "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CPU_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(SCL_MCU_FLAGS "${CPU_FLAGS}")

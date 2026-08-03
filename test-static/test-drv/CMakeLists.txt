cmake_minimum_required(VERSION 3.20)
project(scl-drv-tests LANGUAGES C CXX)

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake")
endif()

if(NOT TARGET scl-static)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
endif()

string(REPLACE "-fpic " "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-fpic " "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-ffreestanding " "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-ffreestanding " "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-mfloat-abi=hard -mfpu=fpv4-sp-d16" "-mfloat-abi=soft" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-mfloat-abi=hard -mfpu=fpv4-sp-d16" "-mfloat-abi=soft" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

set(DRV_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ST ${SCL_ROOT}/test-static/test-static)

# UART fixture baud divisor depends on PCLK2 (NUCLEO 84 MHz / WEACT 100 MHz).
if(SCL_BOARD STREQUAL "nucleo")
    set(UART_PCLK 84000000)
else()
    set(UART_PCLK 100000000)
endif()

set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra)

function(add_shlib name)
    add_custom_target(build_${name}
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} -I${DRV_DIR}/fixtures/include
                -DUART_PCLK=${UART_PCLK}
                -c ${DRV_DIR}/fixtures/src/${name}.c -o ${DRV_DIR}/fixtures/src/${name}.o
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} ${DRV_DIR}/fixtures/src/${name}.o
                -o ${DRV_DIR}/fixtures/lib${name}.so -shared -nostartfiles
                -T ${DRV_DIR}/fixtures/ldscripts/mem.ld -Wl,--gc-sections
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib(spi)
add_shlib(uart)
add_shlib(led)

# C++ fixture — needs C++ compiler, -nostdlib (self-contained new/delete)
set(LIB_CXXFLAGS ${LIB_CFLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics)
function(add_shlib_cpp name)
    add_custom_target(build_cpp_${name}
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} -I${DRV_DIR}/fixtures/include
                -c ${DRV_DIR}/fixtures/src/${name}.cpp -o ${DRV_DIR}/fixtures/src/${name}.o
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} ${DRV_DIR}/fixtures/src/${name}.o
                -o ${DRV_DIR}/fixtures/lib${name}.so -shared -nostartfiles -nostdlib
                -T ${DRV_DIR}/fixtures/ldscripts/mem.ld -Wl,--gc-sections,-S
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib_cpp(cpp_features)

add_executable(scl-fw-drv
# --- board selection (unified platform; see test/platforms/board/include/board.h) ---
set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
if(SCL_BOARD STREQUAL "nucleo")
    set(BOARD_DEFS SCL_BOARD_NUCLEO=1)
    set(HSE_VAL 16000000)
elseif(SCL_BOARD STREQUAL "weact")
    set(BOARD_DEFS SCL_BOARD_WEACT=1)
    set(HSE_VAL 25000000)
else()
    message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
endif()


    src/main.cpp
    ${ST}/src/initialize-hardware.c
    ${ST}/system/src/cmsis/system_stm32f4xx.c
    ${ST}/system/src/cmsis/vectors_stm32f411xe.c
    ${ST}/system/src/cortexm/exception-handlers.c
    ${ST}/system/src/cortexm/initialize-hardware.c
    ${ST}/system/src/cortexm/reset-hardware.c
    ${ST}/system/src/diag/trace-impl.c
    ${ST}/system/src/diag/trace.c
    ${ST}/system/src/newlib/assert.c
    ${ST}/system/src/newlib/exit.c
    ${ST}/system/src/newlib/sbrk.c
    ${ST}/system/src/newlib/startup.c
    ${ST}/system/src/newlib/syscalls.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c)

target_include_directories(scl-fw-drv PRIVATE
    ${DRV_DIR}/fixtures/include
    ${ST}/include ${ST}/system/include ${ST}/system/include/cmsis ${ST}/system/include/stm32f4-hal)

target_link_libraries(scl-fw-drv PRIVATE scl::static
    ${DRV_DIR}/fixtures/libspi.so
    ${DRV_DIR}/fixtures/libuart.so
    ${DRV_DIR}/fixtures/libled.so
    ${DRV_DIR}/fixtures/libcpp_features.so)

target_compile_definitions(scl-fw-drv PRIVATE
    STM32F411xE USE_HAL_DRIVER HSE_VALUE=${HSE_VAL} ${BOARD_DEFS} OS_USE_SEMIHOSTING)

target_compile_options(scl-fw-drv PRIVATE
    -O0 -g3 -Wall -Wextra -ffunction-sections -fdata-sections
    -DDEBUG -DTRACE -DOS_USE_TRACE_SEMIHOSTING_DEBUG
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20 -fno-rtti -fno-exceptions>)

target_link_options(scl-fw-drv PRIVATE
    -T${ST}/ldscripts/mem-sh.ld
    -T${ST}/ldscripts/libs-sh.ld
    -T${ST}/ldscripts/sections-sh.ld
    -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs
    -Wl,--no-warn-rwx-segments
)

add_dependencies(scl-fw-drv build_spi build_uart build_led build_cpp_cpp_features)

add_custom_command(TARGET scl-fw-drv POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:scl-fw-drv> $<TARGET_FILE_DIR:scl-fw-drv>/scl-fw-drv.hex)

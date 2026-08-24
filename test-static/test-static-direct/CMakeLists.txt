# scl-static-direct firmware — STM32F411CE (WeAct) with runtime ELF32 static patcher.
# Direct call variant (replaces pointer lookup with direct linking and GOT/PLT patching).
#
#   cmake -S test-static-direct -B test-static-direct/build \
#         -DCMAKE_TOOLCHAIN_FILE=test-static-direct/cmake/arm-none-eabi.cmake
#   cmake --build test-static-direct/build
#

cmake_minimum_required(VERSION 3.20)
project(scl-static-direct LANGUAGES C CXX)

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "Configure with the ARM toolchain, e.g.\n"
        "  cmake -S test-static-direct -B test-static-direct/build "
        "-DCMAKE_TOOLCHAIN_FILE=test-static-direct/cmake/arm-none-eabi.cmake")
endif()

# Standalone configure: pull in SCL to get scl::static
if(NOT TARGET scl-static)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
endif()

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
# --- board selection (unified platform; see test/platforms/board/include/board.h) ---
set(SCL_PLATFORM "board" CACHE STRING "Target platform (board for nucleo/weact, or qemu-f411re for QEMU)")
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(BOARD_DEFS SCL_BOARD_QEMU=1)
    set(HSE_VAL 16000000)
else()
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
endif()


set(ST ${SCL_ROOT}/test-static/test-static)
set(ST_CPP ${SCL_ROOT}/test-static/test-static-cpp)

# ── Shared libraries (position-independent, linked at RAM base 0) ────────────
set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra -DSTM32F411RxE)

function(add_shlib name srcdir srcfile)
    set(so  "${APP_DIR}/${srcdir}/lib${name}.so")
    set(obj "${APP_DIR}/${srcdir}/${srcfile}.o")
    file(MAKE_DIRECTORY "${APP_DIR}/${srcdir}")
    add_custom_command(
        OUTPUT  "${so}"
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} -I${ST}/${srcdir}
                -c "${ST}/${srcdir}/${srcfile}.c" -o "${obj}"
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} "${obj}" -o "${so}"
                -shared -nostartfiles
                -T "${ST}/${srcdir}/ldscripts/gcc.ld"
                -L "${ST}/${srcdir}/ldscripts"
                -Wl,--gc-sections -Wl,--no-warn-rwx-segments
        COMMAND ${CMAKE_OBJCOPY} -O binary "${so}" "${APP_DIR}/${srcdir}/lib${name}.bin"
        COMMAND ${CMAKE_SIZE_UTIL} "${so}"
        DEPENDS "${ST}/${srcdir}/${srcfile}.c"
                "${ST}/${srcdir}/${srcfile}.h"
        WORKING_DIRECTORY "${APP_DIR}"
        COMMENT "Building lib${name}.so"
        VERBATIM)
    add_custom_target(${name} DEPENDS "${so}")
endfunction()

add_shlib(sh1 libsh1 mylib1)
add_shlib(sh2 libsh2 mylib2)
add_shlib(tls_lib tls-lib tls_lib)

# C++ features shared library
set(CPP_FEATURES_SO "${APP_DIR}/new-lib/libcpp_features.so")
set(CPP_FEATURES_OBJ "${APP_DIR}/new-lib/cpp_features.o")
file(MAKE_DIRECTORY "${APP_DIR}/new-lib")
add_custom_command(
    OUTPUT  "${CPP_FEATURES_SO}"
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CFLAGS}
            -fno-exceptions -fno-rtti -fno-threadsafe-statics
            -I${ST_CPP}/new-lib
            -c "${ST_CPP}/new-lib/cpp_features.cpp" -o "${CPP_FEATURES_OBJ}"
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CFLAGS}
            -fno-exceptions -fno-rtti -fno-threadsafe-statics
            "${CPP_FEATURES_OBJ}" -o "${CPP_FEATURES_SO}"
            -shared -nostartfiles -nostdlib
            -Wl,--gc-sections -Wl,--no-warn-rwx-segments
    DEPENDS "${ST_CPP}/new-lib/cpp_features.cpp"
            "${ST_CPP}/new-lib/cpp_features.h"
    COMMENT "Building libcpp_features.so"
    VERBATIM)
add_custom_target(cpp_features DEPENDS "${CPP_FEATURES_SO}")

# ── Firmware application (loader comes from scl::static) ──────────────────────
# Under QEMU use the QEMU clock init (no HSE/PLL ready-bit polling).
set(C_SRCS
    $<$<STREQUAL:${SCL_PLATFORM},qemu-f411re>:${ST}/src/initialize-hardware-qemu.c>
    $<$<NOT:$<STREQUAL:${SCL_PLATFORM},qemu-f411re>>:${ST}/src/initialize-hardware.c>
    ${ST}/src/stm32f4xx-hal-msp.c
    ${ST}/src/write.c
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
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c)

set(CXX_SRCS
    src/main.cpp
    ${ST}/src/led.cpp
    ${ST}/src/timer.cpp
    ${ST}/system/src/newlib/cxx.cpp)

add_executable(scl-fw-static-direct ${C_SRCS} ${CXX_SRCS})
set_target_properties(scl-fw-static-direct PROPERTIES SUFFIX ".elf")
add_dependencies(scl-fw-static-direct sh1 sh2 tls_lib cpp_features)

target_include_directories(scl-fw-static-direct PRIVATE
    src
    ${ST}/include
    ${ST}/system/include
    ${ST}/system/include/cmsis
    ${ST}/system/include/stm32f4-hal
    ${ST}/libsh1
    ${ST}/libsh2
    ${ST}/tls-lib
    ${ST_CPP}/new-lib)

# The loader (elf_reader/shlib_loader/logger) + the common-elf-reader parser
# arrive from scl::static as INTERFACE sources, compiled into target with the
# flags below (no .a).
target_link_libraries(scl-fw-static-direct PRIVATE scl::static)

target_compile_options(scl-fw-static-direct PRIVATE
    -O0 -g3 -Wall -Wextra
    -fmessage-length=0 -fsigned-char
    -ffunction-sections -fdata-sections -fno-move-loop-invariants
    -DDEBUG -DUSE_FULL_ASSERT -DOS_USE_SEMIHOSTING -DTRACE
    -DOS_USE_TRACE_SEMIHOSTING_DEBUG -DSTM32F411xE -DUSE_HAL_DRIVER -DHSE_VALUE=${HSE_VAL} -D${BOARD_DEFS}
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

target_link_options(scl-fw-static-direct PRIVATE
    -T${ST}/ldscripts/mem-sh.ld
    -T${ST}/ldscripts/libs-sh.ld
    -T${ST}/ldscripts/sections-sh.ld
    -nostartfiles -Wl,--gc-sections --specs=nano.specs -u _printf_float
    -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
    -L${APP_DIR}/libsh1 -L${APP_DIR}/libsh2 -L${APP_DIR}/tls-lib -L${APP_DIR}/new-lib
    -lsh1 -lsh2 -ltls_lib -lcpp_features -lstdc++ -lgcc)

add_custom_command(TARGET scl-fw-static-direct POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:scl-fw-static-direct> $<TARGET_FILE_DIR:scl-fw-static-direct>/scl-fw-static-direct.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-static-direct> $<TARGET_FILE_DIR:scl-fw-static-direct>/scl-fw-static-direct.bin
    COMMAND ${CMAKE_OBJDUMP} -DSzw $<TARGET_FILE:scl-fw-static-direct> > $<TARGET_FILE_DIR:scl-fw-static-direct>/scl-fw-static-direct.elf.lst
    COMMAND ${CMAKE_SIZE_UTIL} $<TARGET_FILE:scl-fw-static-direct>
    VERBATIM)

# Run under QEMU, or flash via OpenOCD on the board.
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    enable_testing()
    add_test(NAME scl-fw-static-direct-qemu
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../test/tools/qemu-run.sh
                     --machine netduinoplus2
                     --arg scl-fw-static-direct
                     --arg libsh1/libsh1.so --arg libsh2/libsh2.so
                     --arg new-lib/libcpp_features.so
                     --done "Second 1"
                     $<TARGET_FILE:scl-fw-static-direct>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

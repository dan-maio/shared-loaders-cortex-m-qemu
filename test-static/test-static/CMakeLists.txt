# scl-static demo firmware — STM32F411CE (WeAct) with the runtime ELF32 static patcher.
# The loader itself (elf_reader adapter + shlib_loader + logger) is the SCL
# module scl::static (src/static + include/static), which brings the shared
# common-elf-reader parser transitively. This app just builds the two demo .so
# and the firmware, and links scl::static.
#
#   cmake -S test-static/test-static -B test/build-static \
#         -DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake
#   cmake --build test/build-static
#
# Produces libsh1/libsh1.so, libsh2/libsh2.so and build-static/f411ce-sh.elf.

cmake_minimum_required(VERSION 3.20)
project(scl-static-patcher LANGUAGES C CXX)

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "Configure with the ARM toolchain, e.g.\n"
        "  cmake -S test-static/test-static -B test/build-static "
        "-DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake")
endif()

# Standalone configure: pull in the SCL to get scl::static (which links the
# common-elf-reader submodule parser). When built inside the SCL tree the target
# already exists.
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



# ── Shared libraries (position-independent, linked at RAM base 0) ────────────
# Bare-metal PIC .so built with custom linker scripts; a custom command mirrors
# the Makefile (CMake SHARED targets don't fit the Generic system).
set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra -DSTM32F411RxE)

function(add_shlib name srcdir srcfile)
    set(so  "${APP_DIR}/${srcdir}/lib${name}.so")
    set(obj "${APP_DIR}/${srcdir}/${srcfile}.o")
    add_custom_command(
        OUTPUT  "${so}"
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} -I${APP_DIR}/${srcdir}
                -c "${APP_DIR}/${srcdir}/${srcfile}.c" -o "${obj}"
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} "${obj}" -o "${so}"
                -shared -nostartfiles
                -T "${APP_DIR}/${srcdir}/ldscripts/gcc.ld"
                -L "${APP_DIR}/${srcdir}/ldscripts"
                -Wl,--gc-sections -Wl,--no-warn-rwx-segments
        COMMAND ${CMAKE_OBJCOPY} -O binary "${so}" "${APP_DIR}/${srcdir}/lib${name}.bin"
        COMMAND ${CMAKE_SIZE_UTIL} "${so}"
        DEPENDS "${APP_DIR}/${srcdir}/${srcfile}.c"
                "${APP_DIR}/${srcdir}/${srcfile}.h"
        WORKING_DIRECTORY "${APP_DIR}"
        COMMENT "Building lib${name}.so"
        VERBATIM)
    add_custom_target(${name} DEPENDS "${so}")
endfunction()

add_shlib(sh1 libsh1 mylib1)
add_shlib(sh2 libsh2 mylib2)

# ── Firmware application (loader comes from scl::static) ──────────────────────
# Under QEMU (SCL_PLATFORM=qemu-f411re) use the QEMU clock init (no HSE/PLL
# ready-bit polling — the emulator never sets them); on hardware use the real one.
set(C_SRCS
    $<$<STREQUAL:${SCL_PLATFORM},qemu-f411re>:src/initialize-hardware-qemu.c>
    $<$<NOT:$<STREQUAL:${SCL_PLATFORM},qemu-f411re>>:src/initialize-hardware.c>
    src/stm32f4xx-hal-msp.c
    src/write.c
    system/src/cmsis/system_stm32f4xx.c
    system/src/cmsis/vectors_stm32f411xe.c
    system/src/cortexm/exception-handlers.c
    system/src/cortexm/initialize-hardware.c
    system/src/cortexm/reset-hardware.c
    system/src/diag/trace-impl.c
    system/src/diag/trace.c
    system/src/newlib/assert.c
    system/src/newlib/exit.c
    system/src/newlib/sbrk.c
    system/src/newlib/startup.c
    system/src/newlib/syscalls.c
    system/src/stm32f4-hal/stm32f4xx_hal.c
    system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    system/src/stm32f4-hal/stm32f4xx_hal_rcc.c)

set(CXX_SRCS
    src/main.cpp
    src/led.cpp
    src/timer.cpp
    system/src/newlib/cxx.cpp)

add_executable(f411ce-sh ${C_SRCS} ${CXX_SRCS})
set_target_properties(f411ce-sh PROPERTIES SUFFIX ".elf")
add_dependencies(f411ce-sh sh1 sh2)

target_include_directories(f411ce-sh PRIVATE
    include
    system/include
    system/include/cmsis
    system/include/stm32f4-hal
    libsh1 libsh2)

# The loader (elf_reader/shlib_loader/logger) + the common-elf-reader parser
# arrive from scl::static as INTERFACE sources, compiled into f411ce-sh with the
# flags below (no .a).
target_link_libraries(f411ce-sh PRIVATE scl::static)

target_compile_options(f411ce-sh PRIVATE
    -O0 -g3 -Wall -Wextra
    -fmessage-length=0 -fsigned-char
    -ffunction-sections -fdata-sections -fno-move-loop-invariants
    -DDEBUG -DUSE_FULL_ASSERT -DOS_USE_SEMIHOSTING -DTRACE
    -DOS_USE_TRACE_SEMIHOSTING_DEBUG -DSTM32F411xE -DUSE_HAL_DRIVER -DHSE_VALUE=${HSE_VAL} -D${BOARD_DEFS}
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

# -T<script> with no space: one argument, so CMake quotes it correctly even
# when the repo path contains spaces. Order: mem -> libs -> sections.
target_link_options(f411ce-sh PRIVATE
    -T${APP_DIR}/ldscripts/mem-sh.ld
    -T${APP_DIR}/ldscripts/libs-sh.ld
    -T${APP_DIR}/ldscripts/sections-sh.ld
    -nostartfiles -Wl,--gc-sections --specs=nano.specs -u _printf_float
    -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
    -L${APP_DIR}/libsh1 -L${APP_DIR}/libsh2
    -lsh1 -lsh2 -lstdc++)

add_custom_command(TARGET f411ce-sh POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:f411ce-sh> $<TARGET_FILE_DIR:f411ce-sh>/f411ce-sh.bin
    COMMAND ${CMAKE_OBJDUMP} -DSzw $<TARGET_FILE:f411ce-sh> > $<TARGET_FILE_DIR:f411ce-sh>/f411ce-sh.elf.lst
    COMMAND ${CMAKE_SIZE_UTIL} $<TARGET_FILE:f411ce-sh>
    VERBATIM)

# Run under QEMU with the .so paths delivered via the semihosting command line
# (argv[1]/argv[2] in main), or flash via OpenOCD on the board.
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    enable_testing()
    add_test(NAME f411ce-sh-qemu
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../test/tools/qemu-run.sh
                     --machine netduinoplus2
                     --arg f411ce-sh --arg libsh1/libsh1.so --arg libsh2/libsh2.so
                     --done "Second 1"
                     $<TARGET_FILE:f411ce-sh>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

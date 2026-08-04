# HOSTED platform library for the QEMU emulated NUCLEO-F411RE (CMSIS + ST HAL +
# newlib) — the environment the hosted static loader (scl::dynamic-newlib,
# SCL_HOSTED) needs. This is the QEMU counterpart of
# platforms/board-hosted: it reuses the vendored CMSIS/HAL/newlib tree
# under ../board-hosted/vendor but drives the clock with a QEMU-friendly
# config (the emulated RCC never signals HSE/PLL ready, so the hardware bring-up
# would hang) and reports LED state over semihosting.
#
# Use with cmake/arm-none-eabi-hosted.cmake — NOT the freestanding toolchain.
message(VERBOSE "Including platforms/qemu-f411re-hosted/platform-library.cmake...")

get_filename_component(STATIC_UNIFY_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../board-hosted/vendor ABSOLUTE)
if(NOT EXISTS ${STATIC_UNIFY_DIR}/common/system)
    message(FATAL_ERROR "vendored hosted platform not found under "
                        "test/platforms/board-hosted/vendor/")
endif()

set(QEMU_PLATFORM_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(QEMU_PLATFORM_DIR ${QEMU_PLATFORM_DIR} ABSOLUTE)

# The QEMU runner executes the firmware under qemu-system-arm instead of
# flashing via OpenOCD. These are set BEFORE the guard below so every sibling
# test directory that includes this file sees them (the set() calls after the
# return() would only be visible to the first includer).
set(PLATFORM_QEMU_F411RE_HOSTED_RUNNER
    ${QEMU_PLATFORM_DIR}/../../../test/tools/qemu-run.sh)
set(PLATFORM_QEMU_F411RE_HOSTED_QEMU_MACHINE netduinoplus2)

if(TARGET platform-qemu-f411re-hosted-interface)
    return()   # already defined by a sibling test app
endif()
add_library(platform-qemu-f411re-hosted-interface INTERFACE EXCLUDE_FROM_ALL)

set(_su ${STATIC_UNIFY_DIR})
target_sources(platform-qemu-f411re-hosted-interface INTERFACE
    ${_su}/firmware/src/symtab-blob.c              # flash placeholder, rewritten post-link
    # app-side loader glue (semihosting byte_source, load front-end, log bridge)
    ${_su}/common/firmware/src/semihost-file.cpp
    ${_su}/common/firmware/src/dl-load.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    # board helpers + HAL bring-up (QEMU variants of the clock + LED)
    ${QEMU_PLATFORM_DIR}/src/led.cpp
    ${_su}/common/firmware/src/timer.cpp
    ${QEMU_PLATFORM_DIR}/src/initialize-hardware.c
    ${_su}/common/firmware/src/stm32f4xx_hal_msp.c
    # system: CMSIS + cortexm + diag + newlib glue
    ${_su}/common/system/src/cmsis/system_stm32f4xx.c
    ${_su}/common/system/src/cmsis/vectors_stm32f411xe.c
    ${_su}/common/system/src/cortexm/exception-handlers.c
    ${_su}/common/system/src/cortexm/initialize-hardware.c
    ${_su}/common/system/src/cortexm/reset-hardware.c
    ${_su}/common/system/src/diag/trace.c
    ${_su}/common/system/src/diag/trace-impl.c
    ${_su}/common/system/src/newlib/assert.c
    ${_su}/common/system/src/newlib/exit.c
    ${_su}/common/system/src/newlib/sbrk.c
    ${_su}/common/system/src/newlib/startup.c
    ${_su}/common/system/src/newlib/syscalls.c
    ${_su}/common/system/src/newlib/cxx.cpp
    # HAL subset
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
)
target_include_directories(platform-qemu-f411re-hosted-interface INTERFACE
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include)
target_compile_definitions(platform-qemu-f411re-hosted-interface INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER HSE_VALUE=16000000
    $<$<BOOL:${SCL_HF}>:HARD_FLOAT>)

# Link recipe of the DYN firmware: newlib-nano, the _reloc-table linker
# scripts (mem-sh + libs-sh + sections-dyn keep .dynsym/.rel.dyn and emit the
# _reloc table the static link_context reads).
target_link_options(platform-qemu-f411re-hosted-interface INTERFACE
    -nostartfiles --specs=nano.specs LINKER:-u,_printf_float
    -T${_su}/common/ldscripts/mem-sh.ld
    -T${_su}/common/ldscripts/libs-sh.ld
    -T${_su}/firmware/ldscripts/sections-dyn.ld
    LINKER:-E)
target_link_libraries(platform-qemu-f411re-hosted-interface INTERFACE stdc++ m)

# --- Link-time loader symbol table -------------------------------------------
# (identical to the hardware platform: symtab-blob.c reserves a zero-filled
# region in flash; the real table is objcopy'd in after the link.)
set(SCL_SYMTAB_BLOB_KB 88 CACHE STRING
    "Flash reserved for the link-time loader symbol table, in KB")
target_compile_definitions(platform-qemu-f411re-hosted-interface INTERFACE
    SCL_SYMTAB_BLOB_KB=${SCL_SYMTAB_BLOB_KB})

function(scl_embed_symtab target)
    add_dependencies(${target} gen-symtab-host)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${SCL_HOST_GEN_SYMTAB}"
                $<TARGET_FILE:${target}> $<TARGET_FILE:${target}>.symtab.bin
        COMMAND ${CMAKE_OBJCOPY} --update-section
                .scl_symtab=$<TARGET_FILE:${target}>.symtab.bin
                $<TARGET_FILE:${target}>
        COMMENT "Embedding loader symbol table into ${target}"
        VERBATIM)
endfunction()

# The QEMU runner variables are defined at the top of this file (before the
# target guard) so every includer sees them.
set(STATIC_UNIFY_DIR ${STATIC_UNIFY_DIR})

add_library(platform::qemu-f411re-hosted ALIAS platform-qemu-f411re-hosted-interface)
message(VERBOSE "> platform::qemu-f411re-hosted -> platform-qemu-f411re-hosted-interface")

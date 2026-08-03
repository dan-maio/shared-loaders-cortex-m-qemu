# Unified HOSTED platform library (CMSIS + ST HAL + newlib) for the static
# loader (scl::dynamic-newlib, SCL_HOSTED) firmware apps.
#
# One INTERFACE target serves BOTH supported boards; the active board is chosen
# with -DSCL_BOARD=nucleo|weact (default weact). Only board boot/init differs:
# the clock configuration in vendor/common/firmware/src/initialize-hardware.c
# (HSE 16 MHz -> 84 MHz for NUCLEO, HSE 25 MHz -> 100 MHz for WEACT), the
# HSE_VALUE define, and the OpenOCD flash config (ST-LINK vs CMSIS-DAP).
#
# Use with cmake/arm-none-eabi-hosted.cmake — NOT the freestanding toolchain
# (different flags: non-PIC, newlib, no -ffreestanding).
message(VERBOSE "Including platforms/board-hosted/platform-library.cmake...")

# Self-contained: the CMSIS/HAL/newlib platform lives under vendor/ (copied
# from the original static-unify_code project). No sibling dependency.
get_filename_component(STATIC_UNIFY_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../vendor ABSOLUTE)
if(NOT EXISTS ${STATIC_UNIFY_DIR}/common/system)
    message(FATAL_ERROR "vendored hosted platform not found under "
                        "test/platforms/board-hosted/vendor/")
endif()

# --- board selection ---------------------------------------------------------
set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
set_property(CACHE SCL_BOARD PROPERTY STRINGS nucleo weact)
if(NOT SCL_BOARD STREQUAL "nucleo" AND NOT SCL_BOARD STREQUAL "weact")
    message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
endif()

if(TARGET platform-board-hosted-interface)
    return()   # already defined by a sibling test app
endif()
add_library(platform-board-hosted-interface INTERFACE EXCLUDE_FROM_ALL)

set(_su ${STATIC_UNIFY_DIR})
target_sources(platform-board-hosted-interface INTERFACE
    ${_su}/firmware/src/symtab-blob.c              # flash placeholder, rewritten post-link
    # app-side loader glue (semihosting byte_source, load front-end, log bridge)
    ${_su}/common/firmware/src/semihost-file.cpp
    ${_su}/common/firmware/src/dl-load.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    # board helpers + HAL bring-up
    ${_su}/common/firmware/src/led.cpp
    ${_su}/common/firmware/src/timer.cpp
    ${_su}/common/firmware/src/initialize-hardware.c
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
target_include_directories(platform-board-hosted-interface INTERFACE
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include)

# Board boot/init switch + HSE frequency used by initialize-hardware.c and the
# HAL clock tree.
if(SCL_BOARD STREQUAL "nucleo")
    target_compile_definitions(platform-board-hosted-interface INTERFACE
        SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
else()
    target_compile_definitions(platform-board-hosted-interface INTERFACE
        SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
endif()
target_compile_definitions(platform-board-hosted-interface INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER
    $<$<BOOL:${SCL_HF}>:HARD_FLOAT>)

# Link recipe of the DYN firmware: newlib-nano, the _reloc-table linker
# scripts (mem-sh + libs-sh + sections-dyn keep .dynsym/.rel.dyn and emit the
# _reloc table the static link_context reads).
target_link_options(platform-board-hosted-interface INTERFACE
    -nostartfiles --specs=nano.specs LINKER:-u,_printf_float
    -T${_su}/common/ldscripts/mem-sh.ld
    -T${_su}/common/ldscripts/libs-sh.ld
    -T${_su}/firmware/ldscripts/sections-dyn.ld
    LINKER:-E)
target_link_libraries(platform-board-hosted-interface INTERFACE stdc++ m)

# --- Link-time loader symbol table -------------------------------------------
# symtab-blob.c reserves a zero-filled region in flash; after the link we render
# the real table from the ELF and objcopy it into that region. The section keeps
# its address and size, so no symbol moves and the baked-in addresses stay valid.
#
# Call scl_embed_symtab(<target>) BEFORE the target's .hex/.bin conversion, or
# the images will be made from the un-updated ELF.
set(SCL_SYMTAB_BLOB_KB 88 CACHE STRING
    "Flash reserved for the link-time loader symbol table, in KB")
target_compile_definitions(platform-board-hosted-interface INTERFACE
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

set(PLATFORM_BOARD_HOSTED_OPENOCD_CFG
    ${CMAKE_CURRENT_LIST_DIR}/../../board/openocd-${SCL_BOARD}.cfg)
set(STATIC_UNIFY_DIR ${STATIC_UNIFY_DIR})

add_library(platform::board-hosted ALIAS platform-board-hosted-interface)
message(VERBOSE "> platform::board-hosted -> platform-board-hosted-interface (board=${SCL_BOARD})")

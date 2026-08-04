# Platform library for the QEMU emulated NUCLEO-F411RE (xpack-style INTERFACE
# target): everything a firmware test app needs beyond the SCL itself —
# startup, board init, semihosting console + file byte_source, libc shim, C++
# runtime glue, the app-side dlopen front-end, the scl_log bridge, linker
# scripts, and the firmware link recipe.
#
# This platform is byte-identical to platforms/nucleo-f411re except:
#   * src/board.c does not poll RCC ready bits (QEMU's STM32F4 model never sets
#     them) and reports LED state over semihosting so the harness can verify it;
#   * ldscripts/mem-flatfs.ld + sections-flatfs.ld map the flat_fs "flash"
#     region to CCM SRAM (the emulated flash is read-only ROM);
#   * the firmware is executed by qemu-system-arm -machine netduinoplus2, not
#     flashed via OpenOCD (see test/tools/qemu-run.sh).
#
# INTERFACE linkage means each consumer executable compiles these sources
# itself (the platform code is part of the application, per the SCL design
# rules; the SCL libraries never contain board code).
message(VERBOSE "Including platforms/qemu-f411re/platform-library.cmake...")

set(PLATFORM_QEMU_F411RE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(PLATFORM_QEMU_F411RE_DIR ${PLATFORM_QEMU_F411RE_DIR} ABSOLUTE)

# The runner + QEMU machine are set BEFORE the guard below so every sibling
# test directory that includes this file sees them (set() calls after the
# return() would only be visible to the first includer).
set(PLATFORM_QEMU_F411RE_RUNNER ${PLATFORM_QEMU_F411RE_DIR}/../../../test/tools/qemu-run.sh)
set(PLATFORM_QEMU_F411RE_QEMU_MACHINE netduinoplus2)

if(TARGET platform-qemu-f411re-interface)
    return()   # already defined
endif()
add_library(platform-qemu-f411re-interface INTERFACE EXCLUDE_FROM_ALL)

file(GLOB _platform_src CONFIGURE_DEPENDS
     ${PLATFORM_QEMU_F411RE_DIR}/src/*.c
     ${PLATFORM_QEMU_F411RE_DIR}/src/*.cpp
     # board-independent glue shared with the other platforms
     ${PLATFORM_QEMU_F411RE_DIR}/../common/src/*.cpp)
target_sources(platform-qemu-f411re-interface INTERFACE ${_platform_src})
target_include_directories(platform-qemu-f411re-interface INTERFACE
    ${PLATFORM_QEMU_F411RE_DIR}/include
    # single-copy fixture headers (fm-iface.h, cross-error.hpp)
    ${PLATFORM_QEMU_F411RE_DIR}/../../fixtures-src/include
    ${PLATFORM_QEMU_F411RE_DIR}/../../fixtures-src/libcppeh/include
    # SCL firmware-level headers the app mains use unprefixed
    # (pic-trampoline.h, image-memory.hpp, ...)
    ${PLATFORM_QEMU_F411RE_DIR}/../../../include/scl)

# Firmware link recipe (verified in the original Makefile builds):
#  -pie/-nostdlib with the platform linker scripts; -Wl,-E re-exports main's
#  dynamic symbols so loaded libraries resolve against it; -Bsymbolic binds
#  main's own references at link time (no dynamic linker on the target).
# The linker script itself is chosen per app (sections.ld / sections-overlay.ld)
# via the LDSCRIPT argument of scl_test_app.
target_link_options(platform-qemu-f411re-interface INTERFACE
    -pie -nostdlib -nostartfiles
    LINKER:-Bsymbolic LINKER:--unresolved-symbols=ignore-all
    LINKER:-E LINKER:--gc-sections
    -L${PLATFORM_QEMU_F411RE_DIR}/ldscripts)

set(PLATFORM_QEMU_F411RE_LDSCRIPTS ${PLATFORM_QEMU_F411RE_DIR}/ldscripts)

add_library(platform::qemu-f411re ALIAS platform-qemu-f411re-interface)
message(VERBOSE "> platform::qemu-f411re -> platform-qemu-f411re-interface")

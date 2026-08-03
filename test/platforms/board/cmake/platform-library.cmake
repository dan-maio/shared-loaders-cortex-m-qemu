# Unified freestanding platform library for the SCL firmware test apps.
#
# One INTERFACE target serves BOTH supported boards; the active board is chosen
# with -DSCL_BOARD=nucleo|weact (default weact). Only board boot/init differs
# between the two (clock source/speed, LED pin, and the WEACT-only USART1/SPI1);
# every other platform source (startup/semihost/libc-shim/ldscripts) is shared.
#
# The NUCLEO-F411RE is flashed over its ST-LINK, the WEACT-F411CE over its
# CMSIS-DAP probe; the matching openocd-<board>.cfg is selected here too.
message(VERBOSE "Including platforms/board/platform-library.cmake...")

set(PLATFORM_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(PLATFORM_BOARD_DIR ${PLATFORM_BOARD_DIR} ABSOLUTE)

# --- board selection ---------------------------------------------------------
set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
set_property(CACHE SCL_BOARD PROPERTY STRINGS nucleo weact)
if(NOT SCL_BOARD STREQUAL "nucleo" AND NOT SCL_BOARD STREQUAL "weact")
    message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
endif()

if(TARGET platform-board-interface)
    return()   # already defined
endif()
add_library(platform-board-interface INTERFACE EXCLUDE_FROM_ALL)

file(GLOB _platform_src CONFIGURE_DEPENDS
     ${PLATFORM_BOARD_DIR}/src/*.c
     ${PLATFORM_BOARD_DIR}/src/*.cpp
     # board-independent glue shared with the hosted platform
     ${PLATFORM_BOARD_DIR}/../common/src/*.cpp)
target_sources(platform-board-interface INTERFACE ${_platform_src})
target_include_directories(platform-board-interface INTERFACE
    ${PLATFORM_BOARD_DIR}/include
    # single-copy fixture headers (fm-iface.h, cross-error.hpp)
    ${PLATFORM_BOARD_DIR}/../../fixtures-src/include
    ${PLATFORM_BOARD_DIR}/../../fixtures-src/libcppeh/include
    # SCL firmware-level headers the app mains use unprefixed
    # (pic-trampoline.h, image-memory.hpp, ...)
    ${PLATFORM_BOARD_DIR}/../../../include/scl)

# Board boot/init compile-time switch + HSE value for the hosted parts that
# read it (the freestanding board.c carries its own constants).
if(SCL_BOARD STREQUAL "nucleo")
    target_compile_definitions(platform-board-interface INTERFACE
        SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
else()
    target_compile_definitions(platform-board-interface INTERFACE
        SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
endif()

# Firmware link recipe (verified in the original Makefile builds):
#  -pie/-nostdlib with the platform linker scripts; -Wl,-E re-exports main's
#  dynamic symbols so loaded libraries resolve against it; -Bsymbolic binds
#  main's own references at link time (no dynamic linker on the target).
# The linker script itself is chosen per app (sections.ld / sections-overlay.ld)
# via the LDSCRIPT argument of scl_test_app.
target_link_options(platform-board-interface INTERFACE
    -pie -nostdlib -nostartfiles
    LINKER:-Bsymbolic LINKER:--unresolved-symbols=ignore-all
    LINKER:-E LINKER:--gc-sections
    -L${PLATFORM_BOARD_DIR}/ldscripts)

set(PLATFORM_BOARD_LDSCRIPTS ${PLATFORM_BOARD_DIR}/ldscripts)
set(PLATFORM_BOARD_OPENOCD_CFG ${PLATFORM_BOARD_DIR}/openocd-${SCL_BOARD}.cfg)

add_library(platform::board ALIAS platform-board-interface)
message(VERBOSE "> platform::board -> platform-board-interface (board=${SCL_BOARD})")

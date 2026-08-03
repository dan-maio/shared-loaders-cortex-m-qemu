# scl_test_app(<name> ...) — define one Cortex-M4 firmware test application:
# the testN's src/* linked against the SCL libraries and the platform interface
# library (which contributes startup/board/semihosting sources and the firmware
# link recipe), converted to .hex/.bin, and registered as a ctest entry that
# flashes and runs it on the board through OpenOCD.
#
#   scl_test_app(scl-fw-dyn1
#       LIBS scl::dynamic-nolib       # SCL targets to link
#       LDSCRIPT sections.ld          # from the platform's ldscripts/
#       ANCHORS trace_event fm_var    # extra -Wl,-u keep-alive anchors
#       LINK_LIBM)                    # append libm.a (hard-float battery libs)
#
# Every app gets the C++ EH runtime (libsupc++/libgcc) and its anchors, as in
# the original build: main re-exports __cxa_*/__gxx_personality_v0 via -Wl,-E
# so loaded libraries resolve exceptions/RTTI against it.
#
# The target board is selected with -DSCL_BOARD=nucleo|weact (default weact);
# it selects the unified platform library (test/platforms/board/), which in turn
# sets the SCL_BOARD_* compile definitions, HSE_VALUE and the OpenOCD flash
# config (ST-LINK for NUCLEO, CMSIS-DAP for WEACT).

set(SCL_TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(SCL_TEST_DIR ${SCL_TEST_DIR} ABSOLUTE)
set(SCL_ROOT ${SCL_TEST_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)

# Unified freestanding platform serves both boards (nucleo-f411re / weact-f411ce).
set(SCL_PLATFORM "board" CACHE STRING "Unified platform (do not change; board is SCL_BOARD)")
include(${SCL_TEST_DIR}/platforms/board/cmake/platform-library.cmake)

# EH runtime archives (paths resolved through the cross compiler). The MCU
# flags select the correct hard-float multilib variant; split the flag string
# into a list or the compiler sees it as one bogus argument.
separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libm.a
                OUTPUT_VARIABLE SCL_LIBM OUTPUT_STRIP_TRAILING_WHITESPACE)

function(scl_test_app name)
    cmake_parse_arguments(A "LINK_LIBM" "LDSCRIPT" "LIBS;ANCHORS;SOURCES;INCLUDE_DIRS" ${ARGN})
    if(NOT A_LDSCRIPT)
        set(A_LDSCRIPT sections.ld)
    endif()

    # Local src/ plus optional SOURCES referenced IN PLACE (e.g. a reference
    # app's storage backend) -- test apps never copy application code.
    file(GLOB _src CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
    add_executable(${name} ${_src} ${A_SOURCES})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${A_INCLUDE_DIRS}
        ${SCL_ROOT}/include/scl)          # lets the original mains keep "pic-trampoline.h"
    target_link_libraries(${name} PRIVATE platform::board ${A_LIBS})
    # Fixtures (prebuilt ARM .so) are opened over semihosting relative to the
    # OpenOCD CWD; an absolute path makes the launch directory irrelevant.
    target_compile_definitions(${name} PRIVATE DL_LIB_DIR="${SCL_TEST_DIR}/fixtures")

    # C++ EH anchors: keep the runtime alive through --gc-sections (main never
    # references it statically; loaded libraries resolve it via -Wl,-E).
    set(_anchors __cxa_throw __cxa_begin_catch __cxa_end_catch __cxa_end_cleanup
                 __cxa_allocate_exception __gxx_personality_v0 __dynamic_cast
                 ${A_ANCHORS})
    set(_uflags "")
    foreach(a IN LISTS _anchors)
        list(APPEND _uflags "LINKER:-u,${a}")
    endforeach()

    target_link_options(${name} PRIVATE
        -T${PLATFORM_BOARD_LDSCRIPTS}/${A_LDSCRIPT}
        ${_uflags})
    target_link_libraries(${name} PRIVATE ${SCL_LIBSUPCXX} ${SCL_LIBGCC})
    if(A_LINK_LIBM)
        target_link_libraries(${name} PRIVATE ${SCL_LIBM})
    endif()

    # Flash the .hex, not the .elf: -pie ELFs carry a PIE header page OpenOCD
    # cannot place in a flash bank (it would silently skip .isr_vector/.text).
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${name}> $<TARGET_FILE:${name}>.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> $<TARGET_FILE:${name}>.bin)

    # On-target run: flash + semihosting console via OpenOCD (needs the board).
    add_test(NAME ${name}
             COMMAND ${SCL_TEST_DIR}/tools/flash-run.sh $<TARGET_FILE:${name}>.hex
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
endfunction()

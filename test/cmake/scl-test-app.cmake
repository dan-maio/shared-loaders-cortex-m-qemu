# scl_test_app(<name> ...) — define one Cortex-M4 firmware test application:
# the testN's src/* linked against the SCL libraries and the platform interface
# library (which contributes startup/board/semihosting sources and the firmware
# link recipe), converted to .hex/.bin, and registered as a ctest entry that
# flashes and runs it on the board through OpenOCD — or, for the qemu-f411re /
# qemu-f411re-hosted platforms, executes it under qemu-system-arm with
# semihosting (+ optional semihosting command line for .so delivery).
#
#   scl_test_app(scl-fw-dyn1
#       LIBS scl::dynamic-nolib       # SCL targets to link
#       LDSCRIPT sections.ld          # from the platform's ldscripts/
#       ANCHORS trace_event fm_var    # extra -Wl,-u keep-alive anchors
#       LINK_LIBM                     # append libm.a (hard-float battery libs)
#       RUN_ARGS c1.so c2.so          # semihosting_cmdline args (QEMU only)
#       DONE "battery: done"          # completion marker the runner waits for
#       NEED_LED 1)                   # require >= 1 LED toggle (QEMU only)
#
# Every app gets the C++ EH runtime (libsupc++/libgcc) and its anchors, as in
# the original build: main re-exports __cxa_*/__gxx_personality_v0 via -Wl,-E
# so loaded libraries resolve exceptions/RTTI against it.
#
# The target platform is selected with -DSCL_PLATFORM=<name>:
#   board          — unified nucleo/weact platform (default; SCL_BOARD selects
#                    the specific board and OpenOCD flash config)
#   qemu-f411re    — QEMU emulated STM32F411RE (netduinoplus2 machine); the
#                    firmware is booted under qemu-system-arm, not flashed.

set(SCL_TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(SCL_TEST_DIR ${SCL_TEST_DIR} ABSOLUTE)
set(SCL_ROOT ${SCL_TEST_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)

set(SCL_PLATFORM "board" CACHE STRING
    "Target platform (board for nucleo/weact hardware, or qemu-f411re for qemu-system-arm)")
include(${SCL_TEST_DIR}/platforms/${SCL_PLATFORM}/cmake/platform-library.cmake)

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
    cmake_parse_arguments(A "LINK_LIBM" "LDSCRIPT;DONE;NEED_LED;NEED_FLASH"
                          "LIBS;ANCHORS;SOURCES;INCLUDE_DIRS;RUN_ARGS" ${ARGN})
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
    target_link_libraries(${name} PRIVATE platform::${SCL_PLATFORM} ${A_LIBS})
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

    # Resolve the platform's LDSCRIPTS directory. For the "board" platform the
    # variable is PLATFORM_BOARD_LDSCRIPTS; for "qemu-f411re" it is
    # PLATFORM_QEMU_F411RE_LDSCRIPTS (set in the platform's platform-library.cmake).
    string(TOUPPER "${SCL_PLATFORM}" _SCL_PLATFORM_UPPER)
    string(REPLACE "-" "_" _SCL_PLATFORM_VAR "${_SCL_PLATFORM_UPPER}")
    set(_ldscripts_var PLATFORM_${_SCL_PLATFORM_VAR}_LDSCRIPTS)
    set(_ldscripts_dir "${${_ldscripts_var}}")
    if(NOT _ldscripts_dir)
        # Fallback for the "board" platform which uses a different variable name
        set(_ldscripts_dir "${PLATFORM_BOARD_LDSCRIPTS}")
    endif()

    target_link_options(${name} PRIVATE
        -T${_ldscripts_dir}/${A_LDSCRIPT}
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

    # Choose the run backend: the platform library may point at a QEMU runner
    # (PLATFORM_<VAR>_RUNNER); otherwise fall back to the OpenOCD flash runner.
    set(_runner_var PLATFORM_${_SCL_PLATFORM_VAR}_RUNNER)
    set(_runner "${${_runner_var}}")
    if(NOT _runner)
        set(_runner ${SCL_TEST_DIR}/tools/flash-run.sh)
    endif()

    if(_runner MATCHES "qemu-run[^/]*\.sh$")
        # QEMU backend: boot the .elf directly under qemu-system-arm with
        # semihosting. The semihosting command line (SYS_GET_CMDLINE) is
        # delivered via --arg (the .so list to load / provision).
        set(_machine_var PLATFORM_${_SCL_PLATFORM_VAR}_QEMU_MACHINE)
        set(_machine "${${_machine_var}}")
        if(NOT _machine)
            set(_machine netduinoplus2)
        endif()
        set(_test_cmd ${_runner}
            --machine ${_machine})
        if(A_DONE)
            list(APPEND _test_cmd --done "${A_DONE}")
        endif()
        if(A_NEED_LED)
            list(APPEND _test_cmd --need-led ${A_NEED_LED})
        endif()
        if(A_NEED_FLASH)
            list(APPEND _test_cmd --need-flash ${A_NEED_FLASH})
        endif()
        foreach(a IN LISTS A_RUN_ARGS)
            list(APPEND _test_cmd --arg ${a})
        endforeach()
        list(APPEND _test_cmd $<TARGET_FILE:${name}>)
    else()
        set(_test_cmd ${_runner} $<TARGET_FILE:${name}>.hex)
    endif()

    # Run: flash + semihosting console via OpenOCD, or execute under QEMU.
    add_test(NAME ${name}
             COMMAND ${_test_cmd}
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
endfunction()

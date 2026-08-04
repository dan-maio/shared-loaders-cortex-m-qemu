cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    project(scl-fw-xip-interrupt1 LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

get_filename_component(_repo ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(_test3 ${_repo}/test-dynamic/test-dynamic-nolib/test3)
set(SCL_TEST_DIR ${_repo}/test)

# Board boot/init switch + HSE value (see test/platforms/board/include/board.h).
set(SCL_PLATFORM "board" CACHE STRING "Target platform (board for nucleo/weact, or qemu-f411re for QEMU)")
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(XIP_BOARD_DEFS "")
else()
    set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
    if(SCL_BOARD STREQUAL "nucleo")
        set(XIP_BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
    elseif(SCL_BOARD STREQUAL "weact")
        set(XIP_BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
    else()
        message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
    endif()
endif()


# Define the custom platform target that includes CMSIS, ST HAL, diag/trace, and LEDs (omitting default timer.cpp)
set(_su ${_repo}/test/platforms/board-hosted/vendor)

set(PLATFORM_SOURCES
    ${_repo}/test-xip/common/src/semihost-file.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    ${_su}/common/firmware/src/led.cpp
    ${_su}/common/firmware/src/initialize-hardware.c
    ${_su}/common/firmware/src/stm32f4xx_hal_msp.c
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

add_library(platform-board-hosted-xip-interrupt INTERFACE)
target_sources(platform-board-hosted-xip-interrupt INTERFACE ${PLATFORM_SOURCES})

# Compile platform sources with -fno-pic to allow non-GOT startup
set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")

target_include_directories(platform-board-hosted-xip-interrupt INTERFACE
    ${_repo}/test/platforms/board/include
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include
)
target_compile_definitions(platform-board-hosted-xip-interrupt INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER ${XIP_BOARD_DEFS}
    HARD_FLOAT
    OS_INCLUDE_STARTUP_GUARD_CHECKS=0
)

target_compile_options(platform-board-hosted-xip-interrupt INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-include;${_repo}/test-xip/common/include/abort_fix.h>
)
target_link_libraries(platform-board-hosted-xip-interrupt INTERFACE stdc++ m)

# Resolve compiler libraries (libsupc++.a, etc.)
separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

add_executable(scl-fw-xip-interrupt1
    src/main.cpp
    ${_repo}/test-xip/test-xip-dynamic-nolib/test1/src/test_c1_c2_cpp3.cpp
    ${_repo}/test-xip/test-xip-dynamic-nolib/test1/src/test_ehx_cons_ppeh_ppsh_prov.cpp
    ${_repo}/test-xip/test-xip-dynamic-nolib/test1/src/walkthrough.cpp
    ${_repo}/test-xip/test-xip-dynamic-nolib/test1/src/walkthrough_complex.cpp
    ${_test3}/src/fs-archive.cpp
    ${_test3}/src/flash-prog.c
)
target_include_directories(scl-fw-xip-interrupt1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${_repo}/test-xip/test-xip-dynamic-nolib/test1/include
    ${_test3}/include
    ${SCL_ROOT}/include/scl
    ${_repo}/include
)
target_link_libraries(scl-fw-xip-interrupt1 PRIVATE
    platform-board-hosted-xip-interrupt
    scl::xip-dynamic-nolib
    ${SCL_LIBSUPCXX}
    ${SCL_LIBGCC}
)
target_compile_definitions(scl-fw-xip-interrupt1 PRIVATE
    DL_LIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures"
    XIP_NS_ALIAS=nolib
)

# Copy standard PIC fixtures from test/fixtures to test-xip/fixtures so the
# provision step can read them via semihosting from the same fixtures dir.
file(COPY
     ${_repo}/test/fixtures/c1.so
     ${_repo}/test/fixtures/c2.so
     ${_repo}/test/fixtures/cpp3.so
     ${_repo}/test/fixtures/libprov.so
     ${_repo}/test/fixtures/libcons.so
     ${_repo}/test/fixtures/libcppeh.so
     ${_repo}/test/fixtures/libcppsh.so
     ${_repo}/test/fixtures/ehx_catch.so
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures)

# Custom link options using the hosted flatfs linker script
target_link_options(scl-fw-xip-interrupt1 PRIVATE
    -pie
    -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic
    LINKER:-E
    -T${_repo}/test-xip/test-xip-dynamic-nolib/test1/src/sections-flatfs-hosted.ld
    LINKER:--gc-sections
    LINKER:-Map=$<TARGET_FILE:scl-fw-xip-interrupt1>.map
)

# Post build: generate bin and hex
add_custom_command(TARGET scl-fw-xip-interrupt1 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:scl-fw-xip-interrupt1> $<TARGET_FILE:scl-fw-xip-interrupt1>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-xip-interrupt1> $<TARGET_FILE:scl-fw-xip-interrupt1>.bin
)

# Test run
add_test(NAME scl-fw-xip-interrupt1
         COMMAND ${SCL_TEST_DIR}/tools/flash-run.sh $<TARGET_FILE:scl-fw-xip-interrupt1>.hex
         WORKING_DIRECTORY ${SCL_TEST_DIR}/..)

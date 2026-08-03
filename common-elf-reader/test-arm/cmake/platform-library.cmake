# Platform library for the elf-reader test-arm build (WeAct STM32F411CE @ 100 MHz).
# All platform sources live IN this directory — self-contained, no external refs.
message(VERBOSE "Including test-arm/platform-library.cmake...")

set(_PLATFORM_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(_PLATFORM_DIR ${_PLATFORM_DIR} ABSOLUTE)

if(TARGET platform-test-arm-interface)
    return()
endif()
add_library(platform-test-arm-interface INTERFACE EXCLUDE_FROM_ALL)

target_sources(platform-test-arm-interface INTERFACE
    ${_PLATFORM_DIR}/src/startup.c
    ${_PLATFORM_DIR}/src/board.c
    ${_PLATFORM_DIR}/src/semihost.c
    ${_PLATFORM_DIR}/src/libc-shim.c
    ${_PLATFORM_DIR}/src/printf-shim.c
)
target_include_directories(platform-test-arm-interface INTERFACE
    ${_PLATFORM_DIR}/include
)
target_compile_definitions(platform-test-arm-interface INTERFACE
    HSE_VALUE=25000000
)
target_compile_options(platform-test-arm-interface INTERFACE
    -fno-tree-loop-distribute-patterns
)
target_link_options(platform-test-arm-interface INTERFACE
    -nostdlib -nostartfiles
    -L${_PLATFORM_DIR}/ldscripts
    -Tsections.ld
    -Wl,--gc-sections
)
add_library(platform::test-arm ALIAS platform-test-arm-interface)

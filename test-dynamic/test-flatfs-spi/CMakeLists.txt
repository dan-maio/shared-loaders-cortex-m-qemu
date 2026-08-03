cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    project(scl-fw-flatfs-spi LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../test/cmake/scl-test-app.cmake)
endif()

get_filename_component(_test3 ${CMAKE_CURRENT_SOURCE_DIR}/../test-dynamic-nolib/test3 ABSOLUTE)
get_filename_component(_su ${CMAKE_CURRENT_SOURCE_DIR}/../../test/platforms/board-hosted/vendor ABSOLUTE)

scl_test_app(scl-fw-flatfs-spi
    LIBS scl::dynamic-nolib
    LDSCRIPT sections.ld
    SOURCES
        ${_test3}/src/main-tests.cpp
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_spi.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
        ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr_ex.c
    INCLUDE_DIRS
        ${_test3}/include
        ${_su}/common/firmware/include
        ${_su}/common/system/include/cmsis
        ${_su}/common/system/include/stm32f4-hal
        ${_su}/common/system/include
    ANCHORS printf puts sinf cosf logf sqrtf
            trace_event trace_u32 fm_callback fm_var fm_info
    LINK_LIBM)

# SPI-flash app: WEACT-only (the NUCLEO-F411RE has no SPI wiring). The board
# platform (platform::board) already provides HSE_VALUE + SCL_BOARD_*; the HAL
# SPI driver is pulled in from the hosted vendor tree.
target_compile_definitions(scl-fw-flatfs-spi PRIVATE
    STM32F411xE USE_HAL_DRIVER)

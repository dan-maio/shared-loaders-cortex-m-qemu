# test-static — the static-patcher loading technique (scl::static).
#
# Symbols are resolved once at load and the GOT patched in place; there is no
# runtime dlopen/dlsym service. Each app is Cortex-M4 firmware.
#
# These four configure standalone rather than from here, because they carry
# their own platform trees and toolchain files:
#
#   test-static         demo firmware, two .so over semihosting
#   test-static-cpp     same, C++ fixtures
#   test-static-direct  direct API calls (main's GOT patched at load)
#   test-drv            real drivers (spi/uart/led), SPI flash + flat-fs
#
# e.g.  cmake -S test-static/test-drv -B build \
#             -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

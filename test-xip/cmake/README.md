# test-xip/cmake — reused from ../test/cmake

This tree does not duplicate the toolchain files or the app helper. The top
`test-xip/CMakeLists.txt` includes `../test/cmake/scl-test-app.cmake` and the
same `cmake/arm-none-eabi.cmake` toolchain used by `test/`. Add XIP-only cmake
modules here if the prototype needs them.

# XIP + static loader, resolved calls (no direct API).
#
# Both scenarios use the same loader configuration — xip::stat, symbols looked up
# by name at run time. They differ only in where the modules come from and what
# they are, which is why the scenario is the directory name and not a qualifier
# on the configuration above it (see doc/test-taxonomy.md):
#
#   semihosting  synthetic fixtures (slib/mylib1/mylib2/c1/c2/cpp3), provisioned
#                straight into the internal flat-fs over semihosting.
#
#   spi-flash    real modules (spi/uart/led/cpp_features). spi.so is loaded
#                first and then used to reach the SPI flash holding the others,
#                which are copied back into the internal flat-fs before
#                executing in place.
#
# Both execute from *internal* flash — the SPI flash is a staging hop, not where
# code runs.
#
# Only semihosting joins this aggregate. spi-flash builds standalone (like
# test-drv): it is a hosted app with its own platform flags and does not compile
# under the freestanding options this tree applies. Build it with its own
# build-and-run.sh, or `test-xip/build-and-run.sh spi-flash`.
add_subdirectory(semihosting)

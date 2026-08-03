# test-xip-static/spi-flash — SPI Flash Storage + Internal Flash XIP

## Overview

This test demonstrates a complete module loading pipeline using **SPI flash as transit storage** and **internal flash for XIP execution**. All drivers (SPI, UART, LED) are loaded as runtime `.so` modules, not compiled into the firmware.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Boot Flow                                │
├─────────────────────────────────────────────────────────────────┤
│ 1. Semihosting → Internal flash (provision spi.so)             │
│ 2. Load spi.so via xip::stat (copy-whole fallback)             │
│ 3. Call spi_flash_init() to initialize SPI peripheral          │
│ 4. Semihosting → SPI flash (provision uart.so, led.so, etc.)  │
│ 5. SPI flash → Internal flash (copy via flatfs API)            │
│ 6. Internal flash → XIP execution (xip::stat + call_sym*)     │
└─────────────────────────────────────────────────────────────────┘
```

## Key Features

- **Drivers as .so modules**: SPI, UART, LED are loaded at runtime, not compiled in
- **SPI flash as transit storage**: Only used for flatfs operations (format, add, find, read)
- **XIP from internal flash**: Text stays in flash, only data copied to RAM
- **Copy-whole fallback**: Non-XIP modules (like spi.so) load into RAM automatically
- **Function pointer resolution**: SPI functions resolved from loaded spi.so module

## Build Options

### XIP-Compatible Fixtures

The test-drv fixtures (spi.so, uart.so, led.so, cpp_features.so) must be rebuilt with XIP-compatible flags:

```bash
# Build XIP-compatible fixtures
./test-xip/test-xip-static/spi-flash/fixtures-src/build-xip-drv.sh
```

This uses:
- `-fpic -msingle-pic-base -mpic-register=r10` (single PIC base register)
- `-T xip-split.ld` (split R-X | RW segments)

### Standard Build

```bash
# Configure and build
cmake -S test-xip -B test-xip/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build test-xip/build --target scl-fw-xip-static-spi-flash -j

# Flash and run
./test-xip/build-and-run.sh static2
```

## Constraints

1. **XIP requires split segments**: Modules must have separate R-X and RW segments
2. **Copy-whole fallback**: Non-XIP modules load into RAM (more RAM usage)
3. **SPI flash only for storage**: Never execute code directly from SPI flash
4. **Function pointers for SPI**: SPI functions resolved at runtime via `fw_spi::fs_init()`

## File Structure

```
test-xip-static/spi-flash/
├── CMakeLists.txt
├── fixtures-src/
│   ├── build-xip-drv.sh      # Build test-drv fixtures with XIP flags
│   └── ldscripts/
│       └── xip-split.ld      # Linker script for split R-X|RW segments
├── include/
│   ├── fs-archive-spi.hpp    # SPI flash flatfs (function pointers)
│   ├── fs-archive-internal.hpp # Internal flash flatfs
│   ├── module.h              # Module class
│   └── test-helpers.hpp      # call_sym*, rd, rd_str helpers
└── src/
    ├── main.cpp              # Main test flow
    ├── xip-static-loader.cpp # XIP loader with copy-whole fallback
    ├── fs-archive-spi.cpp    # SPI flash flatfs implementation
    ├── fs-archive-internal.cpp # Internal flash flatfs implementation
    └── xip-static-loader.hpp # XIP loader header
```

## Data Flow

```
Semihosting
    ↓ (provision spi.so)
Internal flash (0x08060000)
    ↓ (load via xip::stat)
RAM (copy-whole mode)
    ↓ (call spi_flash_init())
SPI peripheral initialized
    ↓ (provision uart.so, led.so, cpp_features.so)
SPI flash (flatfs)
    ↓ (copy via spi_flash_read + fw_int::fs_add)
Internal flash (flatfs, sectors 6-7)
    ↓ (load via xip::stat, XIP mode)
Flash execution (text in flash, data in RAM)
```

## Expected Output

```
scl-fw-xip-static-spi-flash: SPI storage → internal flash XIP

========== SPI INITIALIZATION ==========
[spi] Calling spi_flash_init() from loaded spi.so...
[spi] SPI flash initialized

========== SPI FLASH PHASE ==========
[spi] Formatting SPI flash flatfs...
[spi] Provisioning from semihosting to SPI flash...
  [prov-spi] saved uart.so (1234 bytes)
  [prov-spi] saved led.so (5678 bytes)
  [prov-spi] saved cpp_features.so (9012 bytes)
[spi] SPI flash files = 3

========== INTERNAL FLASH PHASE ==========
[int] Formatting internal flash flatfs...
[int] Copying from SPI flash to internal flash...
  [copy] uart.so copied to internal flash (1234 bytes)
  [copy] led.so copied to internal flash (5678 bytes)
  [copy] cpp_features.so copied to internal flash (9012 bytes)
[int] Internal flash files = 3

========== XIP EXECUTION FROM INTERNAL FLASH ==========
[xip] ==== UART test (XIP from internal flash) ====
[xip] load: using XIP mode (text in flash, data in RAM)
  UART from internal flash XIP
  UART test PASSED

[xip] ==== LED test (XIP from internal flash) ====
[xip] load: using XIP mode (text in flash, data in RAM)
  LED test PASSED

[xip] ==== C++ features test (XIP from internal flash) ====
[xip] load: using XIP mode (text in flash, data in RAM)
  C++ OK: run0=10 run1=20 run2=30 sum=0x1234
  C++ features test PASSED

========== SPI FLASH FILES ==========
[spi] Files on SPI flash: 3
  uart.so          1234 bytes (offset 0x000100, crc32 0x12345678)
  led.so           5678 bytes (offset 0x000600, crc32 0xabcdef01)
  cpp_features.so  9012 bytes (offset 0x001000, crc32 0xdeadbeef)

========== INTERNAL FLASH FILES ==========
[int] Files on internal flash: 3
  uart.so          1234 bytes (offset 0x000100, crc32 0x12345678)
  led.so           5678 bytes (offset 0x000600, crc32 0xabcdef01)
  cpp_features.so  9012 bytes (offset 0x001000, crc32 0xdeadbeef)

========== TEST COMPLETE ==========
scl-fw-xip-static-spi-flash: done
```

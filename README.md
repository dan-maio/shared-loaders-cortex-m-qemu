# Cortex-M Shared Loaders — unified NUCLEO-F411RE / WEACT-F411CE workspace

One codebase that builds, flashes and runs the shared-library loader test
suites on **either** of two Cortex-M4F boards:

| Board             | Probe        | Clock source / speed  | LED    | UART  | SPI  |
|-------------------|--------------|-----------------------|--------|-------|------|
| **NUCLEO-F411RE** | ST-LINK/V2.1 | HSE 16 MHz → PLL 84 MHz | PA5 (LD2) | — | — |
| **WEACT-F411CE**  | CMSIS-DAP    | HSE 25 MHz → PLL 100 MHz | PC13 | USART1 | SPI1 |

Everything is local — this directory is self-contained (the ELF reader that
used to be a git submodule is vendored under `common-elf-reader/`), with **no
links into the original `shared-loaders-weact_agy` / `shared-loaders-nucleo_agy`
projects**. The two board builds share **one codebase**: every source, fixture,
linker script, and test is common. Only **board boot/init** differs (the clock
config, the LED pin, and the WEACT-only USART1/SPI1 in
`test/platforms/board/`), plus the OpenOCD flash config
(`openocd-nucleo.cfg` = ST-LINK vs `openocd-weact.cfg` = CMSIS-DAP).

## Build & run — one command, two options: `board` and `test`

```sh
./build-and-run.sh board=<nucleo|weact> test=<name> [abi=sf|hf]
# positional form:
./build-and-run.sh weact dyn2
```

- `board=` selects the target board. Only board boot/init changes between them.
- `test=` selects which firmware app to configure, build, flash and run
  (semihosting console to the terminal, Ctrl-C to stop). The test names are the
  same as before:

```
dyn1  dyn2  flatfs  gotplt-demo            dynamic-nolib  (freestanding)
newlib1-sf/hf  newlib2-sf/hf               dynamic-newlib (hosted)
ovl1                                       overlay
static  static-cpp  static-direct  drv     static-loader apps
reloc  classic  classic-spi  interrupt     ET_REL modules
xip-dynamic-nolib  xip-dynamic-newlib  xip-interrupt
xip-static/semihosting  xip-static/spi-flash  xip-static-direct
xip-interrupt-static  xip-interrupt-static-direct
```

The **SPI** apps (`flatfs-spi`, `flatfs-bitbang-spi`, `drv`, `classic-spi`,
`xip-static/spi-flash`) are **WEACT-only** — the NUCLEO-F411RE has no SPI wiring
— and `build-and-run.sh` refuses `board=nucleo` for them.

Examples:

```sh
./build-and-run.sh board=weact  test=dyn2          # freestanding on WeAct
./build-and-run.sh board=nucleo test=ovl1          # overlay on NUCLEO
./build-and-run.sh board=nucleo test=newlib2-sf abi=sf   # hosted, soft float
./build-and-run.sh board=weact  test=xip-static/spi-flash  # SPI + XIP (WeAct)
```

## Board switch internals

The board is passed to CMake as `-DSCL_BOARD=nucleo|weact` and exported as the
`SCL_BOARD` environment variable to `test/tools/flash-run.sh` (which picks the
OpenOCD cfg). The unified platform translates it into `SCL_BOARD_NUCLEO` /
`SCL_BOARD_WEACT` compile definitions plus the matching `HSE_VALUE`:

- `test/platforms/board/` — freestanding platform. `src/board.c` and
  `include/board.h` carry the per-board boot code (clock source/speed, LED pin,
  and the WEACT-gated `board_uart_*` / `board_spi_*` API). `openocd-nucleo.cfg`
  and `openocd-weact.cfg` hold the two flash configs.
- `test/platforms/board-hosted/` — hosted platform (CMSIS + ST HAL + newlib),
  vendored once under `vendor/`. Only `vendor/common/firmware/src/
  initialize-hardware.c` differs per board (PLLN/PLLP/latency), driven by the
  same `SCL_BOARD_*` switch; `HSE_VALUE` is set by the platform library.

## Manual configure/build (equivalent, no flashing)

```sh
# freestanding apps
cmake -S test-dynamic -B test-dynamic/build \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake -DSCL_BOARD=weact
cmake --build test-dynamic/build -j

# hosted newlib apps
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake -DSCL_BOARD=nucleo
cmake --build test/build-dynamic-newlib -j

# flash + run (SCL_BOARD selects the ST-LINK / CMSIS-DAP OpenOCD config)
SCL_BOARD=nucleo test/tools/flash-run.sh scl-fw-dynamic-newlib1
```

See `doc/build-and-test.md` and `doc/how-to-use.md` for the full recipes and the
expected on-target output.

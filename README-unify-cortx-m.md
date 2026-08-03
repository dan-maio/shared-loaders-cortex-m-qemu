# Cortex-M Shared Loaders — unified NUCLEO-F411RE / WEACT-F411CE workspace

One codebase that builds, flashes and runs the **shared-library loader test
suites** on either of two Cortex-M4F boards. The two boards are selected with a
single build option; **only board boot/init differs** between them.

| Board             | Probe         | Clock source / speed   | LED     | UART   | SPI  |
|-------------------|---------------|------------------------|---------|--------|------|
| **NUCLEO-F411RE** | ST-LINK/V2.1  | HSE 16 MHz → PLL 84 MHz | PA5 (LD2) | —      | —    |
| **WEACT-F411CE**  | CMSIS-DAP     | HSE 25 MHz → PLL 100 MHz| PC13   | USART1 | SPI1 |

---

## 1. What this workspace is

This directory is a **single, self-contained project** built from two projects
that were more than 95% identical:

- `shared-loaders-weact_agy` — the *reference* project (WeAct BlackPill
  STM32F411CE, CMSIS-DAP probe).
- `shared-loaders-nucleo_agy` — the *adaptation* project (NUCLEO-F411RE,
  ST-LINK probe).

Both projects load shared modules / shared libraries on STM32F411 Cortex-M4
boards (freestanding `dlopen`, hosted newlib static loader, ET_REL modules, XIP
and overlay loaders). Their SCL sources, fixtures, linker scripts and test trees
were identical; **only board boot/init differed**:

- clock source and speed (HSE 16 MHz → 84 MHz vs HSE 25 MHz → 100 MHz),
- LED pin (PA5 LD2 vs PC13),
- USART1 and SPI1 **present only on the WeAct** board,
- OpenOCD flash config (ST-LINK vs CMSIS-DAP).

Everything here is **local**. There are no links, symlinks, or git submodules
pointing at the original projects (the ELF reader that used to be a git
submodule is vendored under `common-elf-reader/`). The original two projects are
left untouched.

---

## 2. How the unification was done

The WeAct project was taken as the reference and copied here; then a handful of
steps turned the two board variants into one selectable codebase.

### 2.1 Copy, fully local

- All sources, fixtures, tools, CMake files, docs and test trees were copied
  into this workspace.
- `common-elf-reader/` (previously a git submodule of `elf32_read_parse.git`)
  was **copied in place**, removing the submodule pointer (`.gitmodules`) so the
  workspace builds without any network or sibling dependency.
- Build artifacts, `.git` metadata and per-machine state were excluded.

### 2.2 Unified freestanding platform — `test/platforms/board/`

The two freestanding platforms (`nucleo-f411re` and `weact-f411ce`) were merged
into one directory. Their sources were **identical except for the board boot
code**, so:

- `src/board.c` — a single file with `#if defined(SCL_BOARD_NUCLEO)` /
  `#else /* SCL_BOARD_WEACT */` sections that implement the two clock configs,
  the two LED pins, and gate the USART1 / SPI1 drivers behind
  `SCL_BOARD_WEACT` (the NUCLEO has neither).
- `include/board.h` — one header; the `board_uart_*` / `board_spi_*` API is
  declared only under `SCL_BOARD_WEACT`.
- `openocd-nucleo.cfg` (ST-LINK) and `openocd-weact.cfg` (CMSIS-DAP) — the two
  OpenOCD flash configs, selected by the board.
- `cmake/platform-library.cmake` — reads `SCL_BOARD`, sets the
  `SCL_BOARD_NUCLEO` / `SCL_BOARD_WEACT` compile definitions plus the matching
  `HSE_VALUE`, and exposes the `platform::board` target and the correct OpenOCD
  config path.
- `src/startup.c`, `src/semihost.c`, `src/libc-shim.c`, `src/printf-shim.c`,
  `include/dl-semihost.hpp`, `include/semihost*.{h,hpp}` and all `ldscripts/`
  were byte-identical and are shared verbatim.

### 2.3 Unified hosted platform — `test/platforms/board-hosted/`

The hosted environment (CMSIS + ST HAL + newlib, needed by the `scl::dynamic-newlib`
static loader) was also merged into one vendored tree:

- The vendor tree (`vendor/`, from the original static-unify project) is now a
  **single copy**.
- `vendor/common/firmware/src/initialize-hardware.c` — the **only** per-board
  file; the PLL configuration (PLLN/PLLP and flash latency) is switched with the
  same `SCL_BOARD_*` macro. For NUCLEO: HSE/16 · 336 / 4 = **84 MHz**; for
  WEACT: HSE/25 · 200 / 2 = **100 MHz**.
- The app-side LED (`firmware/src/main.cpp`) selects the port/pin per board
  (PA5 on NUCLEO, PC13 on WEACT).
- `cmake/platform-library.cmake` sets `HSE_VALUE` (16 MHz vs 25 MHz) and the
  board defines, and points at the matching OpenOCD config.

### 2.4 Board-aware build plumbing

- `test/cmake/scl-test-app.cmake` now includes the unified freestanding
  platform and exposes `SCL_BOARD` (default `weact`); every freestanding test
  app inherits the board switch.
- `test/tools/flash-run.sh` (and `flash-run-make.sh`) pick
  `test/platforms/board/openocd-<board>.cfg` from `SCL_BOARD`.
- The hosted test apps (`test-dynamic/test-dynamic-newlib/test1|2`), the
  `test-xip` tree and `test-relocatable` were switched to the unified
  `board` / `board-hosted` directories.
- The self-contained static patcher apps (`test-static/test-static`,
  `-static-cpp`, `-static-direct`, `-drv`) gained `SCL_BOARD` handling: their
  `HSE_VALUE`, PLL config and LED pin follow the board, and their build scripts
  select the OpenOCD config.
- New **root `build-and-run.sh`** with the two options `board=` and `test=`
  (see §4).

### 2.5 SPI tests stay WeAct-only

The SPI apps (`flatfs-spi`, `flatfs-bitbang-spi`, `drv`, `classic-spi`,
`xip-static/spi-flash`) need SPI hardware that only exists on the WeAct board.
They build normally for `board=weact`, and `build-and-run.sh` **refuses**
`board=nucleo` for them with a clear message.

### 2.6 Verification

Every test tree was cross-compiled for **both** boards with the xPack
`arm-none-eabi-gcc` 15.2.1 toolchain:

- freestanding: `dyn1`, `dyn2`, `gotplt-demo`, `flatfs`, `flatfs-spi`,
  `flatfs-bitbang-spi`, `ovl1`,
- hosted: `dynamic-newlib1`, `dynamic-newlib2` (incl. the full DYN battery),
- relocatable: `reloc`, `classic`, `classic-spi`, `interrupt`,
- XIP: `xip-nolib1`, `xip-newlib1`, `xip-static-semihosting`,
  `xip-static-direct`, `xip-interrupt1`, `xip-interrupt-static`,
  `xip-interrupt-static-direct`, `xip-static-spi-flash`,
- static patcher: `f411ce-sh`, `scl-fw-static-direct`, `scl-fw-drv`.

All targets link cleanly for both boards with the correct `SCL_BOARD_*` and
`HSE_VALUE` flags (verified in the generated `flags.make` files).

---

## 3. How it works

### 3.1 The board switch

The board is carried in **one CMake cache variable**:

```
SCL_BOARD = nucleo | weact      (default: weact)
```

It is passed to CMake as `-DSCL_BOARD=<board>` and exported as the `SCL_BOARD`
environment variable for the flash scripts. The unified platform libraries
translate it into compile definitions that reach every firmware source:

```
SCL_BOARD_NUCLEO=1  HSE_VALUE=16000000   -> NUCLEO-F411RE
SCL_BOARD_WEACT=1   HSE_VALUE=25000000   -> WEACT-F411CE
```

`board.c`, `board.h`, `initialize-hardware.c` and the test apps' `main.cpp`
branch on those macros — so the **same source files** produce the right boot
sequence for each board.

### 3.2 What differs per board (and nothing else)

| Concern              | NUCLEO-F411RE                       | WEACT-F411CE                       |
|----------------------|-------------------------------------|------------------------------------|
| HSE crystal          | 16 MHz                              | 25 MHz                             |
| PLL → system clock   | /16 · 336 / 4 = **84 MHz**          | /25 · 200 / 2 = **100 MHz**        |
| Flash wait states    | 2                                   | 3                                  |
| LED                  | PA5 (LD2)                           | PC13                               |
| USART1 (115200 8N1)  | —                                   | PA9/PA10                           |
| SPI1 (master)        | — (no SPI wiring)                   | PA4 CS, PA5 SCK, PA6 MISO, PA7 MOSI|
| OpenOCD probe        | ST-LINK/V2.1 (`openocd-nucleo.cfg`) | CMSIS-DAP (`openocd-weact.cfg`)    |

Everything else — SCL loaders, fixtures, linker scripts, test mains, semihosting
console, the linker symbol table (`gen-symtab`), XIP/overlay/reloc logic — is
**shared, one copy**.

### 3.3 Platform layout

```
test/platforms/
├── board/                unified FREESTANDING platform (both boards)
│   ├── cmake/platform-library.cmake   reads SCL_BOARD, sets defines + HSE_VALUE
│   ├── include/board.h                board API (SPI/UART gated to WEACT)
│   ├── src/board.c                    per-board clock/LED/SPI/UART
│   ├── src/{startup,semihost,libc-shim,printf-shim}.c   shared
│   ├── ldscripts/                     shared (identical for both F411s)
│   ├── openocd-nucleo.cfg             ST-LINK
│   └── openocd-weact.cfg              CMSIS-DAP
├── board-hosted/         unified HOSTED platform (CMSIS+HAL+newlib)
│   ├── cmake/platform-library.cmake
│   └── vendor/                        one vendored platform; only
│        common/firmware/src/initialize-hardware.c differs per board
└── common/               board-independent glue (crt, semihost-file, ...)
```

---

## 4. How to use

### 4.1 One command: `board` + `test`

```sh
# two options, key=value form
./build-and-run.sh board=<nucleo|weact> test=<name> [abi=sf|hf]

# positional form (board comes first)
./build-and-run.sh <board> <name> [abi=sf|hf]
./build-and-run.sh weact dyn2
./build-and-run.sh nucleo ovl1
```

`<board>` / `board=` selects the target board (`nucleo` or `weact`; only board
boot/init changes). `<name>` / `test=` picks the firmware app to configure,
build, flash and run — the same names as in the original projects. The
semihosting console streams to the terminal; **Ctrl-C** stops. Run it with no
arguments for the full test list.

The same `<board> <target>` positional form works in every per-tree
`build-and-run.sh` (`test-dynamic/`, `test-overlay/`, `test-static/`,
`test-relocatable/`, `test-xip/`); when the board is omitted they fall back to
the `SCL_BOARD` environment variable (default `weact`).

Available tests:

```
dyn1  dyn2  flatfs  gotplt-demo            dynamic-nolib  (freestanding)
newlib1-sf/hf  newlib2-sf/hf               dynamic-newlib (hosted)
ovl1                                       overlay
static  static-cpp  static-direct  drv     static-loader apps (drv = WEACT SPI)
reloc  classic  classic-spi  interrupt     ET_REL modules (classic-spi = WEACT SPI)
xip-dynamic-nolib  xip-dynamic-newlib  xip-interrupt
xip-static/semihosting  xip-static/spi-flash  xip-static-direct
xip-interrupt-static  xip-interrupt-static-direct
```

Examples:

```sh
./build-and-run.sh board=weact  test=dyn2                    # freestanding on WeAct
./build-and-run.sh board=nucleo test=ovl1                    # overlay on NUCLEO
./build-and-run.sh nucleo newlib2-sf abi=sf                  # hosted, soft float (positional)
./build-and-run.sh board=weact  test=newlib2-hf abi=hf       # hosted, hard float
./build-and-run.sh board=weact  test=xip-static/spi-flash    # SPI + XIP (WeAct only)
```

`abi=sf|hf` (or the `-sf`/`-hf` suffix in the test name) selects the float ABI
for the hosted newlib apps; `flash-run.sh` re-stages the matching fixture `.so`
into `/tmp` and verifies the ABI before flashing.

### 4.2 Manual CMake (equivalent, without flashing)

```sh
# freestanding apps
cmake -S test-dynamic -B test-dynamic/build \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake -DSCL_BOARD=weact
cmake --build test-dynamic/build -j

# hosted newlib apps
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake -DSCL_BOARD=nucleo
cmake --build test/build-dynamic-newlib -j

# flash + run (SCL_BOARD picks the ST-LINK / CMSIS-DAP OpenOCD config)
SCL_BOARD=nucleo test/tools/flash-run.sh scl-fw-dynamic-newlib1
```

### 4.3 Prerequisites

- ARM GCC toolchain: `arm-none-eabi-gcc` (the xPack 15.2.1 install is detected
  automatically by `tools-config.sh`).
- OpenOCD (`tools-config.sh` finds the xPack install).
- The board connected: NUCLEO-F411RE via its on-board **ST-LINK**, or
  WEACT-F411CE via **CMSIS-DAP**.

### 4.4 Cleaning

```sh
./cleaner.sh
```

removes every build tree in the workspace.

---

## 5. Document index

- `doc/build-and-test.md` — full build + run recipe for every module.
- `doc/how-to-use.md` — test execution guide.
- `doc/manual-scl-loaders.md` — reference manual for all loaders.
- `doc/Full-Manual-SCL.md` — the full SCL manual.
- `common-elf-reader/README.md` — the vendored ELF32 reader/parser.

This file is also available as `README-unify-cortx-m.pdf`.

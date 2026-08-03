# SCL Loader Workspace — Test Execution Guide (`how-to-use.md`)

This guide summarizes how to configure, build, flash, and run the various firmware loaders and test batteries on either of the two supported boards:
the **WEACT-F411CE** (BlackPill, CMSIS-DAP probe) or the **NUCLEO-F411RE**
(ST-LINK probe). Both boards share this single codebase; only board boot/init
and the OpenOCD flash config differ, selected with `-DSCL_BOARD=nucleo|weact`
(default `weact`).

The **one-command entry point** is the repo-root
[`build-and-run.sh`](../README.md): `./build-and-run.sh board=<nucleo|weact>
test=<name>`. The per-tree `build-and-run.sh` scripts (`test-dynamic/`,
`test-relocatable/`, `test-xip/`, …) also accept the `SCL_BOARD` environment
variable and automate the compile-flash-run cycle.

---

## Prerequisites

* **ARM GCC Toolchain**: `arm-none-eabi-gcc` must be available (the scripts automatically add the default xPack toolchain path to your `PATH` variable).
* **OpenOCD**: Must be available at `$OPENOCD` or on your `PATH`.
* **Hardware Connection**: A WEACT-F411CE board connected via CMSIS-DAP, or a NUCLEO-F411RE via its on-board ST-LINK, to your USB port.

---

## 1. Dynamic Loaders (`test/build_and_run.sh`)

Orchestrates all standard dynamic shared-library (`.so`) loader tests.

### Usage
```bash
./test/build_and_run.sh <variant-target>
```

### Available Target Options
| Target Option | Test Firmware | Environment / Float ABI | Description |
| :--- | :--- | :--- | :--- |
| **`dyn1`** | `scl-fw-dyn1` | Freestanding / Hard-Float | Basic `dlopen`/`dlsym`/`dlclose` tests. |
| **`dyn2`** | `scl-fw-dyn2` | Freestanding / Hard-Float | 16-test cross-module C/C++ battery with r10 routing, lazy binding, exceptions. |
| **`flatfs`** | `scl-fw-flatfs` | Freestanding / Hard-Float | Dynamic loading from the on-chip flat filesystem. |
| **`gotplt-demo`** | `scl-fw-gotplt-demo` | Freestanding / Hard-Float | Step-by-step GOT/PLT lazy-binding didactic walkthrough. |
| **`ovl1`** | `scl-fw-ovl1` | Freestanding / Hard-Float | Overlay-vs-dynamic memory usage comparison. |
| **`newlib1-sf`** | `scl-fw-dynamic-newlib1` | Hosted / Soft-Float | Basic load, journal reverts, and negative tests. |
| **`newlib2-sf`** | `scl-fw-dynamic-newlib2` | Hosted / Soft-Float | Full hosted dynamic suite with C++ cross-calls. |
| **`newlib1-hf`** | `scl-fw-dynamic-newlib1` | Hosted / Hard-Float | Same as `newlib1-sf` but utilizing hard-float ABI. |
| **`newlib2-hf`** | `scl-fw-dynamic-newlib2` | Hosted / Hard-Float | Same as `newlib2-sf` but utilizing hard-float ABI. |
| **`static`** | — | — | Launches the static GOT patcher cmake build & run script. |

---

## 2. Relocatable Loader (`test-relocatable/build_and_run.sh`)

Orchestrates relocatable object (`.rel.o`) loader tests.

### Usage
```bash
./test-relocatable/build_and_run.sh <variant-target>
```

### Available Target Options
| Target Option | Test Firmware | Description |
| :--- | :--- | :--- |
| **`reloc`** | `scl-fw-reloc.hex` | 9-check relocatable object test suite (relprov + relmod, cyclic C/C++). |
| **`classic`** | `scl-fw-classic-reloc.hex` | Classic relocatable demo with flatfs blob embedding. |
| **`classic-spi`** | `scl-fw-classic-spi-reloc.hex` | Relocatable loading from SPI flash storage. |
| **`interrupt`** | `scl-fw-reloc-interrupt.hex` | ET_REL modules with interrupt handler registration. |

---

## 3. Execute In Place (XIP) Loaders (`test-xip/build_and_run.sh`)

Orchestrates execute-in-place loader tests (where text segments execute directly from Flash while data resides in RAM).

### Usage
```bash
./test-xip/build_and_run.sh <variant-target>
```

### Available Target Options
| Target Option | Test Firmware | Description |
| :--- | :--- | :--- |
| **`nolib1`** | `scl-fw-xip-nolib1` | Freestanding nolib XIP loader test. Runs a final didactic walkthrough. |
| **`interrupt`** | `scl-fw-xip-interrupt1` | Freestanding nolib XIP loader with SysTick interrupt trampoline blinking PA5. |
| **`newlib1`** | `scl-fw-xip-newlib1` | Hosted newlib XIP loader (auto hard-float and hosted environment). |
| **`static`** | `scl-fw-xip-static-semihosting` | Static XIP loader (code executes from flash without dynamic `dlopen`). |
| **`static-direct`** | `scl-fw-xip-static-direct` | Direct API calls XIP (uses call_sym*, rd, rd_str helpers). |
| **`interrupt-static`** | `scl-fw-xip-interrupt-static` | XIP + TIM2 interrupt handler. |
| **`interrupt-static-direct`** | `scl-fw-xip-interrupt-static-direct` | Direct calls + interrupts. |
| **`static2`** | `scl-fw-xip-static-spi-flash` | SPI flash storage + internal flash XIP. Full pipeline: semihosting → SPI → internal flash → XIP. |

### Building XIP-Compatible Fixtures

For `static2`, you must first build XIP-compatible fixtures:

```bash
# Build XIP-compatible fixtures (spi.so, uart.so, led.so, cpp_features.so)
./test-xip/test-xip-static/spi-flash/fixtures-src/build-xip-drv.sh

# Then build and run
./test-xip/build-and-run.sh static2
```

### XIP Build Constraints

Modules must be built with XIP-compatible flags:
- `-fpic -msingle-pic-base -mpic-register=r10` (single PIC base register)
- `-T xip-split.ld` (split R-X | RW segments)
- `-Wl,-E -Wl,-Bsymbolic` (export symbols, bind symbolically)

Without these flags, modules load in **copy-whole mode** (everything in RAM).

---

## 4. Full Build-to-Flash Pipeline (Step by Step)

Every `build_and_run.sh` script follows the same end-to-end path from source code to running firmware on the board. Here is each stage in detail:

### Stage 1 — Environment Setup (`tools-config.sh`)

When you invoke `./test/build_and_run.sh dyn1`, the script first sources `tools-config.sh` from the repository root. This sets three critical environment variables:

| Variable | Purpose | Default |
|:---|:---|:---|
| `ARM_TOOLCHAIN_BIN` | Path to `arm-none-eabi-gcc` | xPack toolchain at `~/.local/xPacks/.../bin` |
| `OPENOCD` | Path to the `openocd` binary | xPack OpenOCD at `~/.local/xPacks/.../bin/openocd` |
| `OPENOCD_SCRIPTS` | OpenOCD's built-in script directory | xPack OpenOCD `.../openocd/scripts` |

The ARM toolchain bin directory is prepended to `PATH` so that `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, and `arm-none-eabi-readelf` are all available.

### Stage 2 — CMake Configure

The script runs CMake to generate the build system, pointing at the ARM cross-compilation toolchain file:

```bash
cmake -S <source-dir> -B <build-dir> \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
```

The toolchain file (`cmake/arm-none-eabi.cmake`) tells CMake to use `arm-none-eabi-gcc` as the C/C++ compiler and sets MCU-specific flags (`-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`). CMake discovers all source files, resolves dependencies, and generates Makefiles under the build directory.

For hosted newlib targets (`newlib1`, `newlib2`), a different toolchain file is used:
```bash
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake
```

### Stage 3 — CMake Build (Compilation & Linking)

```bash
cmake --build <build-dir> --target <firmware-target> -j
```

This compiles all `.c`/`.cpp` sources with `arm-none-eabi-gcc`, links them with the platform linker script, and produces:

1. **ELF file** — the debuggable firmware binary with symbol tables.
2. **HEX file** — the Intel HEX format used for flashing (e.g. `scl-fw-dyn1.hex`).
3. **Shared library fixtures** (`.so`) — test modules that will be loaded at runtime.

For dynamic loaders, the build also runs `gen-symtab` as a post-build step (see `dynamic-pre-build.md`) to embed the symbol table into the `.scl_symtab` section.

### Stage 4 — Fixture Staging (Dynamic Loaders Only)

For hosted newlib targets, the script copies the compiled `.so` fixture libraries from the build tree to `/tmp`:

```bash
cp -f "$STAGE_DIR"/*.so /tmp/
```

The firmware loads shared libraries from hardcoded `/tmp/*.so` paths via semihosting. An ABI guard verifies that the staged fixtures' float ABI (hard-float vs soft-float) matches the firmware to prevent HardFault (NOCP) exceptions.

### Stage 5 — Flash & Run (`flash-run.sh`)

The `build_and_run.sh` script delegates to `flash-run.sh`:

```bash
exec test/tools/flash-run.sh <path/to/firmware.hex>
```

`flash-run.sh` resolves the `.hex` path (either directly or by searching build trees), then launches OpenOCD:

```bash
openocd -s "$OPENOCD_SCRIPTS" \
  -f test/platforms/board/openocd-${SCL_BOARD:-weact}.cfg \
  -c "init" \
  -c "reset halt" \
  -c "program $HEX" \
  -c "arm semihosting enable" \
  -c "reset run"
```

### Stage 6 — OpenOCD Hardware Interaction

The OpenOCD configuration (`openocd.cfg`) sets up the debug connection:

| Setting | Value | Meaning |
|:---|:---|:---|
| `interface` | `cmsis-dap` | Use CMSIS-DAP debug probe |
| `vid_pid` | `0x1a86 0x8011` | USB vendor/product ID for the probe |
| `transport` | `swd` | Serial Wire Debug (2-wire protocol) |
| `target` | `stm32f4x.cfg` | STM32F4 family configuration |
| `adapter speed` | `2000` | 2 MHz SWD clock |

OpenOCD then performs these operations in sequence:

1. **`init`** — Initializes the debug adapter and connects to the target MCU via SWD.
2. **`reset halt`** — Asserts the reset line and halts the CPU before any instruction executes.
3. **`program $HEX`** — Erases the internal Flash sectors written by the HEX file, then writes the firmware bytes starting at `0x08000000` (STM32 Flash base address). Programs sector-by-sector to avoid erasing the full chip.
4. **`arm semihosting enable`** — Enables ARM semihosting so that `trace_puts()` calls in the firmware are forwarded to the host's stdout via the debug probe.
5. **`reset run`** — Releases the CPU from halt; the firmware begins executing from the reset vector.

For XIP targets, OpenOCD additionally erases the second Flash bank (`0x08040000`, 256 KB) before programming, since XIP modules execute directly from that region.

### Stage 7 — Firmware Execution & Console Capture

Once `reset run` fires, the MCU boots:

1. The Cortex-M4 vector table loads the stack pointer and reset handler address.
2. `SystemInit()` configures the clock tree (HSE 25 MHz → PLL → 100 MHz SYSCLK).
3. `main()` initializes the SCL loader, loads fixture libraries via `dlopen`/`dlsym`, and runs the test battery.
4. Every `trace_puts("...")` call triggers a semihosting BKPT (`0xAB`) that OpenOCD intercepts and prints to your terminal.

The script blocks on OpenOCD's stdout, so the test output streams live to your terminal. Press `Ctrl-C` to terminate OpenOCD and release the board.

### Visual Pipeline Summary

```
build_and_run.sh <target>
       │
       ▼
┌──────────────────────────┐
│ source tools-config.sh   │  ARM_TOOLCHAIN_BIN, OPENOCD, OPENOCD_SCRIPTS
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ cmake -S ... -B ...      │  Generate Makefiles with ARM cross-toolchain
│   -DCMAKE_TOOLCHAIN_FILE │
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ cmake --build ... -j     │  Compile C/C++ → .o → link → .elf → .hex
│   + gen-symtab post-build│  (embed symbol table into .scl_symtab)
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ cp *.so /tmp             │  Stage fixture libraries (dynamic loaders only)
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ flash-run.sh <file>.hex  │
│   └─ openocd             │
│       ├─ init            │  Connect to MCU via SWD (CMSIS-DAP)
│       ├─ reset halt      │  Freeze CPU
│       ├─ program <>.hex  │  Erase + write Flash
│       ├─ semihosting on  │  Enable trace output channel
│       └─ reset run       │  Start firmware execution
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ MCU boots → main()       │  dlopen/dlsym tests run
│ trace_puts → BKPT 0xAB  │  Semihosting → OpenOCD → your terminal
└──────────────────────────┘
```

---

## Capturing Console Output & Stopping Tests

* **Live Output**: Each script compiles the target, flashes it onto the target device, enables semihosting, and prints the firmware console trace messages directly to the terminal stdout.
* **Termination**: Since the test firmware runs indefinitely inside semihosting, press `Ctrl-C` to cancel the script and shut down the OpenOCD connection.

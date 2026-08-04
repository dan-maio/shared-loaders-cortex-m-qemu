# SCL Loader Workspace — Test Execution Guide (`how-to-use.md`)

This guide summarizes how to configure, build, flash, and run the various firmware loaders and test batteries on any of the three supported targets:

* **WEACT-F411CE** (BlackPill, CMSIS-DAP probe) — real hardware
* **NUCLEO-F411RE** (ST-LINK probe) — real hardware
* **QEMU** (emulated STM32F411RE, `-machine netduinoplus2`) — no board needed

All three share this single codebase; only board boot/init differs. The target
is selected with `board=<nucleo|weact|qemu>` (default `weact`).

The **one-command entry point** is the repo-root
[`build-and-run.sh`](../README.md): `./build-and-run.sh board=<nucleo|weact|qemu>
test=<name>`. The per-tree `build-and-run.sh` scripts (`test-dynamic/`,
`test-relocatable/`, `test-xip/`, …) also accept the `SCL_BOARD` environment
variable and automate the compile-flash-run cycle.

---

## Prerequisites

* **ARM GCC Toolchain**: `arm-none-eabi-gcc` must be available (the scripts automatically add the default xPack toolchain path to your `PATH` variable).
* **OpenOCD** (hardware only): Must be available at `$OPENOCD` or on your `PATH`. Not needed for QEMU targets.
* **QEMU** (emulation only): `qemu-system-arm` must be available at `$QEMU` or on your `PATH`. Not needed for hardware targets.
* **Hardware Connection** (hardware only): A WEACT-F411CE board connected via CMSIS-DAP, or a NUCLEO-F411RE via its on-board ST-LINK, to your USB port.

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

Every `build_and_run.sh` script follows the same end-to-end path from source code to running firmware. The pipeline differs between hardware (OpenOCD) and QEMU targets:

### Stage 1 — Environment Setup (`tools-config.sh`)

When you invoke `./build-and-run.sh board=qemu test=dyn1`, the script first sources `tools-config.sh` from the repository root. This sets the critical environment variables:

| Variable | Purpose | Default |
|:---|:---|:---|
| `ARM_TOOLCHAIN_BIN` | Path to `arm-none-eabi-gcc` | xPack toolchain at `~/.local/xPacks/.../bin` |
| `OPENOCD` | Path to the `openocd` binary (hardware only) | xPack OpenOCD at `~/.local/xPacks/.../bin/openocd` |
| `OPENOCD_SCRIPTS` | OpenOCD's built-in script directory (hardware only) | xPack OpenOCD `.../openocd/scripts` |
| `QEMU` | Path to `qemu-system-arm` (QEMU only) | `qemu-system-arm` on PATH |
| `ARM_GDB` | Path to `arm-none-eabi-gdb` (QEMU debug only) | xPack toolchain `.../bin/arm-none-eabi-gdb` |

The ARM toolchain bin directory is prepended to `PATH` so that `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, and `arm-none-eabi-readelf` are all available.

### Stage 2 — CMake Configure

The script runs CMake to generate the build system, pointing at the ARM cross-compilation toolchain file:

```bash
# Hardware target (nucleo/weact):
cmake -S <source-dir> -B <build-dir> \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DSCL_BOARD=weact

# QEMU target:
cmake -S <source-dir> -B <build-dir> \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DSCL_PLATFORM=qemu-f411re
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

### Stage 5 — Run Backend (Hardware vs QEMU)

The `build_and_run.sh` script branches based on the selected board:

**Hardware (nucleo/weact)** — delegates to `flash-run.sh`:

```bash
exec test/tools/flash-run.sh <path/to/firmware.hex>
```

`flash-run.sh` resolves the `.hex` path, then launches OpenOCD:

```bash
openocd -s "$OPENOCD_SCRIPTS" \
  -f test/platforms/board/openocd-${SCL_BOARD}.cfg \
  -c "init" \
  -c "reset halt" \
  -c "program $HEX" \
  -c "arm semihosting enable" \
  -c "reset run"
```

**QEMU** — delegates to `qemu-run.sh`:

```bash
exec test/tools/qemu-run.sh --machine netduinoplus2 \
    --done "battery: done" --need-led 1 \
    <path/to/firmware.elf>
```

`qemu-run.sh` boots the ELF directly under `qemu-system-arm` with semihosting enabled. No flashing is needed — QEMU loads the firmware into its emulated memory.

### Stage 6a — OpenOCD Hardware Interaction (nucleo/weact)

The OpenOCD configuration (`openocd-<board>.cfg`) sets up the debug connection:

| Setting | WEACT-F411CE | NUCLEO-F411RE |
|:---|:---|:---|
| `interface` | `cmsis-dap` | `st-link` |
| `transport` | `swd` | `swd` |
| `target` | `stm32f4x.cfg` | `stm32f4x.cfg` |
| `adapter speed` | 2000 kHz | 2000 kHz |

OpenOCD then performs these operations in sequence:

1. **`init`** — Initializes the debug adapter and connects to the target MCU via SWD.
2. **`reset halt`** — Asserts the reset line and halts the CPU before any instruction executes.
3. **`program $HEX`** — Erases the internal Flash sectors written by the HEX file, then writes the firmware bytes starting at `0x08000000` (STM32 Flash base address). Programs sector-by-sector to avoid erasing the full chip.
4. **`arm semihosting enable`** — Enables ARM semihosting so that `trace_puts()` calls in the firmware are forwarded to the host's stdout via the debug probe.
5. **`reset run`** — Releases the CPU from halt; the firmware begins executing from the reset vector.

For XIP targets, OpenOCD additionally erases the second Flash bank (`0x08040000`, 256 KB) before programming, since XIP modules execute directly from that region.

### Stage 6b — QEMU Emulation (qemu)

QEMU boots the firmware directly from the ELF file. The key differences from hardware:

| Aspect | Hardware (OpenOCD) | QEMU |
|:---|:---|:---|
| Firmware format | `.hex` (Intel HEX) | `.elf` (ELF with symbols) |
| Flash programming | Erase + write via SWD | Not needed (QEMU loads into emulated memory) |
| Semihosting output | Via debug probe (BKPT 0xAB) | Via QEMU's semihosting support (BKPT 0xAB) |
| Semihosting cmdline | `arm semihosting_cmdline` in OpenOCD | `--arg` flags to `qemu-run.sh` |
| LED observation | GPIO register writes | Semihosting trace (`[LED:ON]`/`[LED:OFF]`) |
| Clock configuration | HSE → PLL → 100 MHz | Stays on reset HSI (16 MHz); timing irrelevant |
| Flash filesystem | Real NOR flash sectors 6–7 | CCM SRAM (0x10000000), pre-filled 0xFF |
| Completion detection | Ctrl-C to stop OpenOCD | `--done` marker triggers automatic shutdown |
| GDB debugging | OpenOCD GDB stub | QEMU built-in GDB stub (`qemu-gdb.sh`) |

The QEMU runner (`qemu-run.sh`) provides automatic pass/fail verdicts based on:
- **Done markers** (`--done`): completion strings like `"battery: done"` or `"RESULT: ALL PASS"`
- **Fail markers**: HardFault, `FAIL`, `FAILED`, `(FAILURES) ==`
- **LED toggles** (`--need-led`): minimum number of `[LED:]` reports
- **Flash provisioning** (`--need-flash`): flat_fs file count check

### Stage 7 — Firmware Execution & Console Capture

Once the firmware starts (either via OpenOCD `reset run` or QEMU boot):

1. The Cortex-M4 vector table loads the stack pointer and reset handler address.
2. Board init configures the clock tree (hardware: HSE → PLL → 100 MHz; QEMU: stays on HSI 16 MHz).
3. `main()` initializes the SCL loader, loads fixture libraries via `dlopen`/`dlsym`, and runs the test battery.
4. Every `trace_puts("...")` call triggers a semihosting BKPT (`0xAB`) that the host intercepts and prints to your terminal.

For hardware targets, the script blocks on OpenOCD's stdout; press `Ctrl-C` to terminate. For QEMU targets, the runner automatically detects the completion marker and shuts down; the pass/fail verdict is printed.

### Visual Pipeline Summary

```
build-and-run.sh board=<nucleo|weact|qemu> test=<name>
       │
       ▼
┌──────────────────────────┐
│ source tools-config.sh   │  ARM_TOOLCHAIN_BIN, OPENOCD/QEMU, OPENOCD_SCRIPTS
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ cmake -S ... -B ...      │  Generate Makefiles with ARM cross-toolchain
│   -DCMAKE_TOOLCHAIN_FILE │  -DSCL_BOARD=weact OR -DSCL_PLATFORM=qemu-f411re
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
           ├─────────────── board=nucleo|weact ──────────────┐
           │                                                  │
           ▼                                                  ▼
┌──────────────────────────┐           ┌──────────────────────────┐
│ flash-run.sh <file>.hex  │           │ qemu-run.sh <file>.elf   │
│   └─ openocd             │           │   └─ qemu-system-arm     │
│       ├─ init            │           │       ├─ -M netduinoplus2│
│       ├─ reset halt      │           │       ├─ -semihosting    │
│       ├─ program <>.hex  │           │       ├─ -kernel <>.elf  │
│       ├─ semihosting on  │           │       └─ --done/--arg    │
│       └─ reset run       │           └──────────┬───────────────┘
└──────────┬───────────────┘                      │
           ▼                                      ▼
┌──────────────────────────────────────────────────────────┐
│ MCU boots → main()                                        │
│ trace_puts → BKPT 0xAB → host console                    │
│ Hardware: OpenOCD streams output; Ctrl-C to stop          │
│ QEMU: qemu-run.sh captures log; auto-shutdown on --done  │
└──────────────────────────────────────────────────────────┘
```

---

## Capturing Console Output & Stopping Tests

### Hardware (nucleo/weact)

There are two modes for hardware test termination:

**Interactive mode** (default): `flash-run.sh` replaces itself with OpenOCD,
which runs the firmware indefinitely. The semihosting console streams directly
to your terminal. Press **Ctrl-C** to terminate OpenOCD and release the board.

```bash
test/tools/flash-run.sh scl-fw-dyn1          # runs until Ctrl-C
```

**Timed mode** (`--timeout`): OpenOCD runs the firmware for the specified
duration, then shuts down cleanly. Useful for CI, batch runs, or when you want
the script to exit automatically.

```bash
test/tools/flash-run.sh --timeout 30000 scl-fw-dyn1   # stop after 30 seconds
RUN_MS=60000 test/tools/flash-run.sh scl-fw-dyn2       # 60s via env var
```

The XIP `build-and-run.sh` uses timed mode automatically (`RUN_MS` defaults to
300000 = 5 minutes). Override it:

```bash
RUN_MS=120000 test-xip/build-and-run.sh static/semihosting
```

**Log capture** (`--log`): tee the console to a file alongside stdout:

```bash
test/tools/flash-run.sh --timeout 30000 --log /tmp/run.log scl-fw-dyn1
```

**Stopping a stuck process**: if OpenOCD does not respond to Ctrl-C:

```bash
killall openocd
```

### QEMU

The runner (`qemu-run.sh`) captures the semihosting console to a log file and
prints it after the run completes. The runner automatically shuts down when a
`--done` marker is detected, or after a timeout (default 120s). The pass/fail
verdict is printed at the end.

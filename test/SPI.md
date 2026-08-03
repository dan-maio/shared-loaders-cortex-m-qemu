# SPI Flash Tests — WeAct STM32F411CE + W25Q64

## Overview

Two SPI flash driver implementations driving a W25Q64 (8 MB) SPI NOR flash
over the STM32F411CE's SPI1 peripheral (PA4–PA7), exercised through a shared
flat-filesystem test battery (15 cross-module dynamic-loading tests).

---

## Hardware

| Signal | Pin  | Notes                         |
|--------|------|-------------------------------|
| CS     | PA4  | GPIO output, active low       |
| SCK    | PA5  | SPI1 clock (AF5)              |
| MISO   | PA6  | SPI1 data in (AF5)            |
| MOSI   | PA7  | SPI1 data out (AF5)           |
| LED    | PC13 | Active high, on-board LED     |

HSE = 25 MHz external crystal.  W25Q64 footprint at C15 (SOIC-8).

---

## Clock Tree

```
HSE (25 MHz)
  └─ PLL: /25 × 200 / 2 = 100 MHz  ── SYSCLK ── HPRE=/1 ── HCLK (100 MHz)
                                                              ├─ PPRE1=/2 ── APB1 (50 MHz)
                                                              └─ PPRE2=/1 ── APB2 (100 MHz)
```

- SPI1 is on APB2 → `PCLK2 = 100 MHz`.
- SysTick reload = 100000 → 1 ms tick.
- Voltage regulator: Scale 1 (required for 100 MHz).
- Flash: 3 wait states, prefetch + I/D cache enabled.

---

## Driver Implementations

### 1. Bitbang Driver

**File:** `test-dynamic/test-flatfs-bitbang-spi/src/spi-flash.cpp`

Raw GPIO toggling on PA4–PA7.  No peripheral (SPI1) used.

```
xfer(byte):
  for each bit (MSB first):
    set MOSI
    raise SCK
    read MISO → shift into result
    lower SCK
  return result
```

- **Clock:** CPU-driven, no delay loops.  At 16 MHz HSI ≈ 500 kHz SCK;
  at 100 MHz PLL ≈ 3.125 MHz SCK.
- **GPIO init:** `GPIOA_MODER`, `GPIOA_PUPDR`, `GPIOA_BSRR` written directly.
- **Pros:** No HAL dependency, works at any CPU speed (within W25Q64 timing).
- **Cons:** 100 MHz PLL SCK may exceed W25Q64 hold/setup margins on some
  boards (garbled reads observed).  No timeout mechanism.

### 2. HAL Driver

**File:** `test-dynamic/test-flatfs-spi/src/w25q64.cpp`

STM32F4 HAL SPI driver (`stm32f4xx_hal_spi.c`) in polled mode.

```
SPI_HandleTypeDef spi_handle = {
    .Instance         = SPI1,
    .Init.Mode        = SPI_MODE_MASTER,
    .Init.Direction   = SPI_DIRECTION_2LINES,
    .Init.DataSize    = SPI_DATASIZE_8BIT,
    .Init.CLKPolarity = SPI_POLARITY_LOW,
    .Init.CLKPhase    = SPI_PHASE_1EDGE,
    .Init.NSS         = SPI_NSS_SOFT,
    .Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4,   // 100/4 = 25 MHz
    .Init.FirstBit    = SPI_FIRSTBIT_MSB,
};
```

- **Clock:** 100 MHz / 4 = 25 MHz SCK.
- **GPIO init:** `HAL_GPIO_Init()` for CS (PP output), SCK/MOSI (AF5), MISO (AF5).
- **Runs:** `HAL_SPI_TransmitReceive()` in polled mode (timeout 100 ms).
- **Pros:** Proper timing, error checking, interrupt/DMA ready.
- **Cons:** Requires `HAL_Init()` and correct `SystemCoreClock`.  SPI handle
  must be re-initialised after PLL clock change.

---

## Filesystem Architecture

### Flat Archive Format

```
Offset  | Content                    | Size
--------+----------------------------+-------
0       | FsSuper (magic, version…)  | 32 B
32      | FsEntry[64] (TOC)          | 2048 B
2080    | Blob data (files)          | …
```

- **FsSuper:** magic=0x46414C31, version=1, toc_off=32, toc_cap=64.
- **FsEntry:** name[16], off, size, crc32, state.
- **Layout computed at compile time** via `constexpr kBlobOff`.

### Test Flow

```
main()
  ├─ spi_flash_init()          # HAL_Init() + flash.power_up()
  ├─ fw::fs_format()           # bulk erase + write super + TOC
  ├─ provision_name() × 8      # semihost read → spi_flash_write()
  ├─ g_use_flatfs = true
  └─ battery_run()
       ├─ board_init()          # PLL 100 MHz (HSI → PLL switch)
       └─ 15 tests via dlopen_fs()
            ├─ fs_find() → FsEntry
            ├─ SpiByteSource::read_at()
            │    └─ spi_flash_read()  # bitbang or HAL
            └─ dlopen_source()        # ELF loader
```

---

## Problems & Solutions

### P1. Cross-unit PLT crash (`battery_run` undefined)

**Symptom:** `_Z11battery_runv` undefined → PLT stub → hardfault on call.

**Root cause:** `main-tests.cpp` (defines `battery_run()`) was missing from
the HAL test's `CMakeLists.txt` SOURCES list.

**Fix:** Add `${_test3}/src/main-tests.cpp` to SOURCES.

### P2. C/C++ linkage mismatch

**Symptom:** Functions defined with `extern "C"` in `main.cpp` (e.g.,
`spi_flash_sector_erase`) were called under their C++ mangled names from
`fs-archive-spi.cpp` → unresolved PLT stubs → hardfault.

**Root cause:** `spi-flash.h` had no `extern "C"` guard, so C++ includers
generated mangled references.

**Fix:** Wrap all declarations in `spi-flash.h`:
```c
#ifdef __cplusplus
extern "C" {
#endif
/* … */
#ifdef __cplusplus
}
#endif
```

### P3. `HAL_SPI_Transmit` hardfault (F4 SPI v2)

**Symptom:** `HAL_SPI_Transmit()` crashes, but `HAL_SPI_TransmitReceive()`
works for the same data.

**Root cause:** On STM32F4 (SPI v2), `HAL_SPI_Transmit()` does not drain the
RX FIFO.  In 2-line mode, each transmitted byte also receives one; if the RX
FIFO overflows, the SPI state machine stalls.  `HAL_SPI_TransmitReceive()`
reads the RX FIFO and avoids the overflow.

**Fix:** Replace all `HAL_SPI_Transmit()` calls with
`HAL_SPI_TransmitReceive()` using a dummy RX buffer:
```cpp
static void tx(uint8_t* buf, uint16_t len) {
    uint8_t dummy[32];
    while (len > 0) {
        uint16_t chunk = len > sizeof(dummy) ? sizeof(dummy) : len;
        HAL_SPI_TransmitReceive(&spi_handle, buf, dummy, chunk, 100);
        buf += chunk;
        len -= chunk;
    }
}
```

### P4. TOC write exceeds 256-byte page limit

**Symptom:** `spi_flash_page_program(g_toc, sizeof(g_toc)=2048)` silently
fails (returns -1, caller ignores return).

**Root cause:** W25Q64 page-program limit is 256 bytes.

**Fix:** Use `spi_flash_write()` (which calls `page_program` in 256-byte
chunks) for both superblock and TOC.

### P5. PLL reconfiguration while PLL running

**Symptom:** Second `board_init()` from `battery_run()` writes `PLLCFGR`
while PLL is already enabled → unpredictable behaviour / hardfault.

**Root cause:** `board_init()` unconditionally reconfigures PLL.

**Fix:** Added SWS (System Clock Switch Status) guard:
```c
if ((RCC_CFGR & (3u << 2)) == (2u << 2))  /* already on PLL */
    return;
```
(Later removed to keep bitbang compatibility; HAL test handles this by
running SPI at HSI 16 MHz before battery_run.)

### P6. SPI read corruption after clock change

**Symptom:** SPI flash reads after `board_init()` switches from HSI 16 MHz
to PLL 100 MHz return garbled data.

**Root cause:** The SPI peripheral (HAL) or bitbang GPIO timing changes
with the APB2 clock.  The SPI handle configured at 16 MHz may not operate
correctly at 100 MHz.

**HAL driver fix:** Call `spi_flash_reinit()` before each read in
`dl-fs-spi.cpp` — re-runs `flash.power_up()` which calls `HAL_SPI_Init()`
with current clock settings:
```cpp
loader::result<handle*> dlopen_fs(const char* name) noexcept {
    /* … */
    spi_flash_reinit();           // re-init SPI at current PCLK2
    SpiByteSource src(e->off, e->size);
    return dlopen_source(src, name);
}
```

**Bitbang driver:** No fix yet — bitbang at 100 MHz PLL produces SCK
~3.125 MHz with marginal hold/setup times on some boards.  Workaround:
run SPI operations before `board_init()` (at HSI 16 MHz), or add NOP
delays in the `xfer()` loop.

### P7. `HAL_Init()` not called

**Symptom:** `HAL_GetTick()` returns 0 → timeout logic misbehaves →
unpredictable hardfaults in HAL SPI functions.

**Root cause:** `HAL_Init()` was removed from `spi_flash_init()` during
refactoring.

**Fix:** Restore `HAL_Init()` call inside `spi_flash_init()`.

---

## Test Targets

| Target                        | Driver  | SPI Clock       | Status |
|-------------------------------|---------|-----------------|--------|
| `scl-fw-flatfs-bitbang-spi`   | Bitbang | CPU-driven      | OK     |
| `scl-fw-flatfs-spi`           | HAL     | 25 MHz (/4)     | OK     |

**Run:**

```bash
cd ~/TMP3/scaffold/shared-loaders
./test/build-and-run.sh flatfs-bitbang-spi
./test/build-and-run.sh flatfs-spi
```

Both tests provision 8 `.so` files to the W25Q64, then run the 15-test
cross-module battery through `dlopen_fs`.

---

## Key Source Files

| Path | Role |
|------|------|
| `test/platforms/weact-f411ce/src/board.c` | Board init: PLL, GPIO, SysTick |
| `test-dynamic/test-flatfs-bitbang-spi/src/spi-flash.cpp` | Bitbang SPI driver |
| `test-dynamic/test-flatfs-spi/src/w25q64.cpp` | HAL SPI driver + W25Q64 commands |
| `test-dynamic/test-flatfs-spi/include/spi-flash.h` | C API declarations (extern "C") |
| `test-dynamic/test-flatfs-spi/include/spi-byte-source.hpp` | SpiByteSource for dlopen |
| `test-dynamic/test-flatfs-spi/src/dl-fs-spi.cpp` | SPI-backed dlopen_fs |
| `test-dynamic/test-flatfs-spi/src/fs-archive-spi.cpp` | Flat archive format/write/read |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | 15-test battery |
| `test-dynamic/test-flatfs-spi/src/main.cpp` | HAL test main |
| `test-dynamic/test-flatfs-bitbang-spi/src/main.cpp` | Bitbang test main |

## 5. Bitbang SPI in a Loaded .so — test-drv

The `test-drv` project loads the SPI driver as a **runtime-loaded .so module**
via the `scl::static` loader, not compiled into the firmware firmware. The driver
uses the same bitbang GPIO approach as §4.1, proving that a position-independent
`.so` executing from RAM can drive the SPI flash correctly.

| Aspect | Detail |
|--------|--------|
| Location | `test-drv/fixtures/src/spi.c` |
| Technique | Bitbang GPIO (identical to §4.1) |
| Init | Module's own `spi_init()` sets GPIO mode via RCC+MODER, no HAL |
| Loader | `scl::static` — `load_so_into()` → `load_bin()` → `patch_shlib()` |
| Zero relocs | `readelf -r spi.so` → no relocations; code runs directly from RAM |

**Key difference from §4.1/4.2:** The SPI driver is NOT part of the firmware
binary. It is loaded from the host filesystem via semihosting, placed in a
dedicated RAM buffer (`g_spi_img`), and its exported symbols (`spi_init`,
`spi_write`, `spi_read`, `spi_erase_sector`, `spi_format`) are resolved by
walking `.dynsym` directly in the loaded buffer. After loading, all SPI
operations proceed identically to the compiled-in variants.

### 5.1. Direct Calls via GOT/PLT

A later refactoring replaced the runtime `.dynsym` walk with **direct calls**
through the linker-generated Global Offset Table (GOT) and Procedure Linkage
Table (PLT). The firmware calls `spi_init()`, `spi_write()`, etc. directly —
the compiler emits PLT calls, the linker creates GOT slots, and `patch_main()`
fills them with the actual addresses from the loaded `.so`.

#### Required Build Changes

| Change | Reason |
|--------|--------|
| **`extern "C"`** in headers (`spi.h`, `uart.h`, `led.h`) | Prevents C++ name mangling — the `.so` exports `spi_init` (C), not `_Z8spi_initv` (C++) |
| **`target_link_libraries` with `.so`** | `target_link_libraries(scl-fw-drv PRIVATE …/libspi.so …/libuart.so …/libled.so)` makes the linker emit `.dynsym`/`.rel.dyn`/`.plt`/`.got` in the firmware ELF |
| **`.so` renamed to `lib*.so`** | The linker's `-l` convention looks for `libspi.so`; cmake for `arm-none-eabi` filters `-l` flags, so absolute paths to `libspi.so` are used instead |
| **`Module::build()` calls `patch_main()`** | `scl::static`'s `patch_main()` iterates `.rel.dyn` and writes resolved addresses into each GOT slot |

#### Generated Sections in the Firmware ELF

| Section | Content |
|---------|---------|
| `.dynsym` | 13 dynamic symbol entries (`spi_init`, `uart_puts`, `led_blink`, …) |
| `.rel.dyn` | 12 `R_ARM_JUMP_SLOT` relocations (one per function GOT slot) |
| `.plt` | Thumb PLT stubs in FLASH (verified with `objdump -d -j .plt`) |
| `.got` | GOT slots in RAM (initialised with PLT stub addresses) |
| `.dynamic` | `DT_NEEDED` entries for `libspi.so`, `libuart.so`, `libled.so` |

#### CMake Snippet

```cmake
target_link_libraries(scl-fw-drv PRIVATE scl::static
    ${DRV_DIR}/fixtures/libspi.so
    ${DRV_DIR}/fixtures/libuart.so
    ${DRV_DIR}/fixtures/libled.so)
target_link_options(scl-fw-drv PRIVATE …)
```

#### main.cpp

```cpp
#include "spi.h"    // extern "C" void spi_init(void);

scl::Module mod;
mod.load_file("fixtures/libspi.so", g_spi_img, sizeof(g_spi_img), &g_syms);
spi_init();          // direct call → GOT → patched by patch_main()
spi_format();
```

### 5.2. Complete Build Options Reference — Plain English

#### Shared Libraries (`.so` files)

These flags go into the `add_shlib()` CMake function or are passed directly
to `arm-none-eabi-gcc` when building `libspi.so`, `libuart.so`, `libled.so`.

| Flag / Setting | Plain English |
|----------------|---------------|
| `-mcpu=cortex-m4` | Generate instructions for Cortex-M4 (FPU not used, but CPU must support Thumb-2 and DSP extensions) |
| `-mthumb` | Use Thumb-2 instruction set (Cortex-M4 cannot execute ARM-mode code) |
| `-mfloat-abi=soft` | Pass floating-point arguments in integer registers, not FPU registers. Keeps the .so compatible even if FPU is disabled |
| `-fPIC` | **Position-Independent Code.** Every global/function reference goes through the Global Offset Table (GOT) instead of using absolute addresses. This is REQUIRED — without it, the .so could only run at the address it was linked for |
| `-fno-inline-functions` | Keep every function as a separate callable entity so the symbol table has one entry per function (no inlined-away symbols) |
| `-fno-common` | Put each global variable in its own section (prevents linker from merging tentatively-defined objects) |
| `-O0` | No optimisation — makes debugging easier |
| `-g3` | Maximum debug info (macro definitions included) |
| `-Wall -Wextra` | Enable all common warnings |
| `-shared` | Produce a shared library (`.so`, ELF type `ET_DYN`), not an executable |
| `-nostartfiles` | Do not link the default C runtime startup (the .so has no `main()` — it is loaded by the firmware's loader) |
| `-T mem.ld` | Custom linker script that places everything at virtual address 0 (position-independent, since the loader copies segments to any RAM address) |
| `-Wl,--gc-sections` | Garbage-collect unused sections (smaller .so) |

**Naming:** The .so file must be called `lib<name>.so` (e.g. `libspi.so`) for
the linker's `-l<name>` convention. The CMake target builds the file as
`lib${name}.so`. The firmware links with absolute paths to these files.

#### Main Firmware

These flags are set in `CMakeLists.txt` for the `scl-fw-drv` executable target.

| Flag / Setting | Plain English |
|----------------|---------------|
| `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft` | Same CPU/instruction-set/float settings as the .so (must match) |
| `-O0 -g3 -Wall -Wextra` | Debug-friendly: no optimisation, full debug info, all warnings |
| `-ffunction-sections -fdata-sections` | Put each function/variable in its own section. Enables `--gc-sections` at link time to remove unused code |
| `-std=c++20 -fno-rtti -fno-exceptions` | C++20 without runtime type info or exceptions (reduces code size) |
| `-nostartfiles` | Do not link the default startup crt0 — the firmware provides its own `_start` via `startup.c` |
| `-Wl,--gc-sections` | Remove unused sections (applies to all .o files) |
| `--specs=nano.specs` | Use newlib-nano (lightweight C library for embedded) instead of full newlib |
| `--specs=nosys.specs` | Use stub syscalls (semihosting handles `_open`, `_read`, `_write`, etc.) |
| `-Wl,--no-warn-rwx-segments` | Suppress the "LOAD segment has RWX permissions" warning (Cortex-M has no MMU so W^X is meaningless) |
| **`.so` in `target_link_libraries`** | **The magic ingredient.** Passing `libspi.so`, `libuart.so`, `libled.so` to the linker makes it create `.dynsym`, `.rel.dyn`, `.plt`, `.got` sections in the firmware ELF. Without this, direct calls (`spi_init()`) cannot work |
| `-T mem-sh.ld -T libs-sh.ld -T sections-sh.ld` | Three linker scripts that define memory layout, library placeholder, and section placement (including `.got`/`.got.plt` in RAM, `.plt` in FLASH) |

**Defines (`-D`):**

| Define | Purpose |
|--------|---------|
| `STM32F411xE` | Select the STM32F411xC/E HAL (peripheral register map) |
| `USE_HAL_DRIVER` | Enable the STM32 HAL (Hardware Abstraction Layer) |
| `HSE_VALUE=25000000` | External crystal frequency = 25 MHz (WeAct F411CE board) |
| `OS_USE_SEMIHOSTING` | Use semihosting for `_open`/`_read`/`_write`/`_close` (files served by the debugger) |
| `DEBUG`, `TRACE` | Enable debug output and tracing |
| `OS_USE_TRACE_SEMIHOSTING_DEBUG` | Route the `trace_puts()` output through semihosting |

**Defines the firmware does NOT use:**

| Flag | Why not needed |
|------|----------------|
| `-fPIC` | The firmware is non-PIC. The compiler emits direct `BL` calls, and the linker creates PLT stubs for .so functions automatically |
| `--no-dynamic-linker` | Omitting this is **required** — the linker must emit `.dynsym`/`.rel.dyn`/`.dynamic` sections for the GOT/PLT mechanism to work |
| `-rdynamic` | Not supported by `arm-none-eabi-ld`; also not needed because the .so paths in `target_link_libraries` already trigger dynamic section generation |
| `-Wl,-E` (export-dynamic) | Only needed on the .so side when the .so's own internal symbols must be visible; the test fixtures have zero relocations so it is unnecessary |

#### Preprocessor Effects on Linkage

```
main.cpp (C++)                libspi.so (C)
─────────────                 ─────────────
#include "spi.h"              #include "spi.h"
// extern "C" void spi_init() → C mangled name
                                .dynsym exports "spi_init" (C)
↓ compiler generates BL spi_init
↓ linker sees "spi_init" is in libspi.so
↓ creates PLT stub + GOT slot + .rel.dyn entry
↓ Module::load_file() calls patch_main()
↓ patch_main() reads .rel.dyn, looks up spi_init
  in libspi.so's .dynsym, writes runtime address
  into GOT slot
↓ calling spi_init() → PLT → GOT → actual function
```

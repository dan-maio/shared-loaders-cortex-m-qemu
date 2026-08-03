# Direct Calls via GOT/PLT — Build & Usage Guide

## How It Works

The firmware calls `.so` functions directly — `spi_init()`, `uart_puts("hello")` —
without any runtime symbol lookup. The linker generates **PLT stubs** in FLASH,
**GOT slots** in RAM, and `.rel.dyn` entries. The `scl::static` loader's
`patch_main()` fills each GOT slot with the function's actual address from the
loaded `.so`. After that, every PLT call jumps straight to the right address.

## Required Build Steps

### 1. `extern "C"` in all shared headers

Files: `fixtures/include/spi.h`, `uart.h`, `led.h`

Without `extern "C"`, C++ mangles `spi_init` to `_Z8spi_initv`. The `.so` exports
`spi_init` (plain C). The linker never matches them.

```c
#ifdef __cplusplus
extern "C" {
#endif

void spi_init(void);

#ifdef __cplusplus
}
#endif
```

### 2. Link the .so files into the firmware

`CMakeLists.txt`:

```cmake
target_link_libraries(scl-fw-drv PRIVATE scl::static
    ${DRV_DIR}/fixtures/libspi.so
    ${DRV_DIR}/fixtures/libuart.so
    ${DRV_DIR}/fixtures/libled.so)
```

The files **must** be named `lib*.so` (`libspi.so`, `libuart.so`, `libled.so`)
for the linker to find them via `-l` convention. CMake for `arm-none-eabi`
filters bare `-lspi` flags; absolute paths bypass this.

### 3. No special compiler flags

| Flag | Needed? | Why |
|------|---------|-----|
| `-fPIC` | No | Firmware is non-PIC; the linker creates PLT/GOT automatically with `.so` inputs |
| `--no-dynamic-linker` | No | Omit this — it suppresses `.dynsym`/`.rel.dyn` generation |
| `-rdynamic` | No | Not supported by `arm-none-eabi-ld` |

### 4. Ensure GOT copy at startup

The `Reset_Handler` in `exception-handlers.c` (included in the firmware source
list) copies `.got` and `.got.plt` from flash to RAM:

```asm
LDR r1, = _sgot
LDR r2, = _esgot
LDR r3, = _sgotdata
```

The linker script `sections-sh.ld` provides the `_sgot`/`_esgot`/`_sgotdata`
symbols and places `.got` in `> RAM AT> FLASH`.

## What the Linker Produces

```
.dynsym    → 13 entries (spi_init, uart_puts, led_blink, …)
.rel.dyn   → 12 R_ARM_JUMP_SLOT relocations
.plt       → Thumb PLT stubs in FLASH
.got       → GOT slots in RAM (initialised with PLT addresses)
.got.plt   → PLT GOT in RAM
.dynamic   → DT_NEEDED entries for libspi.so, libuart.so, libled.so
```

Verify with:
```bash
arm-none-eabi-readelf --dyn-syms scl-fw-drv
arm-none-eabi-readelf -S scl-fw-drv | grep -E 'dynsym|got|plt|rel.dyn'
arm-none-eabi-objdump -d -j .plt scl-fw-drv
```

## main.cpp Usage

```cpp
#include "spi.h"    // extern "C" void spi_init(void);
#include "uart.h"   // extern "C" void uart_puts(const char*);

scl::Module mod;
mod.load_file("fixtures/libspi.so", g_spi_img, sizeof(g_spi_img), &g_syms);

spi_init();          // PLT → GOT → spi_init in RAM
spi_format();
spi_erase_sector(a);
spi_write(0, data, len);
spi_read(addr, buf, len);
```

For modules loaded from SPI flash:

```cpp
spi_read(e->off, g_img, (uint16_t)e->size);
scl::Module m;
m.load_buf(g_img, e->size, g_exec, sizeof(g_exec), &g_syms);
uart_init();         // PLT → GOT → uart_init in g_exec
uart_puts("hello");
```

## Limitations

1. **PLT stubs are Thumb.** Cortex-M4 is Thumb-only. If the linker emits ARM
   PLT stubs (check with `objdump -d -j .plt`), the CPU crashes. Always build
   with `-mthumb`.

2. **CMake filters `-l` for bare-metal.** Use absolute `.so` paths in
   `target_link_libraries`, not `-lspi`. Makefile/Eclipse builds can use
   `-lsh1 -lsh2` directly.

3. **Load before call.** The GOT slot contains a PLT resolver address until
   `patch_main()` runs. Calling a function before its `.so` is loaded causes
   a crash.

4. **C only.** C firmware cannot use the direct-call approach (no C++
   `extern "C"` distinction). Use `Module::sym()` instead.

## Migration from Proxy Calls

| Before (proxy) | After (direct call) |
|----------------|-------------------|
| `mod["spi_init"]()` | `spi_init()` |
| `mod["uart_puts"]("hello")` | `uart_puts("hello")` |
| `spi_write_fn = mod.sym<…>("spi_write")` | `spi_write(…)` (direct) |
| No `extern "C"` in headers | `extern "C" { }` required |
| No `.so` in `target_link_libraries` | `.so` paths required |

## Complete Build Options Reference

### Shared Libraries (.so)

| Flag | What it does |
|------|-------------|
| `-mcpu=cortex-m4 -mthumb` | Thumb-2 code for Cortex-M4 |
| `-mfloat-abi=soft` | Soft-float ABI (no FPU registers) |
| `-fPIC` | **Required.** Position-Independent Code — all globals go through GOT |
| `-fno-inline-functions -fno-common` | Keep every function as a separate symbol; don't merge tentatives |
| `-O0 -g3` | No optimisation, full debug info |
| `-shared` | Produce ET_DYN shared object |
| `-nostartfiles` | No CRT startup (loaded by firmware's loader) |
| `-T mem.ld` | Linker script placing code at vaddr 0 (position-independent) |
| `-Wl,--gc-sections` | Remove unused sections |

**Output:** `libspi.so`, `libuart.so`, `libled.so` (must follow `lib*.so` naming).

### Main Firmware

| Flag / Setting | What it does |
|----------------|-------------|
| `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft` | Same CPU/float settings as the .so (must match) |
| `-O0 -g3 -Wall -Wextra` | Debug-friendly |
| `-ffunction-sections -fdata-sections` | One section per function/data (enables gc-sections) |
| `-std=c++20 -fno-rtti -fno-exceptions` | C++20 no-RTTI no-exceptions (smaller code) |
| `-nostartfiles` | Custom startup via `startup.c` |
| `-Wl,--gc-sections` | Remove unused code |
| `--specs=nano.specs --specs=nosys.specs` | newlib-nano + stub syscalls |
| `-Wl,--no-warn-rwx-segments` | Suppress RWX warning (Cortex-M has no MMU) |
| `-T mem-sh.ld -T libs-sh.ld -T sections-sh.ld` | Memory layout + section placement (`.got` in RAM, `.plt` in FLASH) |
| **`.so` paths in `target_link_libraries`** | **Enables GOT/PLT.** Without this, the linker produces no `.dynsym`/`.rel.dyn` and direct calls fail |
| `-D STM32F411xE -D USE_HAL_DRIVER` | STM32F411 HAL selection |
| `-D HSE_VALUE=25000000` | 25 MHz external crystal (WeAct F411CE) |
| `-D OS_USE_SEMIHOSTING` | Semihosting for file I/O |
| `-D DEBUG -D TRACE -D OS_USE_TRACE_SEMIHOSTING_DEBUG` | Debug + trace output |

### Flags NOT Used (and why)

| Flag | Why not |
|------|---------|
| `-fPIC` | Firmware is non-PIC. The linker creates PLT from `-l` linkage |
| `--no-dynamic-linker` | **Omit this** — it suppresses `.dynsym`/`.rel.dyn` generation |
| `-rdynamic` | Not supported by `arm-none-eabi-ld` |
| `-Wl,-E` | Only needed on .so side when .so has inter-library deps (test fixtures have none) |

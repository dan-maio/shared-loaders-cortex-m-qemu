# SCL Loaders Family Reference Manual
## Architecture, API, and Integration Guide for All SCL Loaders

This manual documents the design, implementation files, internal mechanisms, APIs, and firmware integrations for all loaders within the **Cortex-M Source Code Libraries (SCL)** codebase.

---

## 1. Loader Taxonomy Comparison

SCL provides five core loading blocks plus four XIP adapter targets designed for bare-metal ARM Cortex-M microcontrollers. Each handles code relocation, indirection, and memory allocation differently:

| Loader | Compilation Model | RAM Allocation | Relocation & Indirection | Unloading Strategy |
| :--- | :--- | :--- | :--- | :--- |
| **`scl::dynamic-nolib`** | PIC (`-fpic`) | Fixed Slots (Arena) | `r10` GOT base register + PLT Trampolines | Simple slot clearing |
| **`scl::dynamic-newlib`**| PIC (`-fpic`) | Heap (`malloc`) | Dynamic GOT patching + Link-time binding | Transactional rollback (`patch_journal`) |
| **`scl::static`** | Non-PIC | Static/Fixed Flash | Pre-linked compiler offsets | None (Permament) |
| **`scl::overlay`** | Non-PIC | Shared SRAM Window | Direct absolute addresses | Overwritten by next overlay |
| **`scl::reloc`** | Non-PIC (`-mlong-calls -r`)| Contiguous Arena | Direct absolute patching (`ABS32`/`THM_CALL`) | Arena reset |
| **`scl-xip-*`** | PIC (split R-X/RW) | Flash (text) + RAM (data) | Split-base relocation | Free RAM arena |

---

## 2. scl::dynamic-nolib (Freestanding PIC Dynamic Loader)

Designed for freestanding, zero-allocation micro-kernels. It loads Position-Independent Code (`-fpic`) dynamic libraries (`.so`) into reserved RAM slots.

### 2.1. Key Implementation Files
- **`src/dynamic-nolib/dl.cpp`** ([dl.cpp](../src/dynamic-nolib/dl.cpp)): Implements `dlopen_source()`, `dlsym()`, and `dlclose()`.
- **`src/port/pic-trampoline.c`** ([pic-trampoline.c](../src/port/pic-trampoline.c)): Contains assembly shims to load and swap the `r10` GOT base register.
- **`src/port/dl-cross-call.c`** ([dl-cross-call.c](../src/port/dl-cross-call.c)): Inter-library routing trampoline.

### 2.2. Indirection Mechanism (r10 GOT Swapping)
To execute code loaded anywhere in RAM without absolute patching, libraries reference static variables and external functions relative to a Global Offset Table (GOT). The CPU stores the current library's GOT base address in the `r10` register.

When the host application calls a library function, it routes the call through `pic_call_u32`, which swaps the GOT base register:

```text
[Host Application] ──► pic_call_u32(fn_addr, library_got_base, arg)
                            │
                            ▼ (Assembly Trampoline)
                   1. Push current r10 and link register (LR)
                   2. Load r10 = library_got_base
                   3. Jump to fn_addr (executes library code)
                   4. Pop r10 and PC on return
                            │
                            ▼
                    [Library Code]
```

### 2.3. Usage Example
```cpp
#include "dynamic-nolib/dl.hpp"

void run_nolib_demo() {
    semihost_file src("mylib.so");
    std::byte slot_buffer[4096]; // Pre-allocated RAM slot

    // Load library
    auto load_res = fw::dlopen_source(src, slot_buffer);
    if (!load_res) return;

    fw::handle* h = *load_res;

    // Resolve function address
    auto fn_res = fw::dlsym(*h, "library_function");
    if (!fn_res) return;

    auto fn_ptr = reinterpret_cast<unsigned int (*)(unsigned int)>(*fn_res);

    // Call through the r10 trampoline
    unsigned int ret = fw::pic_call_u32(fn_ptr, h->got_base, 7);
    fw::dlclose(h);
}
```

---

## 3. scl::dynamic-newlib (Hosted Dynamic Loader)

Designed for hosted applications featuring a heap allocator (`newlib-nano`) and requiring strict bytecode rollback validation upon library unloading.

### 3.1. Key Implementation Files
- **`src/dynamic-newlib/dynamic-linker.cpp`** ([dynamic-linker.cpp](../src/dynamic-newlib/dynamic-linker.cpp)): Link-time binder and runtime relocator.
- **`src/dynamic-newlib/patch-journal.cpp`** ([patch-journal.cpp](../src/dynamic-newlib/patch-journal.cpp)): Transactional logging ledger tracking all memory modifications.

### 3.2. Transactional Rollback (`patch_journal`)
To ensure that unloading a library leaves the system in a byte-exact original state, the loader logs every memory modification (such as GOT patches and variable binding redirections) to a `patch_journal` ledger. 

```text
                        Library Loading
                  ┌─────────────────────────┐
                  │ Allocate Heap & Reloc   │
                  └────────────┬────────────┘
                               │
                               ▼
                  ┌─────────────────────────┐
                  │ Log each patch offset   │
                  │  and original bytecode  │
                  └────────────┬────────────┘
                               │
                               ▼
                  ┌─────────────────────────┐
                  │ Run Library Constructors│
                  └─────────────────────────┘
                               │
                        Library Unloading
                               │
                               ▼
                  ┌─────────────────────────┐
                  │ Run Library Destructors │
                  └────────────┬────────────┘
                               │
                               ▼
                  ┌─────────────────────────┐
                  │ Revert each patch in    │
                  │  reverse chronolgy      │
                  └────────────┬────────────┘
                               │
                               ▼
                  ┌─────────────────────────┐
                  │ Assert byte-exact match │
                  │  and free heap segments │
                  └─────────────────────────┘
```

### 3.3. Usage Example
```cpp
#include "dynamic-newlib/dynamic-linker.hpp"

void run_hosted_demo() {
    semihost_file src("mylib.so");
    scl::dynamic_linker linker;

    // Load library with heap allocation
    auto load_res = linker.load(src);
    if (!load_res) return;

    // Resolve symbol directly
    void* sym_addr = linker.resolve("library_function");
    auto fn_ptr = reinterpret_cast<unsigned int (*)(unsigned int)>(sym_addr);

    // Call directly (linking is eager, no trampoline needed)
    unsigned int ret = fn_ptr(7);

    // Unload (reverts all memory modifications and frees heap)
    linker.unload();
}
```

---

## 4. scl::static (Static Unify/Loader)

Acts as a link-time dynamic connector when used with pre-linked `.so` libraries
(§7.3), or as a runtime `.so` loader for standalone PIC modules (§7.5). In
either mode it parses ELF tables and binds symbol addresses into the target
memory buffer.

### 4.1. Key Implementation Files
- **`src/static/shlib-loader.cpp`** ([shlib-loader.cpp](../src/static/shlib-loader.cpp)): Links and patches compiled libraries to static host code.
- **`test-drv/src/main.cpp`** ([main.cpp](../test-drv/src/main.cpp)): Demonstrates runtime-only loading without build-time linkage (see §7.5).

### 4.2. Mechanism
1. **ELF Tables Parsing**: Reads section relocation tables during build time.
2. **Absolute Patching**: Modifies target offsets in the flash binary block directly, bypassing the need for runtime GOT tables or register configurations.

---

## 5. scl::overlay (Linker Overlay Manager)

Manages mutually exclusive modules compiled for a shared physical SRAM memory region.

### 5.1. Key Implementation Files
- **`include/overlay/overlay.h`** ([overlay.h](../include/overlay/overlay.h)): Exposes target overlay handles.

### 5.2. Pipeline Flushing (Instruction Barriers)
When swapping code in the overlay window, the loader executes memory copies to place the target code. Before jumping into the new code, it is critical to execute CPU barriers to ensure that the processor's prefetch queue and pipeline are cleared of old instructions:

```cpp
// Copy code into target overlay RAM region
std::memcpy(overlay_ram_start, overlay_source, overlay_size);

// Force memory writes to complete
__asm__ volatile ("dsb sy" ::: "memory");

// Flush CPU instruction prefetch pipeline
__asm__ volatile ("isb" ::: "memory");
```

---

## 6. scl::reloc (Relocatable Object Loader)

Loads unlinked relocatable objects (`ET_REL`) compiled with `-fno-pic -mlong-calls -r`. It performs absolute, set-wide relocation patching at load-time (see `manual-reloc-loader.md` for a comprehensive, detailed guide).

---

## 7. Test Index

All test firmware images are built with `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake`.

### 7.1. ET_DYN Dynamic Loading Tests (`test/`)

| Target | Loader | Storage | Driver | Run |
|--------|--------|---------|--------|-----|
| `scl-fw-flatfs` | `scl::dynamic-nolib` | Main flash (memory-mapped) | — | `./test-dynamic/build-and-run.sh dynamic-nolib/test3` |
| `scl-fw-flatfs-bitbang-spi` | `scl::dynamic-nolib` | W25Q64 SPI flash | Bitbang GPIO | `./test-dynamic/build-and-run.sh flatfs-bitbang-spi` |
| `scl-fw-flatfs-spi` | `scl::dynamic-nolib` | W25Q64 SPI flash | STM32F4 HAL SPI | `./test-dynamic/build-and-run.sh flatfs-spi` |

### 7.2. ET_REL Static Loading Tests (`test-relocatable/`)

| Target | Storage | Driver | Run |
|--------|---------|--------|-----|
| `scl-fw-classic-reloc` | Main flash (embedded const arrays) | — | `./test-relocatable/build-and-run.sh classic` |
| `scl-fw-classic-spi-reloc` | W25Q64 SPI flash | STM32F4 HAL SPI | `./test-relocatable/build-and-run.sh classic-spi` |
| `scl-fw-reloc` | Semihosting `.rel.o` files | — | `./test-relocatable/build-and-run.sh reloc` |
| `scl-fw-reloc-interrupt` | Semihosting `.rel.o` files | — | `./test-relocatable/build-and-run.sh interrupt` |

### 7.3. Static Unify Tests (`test/`)

| Target | Linker | Run |
|--------|--------|-----|
| `f411ce-sh` | ET_DYN `dlopen` (static test bench) | `cd test-static/test-static && ./build-and-run-*.sh` |
| `f411ce-sh` (cpp) | ET_DYN `dlopen` with C++ EH/RTTI | `cd test-static/test-static-cpp && ./build-and-run-*.sh` |

### 7.4. SPI Flash Detail

See [SPI.md](test/SPI.md) for the full W25Q64 driver comparison (bitbang vs HAL), clock tree, filesystem format, and the complete problem/solution log.

### 7.5. Mixed Static-Loader + FlatFS Test (`test-drv/`)

**What it does**

`test-drv` proves the `scl::static` loader can load a PIC `.so` from a host
filesystem (semihosting) into RAM, then use its exported SPI-flash functions
to provision two more `.so` files onto a W25Q64. Those `.so` files are later
read back from the SPI flash, loaded into a separate RAM buffer via the
identical in-memory ELF parser (`load_from_buf`), and their exported
functions called — all without any compile-time linkage between the main
firmware and the loaded modules.

| Step | Action | Loader |
|------|--------|--------|
| 1 | `spi.so` loaded from semihosting → `spi_init()`, `spi_format()` init the W25Q64 | `scl::static` (`load_so_into`) |
| 2 | `uart.so`, `led.so` loaded from semihosting → raw bytes saved to flatfs on W25Q64 via `spi_write()` | Semihosting `read()` only (no patching) |
| 3 | `uart.so` raw bytes read from flatfs → `load_from_buf()` copies LOAD segments to `g_exec` → symbols resolved → `uart_init()`, `uart_puts()`, `uart_discard()` called | Custom in-memory ELF parser (`load_from_buf`) |
| 4 | Same for `led.so` → `led_init()`, `led_blink()`, `led_discard()` | Same as 3 |

**What it teaches us**

1. **No build-time linkage needed.** The `scl::static` loader works fine without
   `-lspi -luart -lled` at link time. Symbols are resolved by walking `.dynsym`
   in the loaded RAM buffer — no `.rel.dyn` or `_reloc` block required on the
   main side (for zero-reloc PIC objects).

2. **Separate RAM buffers are essential.** A single shared buffer (`g_img`)
   caused dangling function pointers when a new `.so` overwrote the previous
   one's code. The fix uses a permanent buffer (`g_spi_img`) for the SPI
   driver and separate temp/exec buffers (`g_img`, `g_exec`) for provision
   and runtime loading.

3. **Zero-reloc PIC `.so` files don't need patching.** `readelf -r spi.so`
   shows zero relocations. Functions that use only hardcoded peripheral
   register addresses compile to position-independent code without any
   GOT/PLT — `patch_shlib()` returns 0 patched symbols, which is correct.

4. **In-memory ELF parsing is self-contained.** `load_from_buf()` parses
   ELF headers, program headers, section headers, copies LOAD segments,
   applies `R_ARM_RELATIVE` if present, and resolves exported symbols —
   all from a raw buffer, without any file I/O or external loader.

5. **`patch_shlib()` returns `uint32_t` (count), not `bool`.** A return
   value of 0 means "nothing to patch", not "failure". Using `!patch_shlib()`
   as a boolean check caused an early return that skipped symbol resolution.

6. **`elf_image` has a dangling-pointer hazard after move.** The `elf_image`
   class stores a raw `byte_source*` into the `elf_reader`'s heap-allocated
   source. When the `elf_reader` is move-constructed into `shlib_loader`,
   the pointer remains valid (the `unique_ptr` points to the same heap
   object), but any code that reads from `elf_image` after the move reads
   through the same source — which is correct only if the source is still
   alive. The safe pattern is to extract section metadata *before* the move,
   as done in `load_so_into()`.

| Target | Storage | Driver | Run |
|--------|---------|--------|-----|
| `scl-fw-drv` | Semihosting → flatfs on W25Q64 | Bitbang GPIO | `./test-drv/build-and-run.sh` |

> **Note:** `test-drv/` is not under `test/` because it exercises `scl::static`
> as a runtime `.so` loader for `.so` files that are not pre-linked with the
> main firmware — a different scenario from the `test-static/test-static` tests,
> which link `.so` files at build time via `-l`.

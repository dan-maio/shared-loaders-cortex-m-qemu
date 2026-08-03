# Walk Through: What, Why, How — Build Characteristics of Every Test

A detailed walkthrough of every test suite in the SCL repository: what it tests,
how it's compiled, what linker scripts it uses, and **why** each choice was made.

---

## 1. The Toolchain — Two Flavours

### 1.1 Freestanding (main toolchain: `cmake/arm-none-eabi.cmake`)

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
-fpic -O0 -g -ffunction-sections -fno-common
-ffreestanding -fno-tree-loop-distribute-patterns
CXX: -fno-threadsafe-statics
```

**Why these flags:**

| Flag | Why |
|------|-----|
| `-mcpu=cortex-m4 -mthumb` | Target is STM32F411 (Cortex-M4F), Thumb-2 instruction set |
| `-mfloat-abi=hard -mfpu=fpv4-sp-d16` | Use hardware FPU, pass float args in FPU registers (S0-S15) |
| `-fpic` | Position-independent code: PC-relative GOT access, no absolute addresses. Required because loaded `.so` libraries run at runtime-chosen addresses |
| `-ffreestanding` | No hosted C runtime — the firmware provides its own `_start`, `memcpy`, etc. GCC won't assume `main` returns or that `stdlib.h` exists |
| `-fno-tree-loop-distribute-patterns` | Prevent GCC from turning hand-written `memcpy`/`memset` loops back into `memcpy` calls (would create circular dependency in `-nostdlib` builds) |
| `-fno-threadsafe-statics` | Skip mutex locking around function-local statics — bare-metal has no threads |
| `-ffunction-sections -fdata-sections` | Each function/variable gets its own section → `--gc-sections` can strip unused code |
| `-fno-common` | Don't place uninitialized globals in COMMON — catches duplicate symbol errors at link time |

### 1.2 Static loader tests (`test-static/*/cmake/arm-none-eabi.cmake`)

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft
```

**Why different:**
- `-mfloat-abi=soft` — The static loader's `.so` libraries use soft-float ABI. This is because the loader must work with any library regardless of whether it was compiled with hard or soft float. Soft-float is the lowest common denominator.
- No `-fpic` — The static loader patches GOT entries directly, so the main firmware doesn't need PIC.
- No `-ffreestanding` — These tests use `--specs=nano.specs` (newlib-nano), which provides a hosted-like environment with `printf`, `malloc`, etc.

---

## 2. test-static — Static ELF Patcher/Loader

### 2.1 test-static/test-static

**What:** The original static shared-library loader. Loads PIC `.so` files at runtime, resolves symbols, patches GOT entries, runs constructors.

**SCL target:** `scl::static`

**Compile flags:**
```
-O0 -g3 -Wall -Wextra -std=c++20
-fno-rtti -fno-exceptions
-DDEBUG -DUSE_FULL_ASSERT -DOS_USE_SEMIHOSTING -DTRACE
-DSTM32F411xE -DUSE_HAL_DRIVER -DHSE_VALUE=25000000
```

**Why:**
- `-O0 -g3` — Debug build, no optimization, full debug info for stepping through loader logic
- `-fno-rtti -fno-exceptions` — Saves ~20 KB of FLASH; the loader doesn't use `dynamic_cast` or `throw`
- `OS_USE_SEMIHOSTING` — Output goes through BKPT 0xAB to OpenOCD console (no UART needed)
- `HSE_VALUE=25000000` — WeAct BlackPill has 25 MHz crystal (not 8 MHz like Nucleo)

**Linker scripts:** `mem-sh.ld` + `libs-sh.ld` + `sections-sh.ld`

**Link flags:**
```
-nostartfiles --specs=nano.specs -u _printf_float
-Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
-lsh1 -lsh2 -lstdc++
```

**Why:**
- `-nostartfiles` — The platform provides its own `Reset_Handler` and `_start` (newlib's would conflict)
- `--specs=nano.specs` — Link against newlib-nano (small `printf`, `malloc`)
- `-u _printf_float` — Force linking floating-point printf support (otherwise stripped by nano)
- `-Wl,--no-dynamic-linker` — Don't emit `PT_DYNAMIC` — this is a static executable
- `-Wl,--no-warn-rwx-segments` — Suppress the "RWX segment" warning (the loader intentionally creates RWX memory regions)

**Shared library build flags:**
```
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft
-fPIC -fno-inline-functions -fno-common -O0 -g3
-shared -nostartfiles
```

**Why:**
- `-mfloat-abi=soft` — The `.so` files must be ABI-compatible with the soft-float main firmware
- `-fPIC` — Position-independent: all code/data addresses are PC-relative or go through the GOT
- `-shared -nostartfiles` — Produce a shared object, not an executable
- `-fno-inline-functions` — Prevent GCC from inlining across translation units (would break PIC assumptions)

**.so linker scripts:** `mem.ld` (RAM at 0x00000000, 35K) + `sections2.ld`

**Why RAM at address 0:**
- The `.so` files are linked at address 0 (a placeholder). At runtime, the loader copies them to a real RAM address and patches all relocations. The address 0 is never actually used — it's just a convention that "this library can be loaded anywhere."

### 2.2 test-static/test-static-cpp

**What:** Same as test-static but with a C++ shared library (`libcpp_features.so`).

**Extra build flags for the .so:**
```
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
-shared -nostartfiles -nostdlib
```

**Why:**
- `-nostdlib` — The `.so` doesn't link against libstdc++ or libc. It resolves `operator new`, `printf`, etc. from the main firmware at load time via GOT patching.
- `-fno-use-cxa-atexit` — Don't register destructors via `__cxa_atexit` (no libc available in the `.so`)

### 2.3 test-static/test-static-direct

**What:** Like test-static but the `.so` libraries are linked directly at build time (not loaded at runtime). Used to test that the same libraries work both ways.

**Extra link flags:**
```
-L${APP_DIR}/new-lib -lcpp_features
```

### 2.4 test-static/test-drv

**What:** Driver-style test — loads multiple small `.so` files (libspi.so, libuart.so, libled.so) as if they were hardware drivers.

**Different toolchain flags:**
```
-mfloat-abi=soft   (not hard)
```

**Why:** The driver `.so` files are compiled with soft-float to match the widest ABI compatibility. Some libraries might be compiled without FPU support.

---

## 3. test-dynamic — Freestanding Dynamic Loader

### 3.1 test-dynamic/test-dynamic-nolib

**What:** The freestanding `dlopen/dlsym/dlclose` loader — no libc, no newlib. Everything runs from RAM.

**SCL target:** `scl::dynamic-nolib`

**Key difference from test-static:** This loader does full dynamic linking — it reads `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` sections and resolves symbols using the SysV hash table.

**Toolchain:** Main freestanding toolchain (`-fpic -ffreestanding -mfloat-abi=hard`)

**Sub-tests:**

| Test | What it exercises |
|------|-------------------|
| test1 | Basic `dlopen` + `dlsym` — load a `.so`, call a function |
| test2 | 16-test cross-module battery (r10 routing, cross-library calls, lazy binding, exceptions, heap, deep recursion) |
| test3 | Flat-filesystem loading (`.so` files stored in a flash filesystem region) |
| test4 | GOT/PLT walkthrough — demonstrates how relocations work step by step |

**Anchors (`-u` flags):**
```
-u printf -u puts -u sinf -u cosf -u logf -u sqrtf
-u trace_event -u trace_u32
-u fm_callback -u fm_var -u fm_info
```

**Why:** `-u` forces these symbols to be linked even if not directly referenced. This makes them available in the main firmware's `.dynsym` for loaded `.so` files to resolve against.

### 3.2 test-dynamic/test-dynamic-newlib

**What:** Dynamic loader using hosted newlib (std::vector, std::string available).

**SCL target:** `scl::dynamic-newlib`

**Key difference:** `SCL_HOSTED=ON` — enables `std::vector`-based helpers like `collect_relocations()` and the full `dynamic_linker` class with `std::vector<reloc_plan>`.

**Platform:** Uses `platform::weact-f411ce-hosted` (CMSIS + HAL + newlib)

---

## 4. test-overlay — Linker Overlay Manager

### 4.1 test-overlay/test1

**What:** Tests the overlay mechanism — multiple code modules share a single RAM window. Only one module is resident at a time.

**SCL targets:** `scl::dynamic-nolib` + `scl::overlay`

**Extra compile flags for overlay sources:**
```
-fno-pic -mlong-calls
C++: -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-unwind-tables
```

**Why:**
- `-fno-pic` — Overlay modules are placed at a fixed address (the OVL window), not relocated at runtime
- `-mlong-calls` — Calls between overlay modules (which are never co-resident) use absolute addresses via `BLX` instead of relative `BL`. This is required because the caller and callee may be loaded at different addresses in the same window.

**Linker script:** `sections-overlay.ld`

**Memory layout:**
```
FLASH (rx)  : 0x08000000, 512K
RAM   (rwx) : 0x20000000, 96K
OVL   (rwx) : 0x20018000, 16K   ← the shared window
```

**Overlay section (from sections-overlay.ld):**
```ld
OVERLAY : NOCROSSREFS
{
  .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
  .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
  .ovcpp { KEEP(*(.ovcpp.text)) KEEP(*(.ovcpp.rodata)) KEEP(*(.ovcpp.data)) }
} >OVL AT>FLASH
```

**Why OVERLAY + NOCROSSREFS:**
- `OVERLAY` — All modules share the same VMA (runtime address) but have different LMAs (load addresses in FLASH)
- `NOCROSSREFS` — Linker will ERROR if one overlay references another (they're never co-resident, so such a reference would be a latent HardFault)

**Key symbols:**
```
_ov1_lma = LOADADDR(.ov1)   _ov1_size = SIZEOF(.ov1)
_ov2_lma = LOADADDR(.ov2)   _ov2_size = SIZEOF(.ov2)
_ovl_vma = ADDR(.ov1)        ← shared run address
```

---

## 5. test-relocatable — ET_REL Module Loader

### 5.1 test-relocatable

**What:** Loads relocatable object files (`.o`, ET_REL) — kernel-module style. No GOT/PLT, no PIC. Places SHF_ALLOC sections in a RAM arena, resolves `.symtab`, applies `R_ARM_ABS32/TARGET1/REL32/THM_CALL`, runs `.init_array`.

**SCL target:** `scl::reloc`

**Compile flags:**
```
-fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
CXX: -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
```

**Why `-fno-pic -fno-pie`:**
- ET_REL modules are NOT position-independent. They contain absolute addresses that the loader patches at load time. PIC would add unnecessary GOT indirection.

**Why `-Os`:**
- Relocatable modules are typically small; optimize for size to fit in the limited RAM arena.

**Link flags:**
```
-T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
-nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments
```

**Linker script:** `platform/reloc.ld`

**Key difference from other linker scripts:**
- NO `.dynsym`, `.dynstr`, `.hash`, `.rel.dyn`, `.got`, `.dynamic` — these are ET_DYN concepts
- Just plain `.isr_vector`, `.text`, `.data`, `.bss` — a standard non-PIC firmware layout

---

## 6. test-xip — Execute-In-Place

### 6.1 The XIP Concept

XIP leaves the code segment in flash (executed in place) and copies only the data segment to RAM. This is a **two-base model**:
- `text_base` = flash address (code runs directly from flash)
- `data_base` = RAM address (GOT, .data, .bss live here)

### 6.2 Linker Script: `xip-split.ld`

```ld
PHDRS {
  text PT_LOAD FLAGS(5);   /* R-X */
  data PT_LOAD FLAGS(6);   /* RW- */
}
```

**Why PHDRS:**
- Explicitly assigns sections to segments with specific permissions
- `.text`, `.rodata`, `.hash`, `.dynsym`, `.dynstr`, `.rel.dyn` → `:text` (R-X, stays in flash)
- `.data`, `.got`, `.dynamic`, `.bss` → `:data` (RW, copied to RAM)

### 6.3 Linker Script: `sections-flatfs-hosted.ld`

**Memory layout:**
```
FLASH    (rx)  : 0x08000000, 256K    ← firmware code
FLASH_FS (r)   : 0x08040000, 256K    ← filesystem with .so files
RAM      (rwx) : 0x20000000, 128K
```

**Key feature:** `.image_arena` in RAM (32K or 64K) — this is where loaded `.so` files' data segments are copied.

**ASSERT:**
```ld
_sidata + (_edata - _sdata) <= _fs_base
```
**Why:** Ensures the firmware's initialized data load image doesn't overflow into the filesystem region. If this fails, the firmware would corrupt the `.so` files stored in flash.

### 6.4 XIP Test Variants

| Test | Loader | What |
|------|--------|------|
| test-xip-static | `scl::xip-static` | Static loader + XIP: loads .so data segments, leaves code in flash |
| test-xip-static-direct | `scl::xip-common` | Same but libraries are linked at build time |
| test-xip-dynamic-nolib | `scl::xip-dynamic-nolib` | Freestanding dlopen + XIP |
| test-xip-dynamic-newlib | `scl::xip-dynamic-newlib` | Hosted dlopen + XIP |
| test-xip-interrupt | `scl::xip-dynamic-nolib` | XIP with interrupt handling in loaded modules |
| test-xip-interrupt-static | `scl::xip-common` | XIP interrupt with static loader |
| test-xip-interrupt-static-direct | `scl::xip-common` | XIP interrupt with direct linking |

**Common compile flags (all XIP tests):**
```
-fno-pic -fhosted
CXX: -include abort_fix.h
```

**Why `-fno-pic`:**
- The main firmware is NOT PIC — it runs at its link address (0x08000000). Only the loaded `.so` files are PIC.

**Common link flags:**
```
-pie -nostartfiles --specs=nano.specs
-Bsymbolic -E
-T sections-flatfs-hosted.ld
--gc-sections
```

**Why these:**
- `-pie` — Produce a position-independent executable (the loader needs to patch the main firmware's GOT too)
- `-Bsymbolic` — Bind references to the main firmware's own symbols directly (don't go through the GOT for self-references)
- `-E` — Export all symbols (so loaded `.so` files can resolve against main)
- `--specs=nano.specs` — Use newlib-nano for small printf/malloc

---

## 7. Linker Script Patterns Explained

### 7.1 `.inits` section with `_reloc` table

Found in `sections-sh.ld` and hosted `sections-flatfs-hosted.ld`:

```ld
.inits : ALIGN(4) {
    _reloc = .;
    LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
    LONG(LOADADDR(.dynsym)); LONG(SIZEOF(.dynsym));
    LONG(LOADADDR(.dynstr)); LONG(SIZEOF(.dynstr));
    LONG(LOADADDR(.hash));   LONG(SIZEOF(.hash));
    ...
} >FLASH
```

**Why:** The static loader reads this table at runtime to find the relocation sections without parsing the ELF file. It's a "cheat sheet" embedded in the firmware binary. The loader's `link_context::from_self()` reads `_DYNAMIC` at its RAM VMA.

### 7.2 Contiguous `.data` + `.dynamic` + `.got` + `.got.plt`

Found in WeAct platform `sections.ld`:

```ld
.data : ALIGN(4) { _sdata = .; *(.data .data.*); } >RAM AT>FLASH
.dynamic : ALIGN(4) { *(.dynamic); } >RAM AT>FLASH
.got : ALIGN(4) { _sgot = .; *(.got); } >RAM AT>FLASH
.got.plt : ALIGN(4) { _sgot_plt = .; *(.got.plt); _edata = .; } >RAM AT>FLASH
```

**Why contiguous:** `Reset_Handler` copies the entire `[_sdata, _edata)` block from FLASH to RAM in one linear pass. This ensures `.dynamic` (which `link_context::from_self()` reads) and `.got` (which the PC-relative GOT access needs) are all initialized.

### 7.3 `.image_arena` (NOLOAD)

```ld
.image_arena (NOLOAD) : ALIGN(8) {
    _simage_arena_start = .;
    . = . + _simage_arena_size;
    _simage_arena_end = .;
} >RAM
```

**Why NOLOAD:** This region is reserved for loaded `.so` files at runtime. It's not initialized from FLASH — the loader fills it. `NOLOAD` prevents the linker from creating a load image for it.

### 7.4 OVERLAY sections

```ld
OVERLAY : NOCROSSREFS {
    .ov1 { KEEP(*(.ov1.text)) KEEP(*(.ov1.rodata)) KEEP(*(.ov1.data)) }
    .ov2 { KEEP(*(.ov2.text)) KEEP(*(.ov2.rodata)) KEEP(*(.ov2.data)) }
} >OVL AT>FLASH
```

**Why:**
- All overlay modules share VMA = `OVL` (0x20018000) but have different LMAs in FLASH
- `KEEP` defeats `--gc-sections` (the module entry points are reached by symbol, not by section name)
- `NOCROSSREFS` makes ld fail if one overlay references another — they are never co-resident

---

## 8. Summary Table

| Test Suite | Float ABI | PIC | libc | SCL Target | Linker Script | Key Feature |
|-----------|-----------|-----|------|-----------|---------------|-------------|
| test-static | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Runtime GOT patching |
| test-static-cpp | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | C++ in .so files |
| test-static-direct | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Direct linking |
| test-drv | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Driver-style .so |
| test-dynamic-nolib | **hard** | **-fpic** | **none** | scl::dynamic-nolib | weact sections.ld | Full dlopen, no libc |
| test-dynamic-newlib | **hard** | **-fpic** | newlib | scl::dynamic-newlib | weact sections.ld | Hosted dlopen |
| test-overlay | **hard** | **-fpic** | none | scl::dynamic-nolib + scl::overlay | sections-overlay.ld | OVERLAY window manager |
| test-relocatable | **hard** | **-fno-pic** | none | scl::reloc | reloc.ld | ET_REL module loading |
| test-xip-* | soft/hard | -fno-pic (main) | nano.specs | scl::xip-* | sections-flatfs-hosted.ld | Execute-in-place |
| common-elf-reader/test | host | N/A | host libc | N/A (test only) | N/A | Host-side parser test |
| common-elf-reader/test-arm | hard | none | none (semihosting) | N/A (test only) | weact sections.ld | Bare-metal parser test |

---

## 9. Key Design Decisions

### Why soft-float for static loader, hard-float for dynamic?

The **static loader** (`test-static`) uses `-mfloat-abi=soft` because:
1. The `.so` files are compiled separately and must be ABI-compatible with the main firmware
2. Using soft-float ensures any library works regardless of whether it uses FPU
3. The static loader doesn't need FPU performance — it's a patcher, not a compute workload

The **dynamic loader** (`test-dynamic-nolib`) uses `-mfloat-abi=hard` because:
1. The main firmware is the only thing compiled — no external `.so` ABI constraint
2. The target code benefits from hardware FPU
3. The dynamic loader's trampolines handle FPU register saving/restoring explicitly

### Why `-fpic` for freestanding, `-fno-pic` for relocatable/XIP?

**Freestanding dynamic loader** (`-fpic`): The main firmware and loaded `.so` files share the same GOT mechanism. Both are PIC so the loader can patch GOT entries uniformly.

**Relocatable loader** (`-fno-pic`): ET_REL modules contain absolute addresses. The loader patches them directly. PIC would add unnecessary indirection.

**XIP** (`-fno-pic` for main): The main firmware runs at a fixed flash address. Only the loaded `.so` files are PIC (they have their own GOT). The main firmware's GOT is patched once at boot.

### Why `.so` files linked at address 0?

```ld
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35K }
```

The `.so` files are linked at address 0 as a convention meaning "relocatable, no preferred address." At runtime, the loader places them at the actual allocated address and applies relocations. The address 0 is never used — it's a placeholder that makes it clear the library doesn't assume a fixed load address.

### Why `_reloc` table in `.inits`?

The static loader needs to find `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` in the firmware binary. Instead of parsing the ELF file (which would require the ELF parser to run on the firmware itself), the linker script embeds a simple table of `{load_address, size}` pairs. The loader reads this table directly — one memory read, no parsing.

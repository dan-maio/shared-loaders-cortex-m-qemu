# Comparison of XIP Dynamic Loaders: Freestanding vs. Hosted

This document details the architectural, memory, symbol-resolution, and build-time differences between the freestanding (`xip-dynamic-nolib`) and hosted (`xip-dynamic-newlib`) Execute-In-Place (XIP) dynamic loaders.

---

## 1. Overview and Core Philosophies

Both loaders are designed for **Cortex-M4 systems** to load Position-Independent Code (PIC) shared libraries (`.so`) compiled with `r10`-based addressing (`-msingle-pic-base -mpic-register=r10`). This model keeps the read-only (`R-X`) code segment in Flash (executing in place) and copies only the read-write (`RW`) data/GOT segment into RAM.

| Attribute | Freestanding XIP (`xip-dynamic-nolib`) | Hosted XIP (`xip-dynamic-newlib`) |
| :--- | :--- | :--- |
| **Namespace** | `xip::nolib` | `xip::newlib` |
| **C Standard Library** | None (Freestanding) | Newlib / Newlib-nano (`libc.a`, `libm.a`, `libstdc++.a`) |
| **Heap Allocator** | Custom `fw::image_arena` (Static Region) | System Heap (`malloc`/`free`, `std::make_unique`) |
| **Main Symbol Lookup** | Relies on `_DYNAMIC` array (self-parsing) | Relies on linker-generated `_reloc` table |
| **Cross-Library Bindings** | Lazy/Eager at runtime, manually enabled | Eagerly patched at provision time |
| **Toolchain File** | `cmake/arm-none-eabi.cmake` | `cmake/arm-none-eabi.cmake` (adapter); `cmake/arm-none-eabi-hosted.cmake` (test app) |

---

## 2. Architectural Differences

### 2.1 Memory Management
* **`xip-dynamic-nolib`**: Does not assume the existence of a heap. Segment allocations for RAM-based sections (like `.data`, `.bss`, and `.got`) are carved out of a pre-allocated RAM block using `fw::image_arena()` (a `loader::region_allocator` defined in [image-memory.cpp](../src/image-memory.cpp)).
* **`xip-dynamic-newlib`**: Operates on top of the standard C++ memory model. It utilizes `std::make_unique<std::byte[]>` to allocate dynamic RAM buffers for copy-whole segments (or copy-write segments in XIP mode), routing allocations directly through the platform's `malloc` heap.

### 2.2 Linker & Runtime Context Resolution
* **`xip-dynamic-nolib`**: Resolves main firmware symbols by querying `fw::link_context::from_self()`. It locates the linker-provided `_DYNAMIC` array (which tracks `.dynsym`, `.dynstr`, and `.hash`) at runtime to dynamically inspect the main application's exported symbol tables.
* **`xip-dynamic-newlib`**: Leverages a structured `_reloc` address-size table defined in the main firmware's linker script `.inits` block:
  ```ld
  _reloc = .;
  LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
  LONG(LOADADDR(.dynsym)); LONG(SIZEOF(.dynsym));
  LONG(LOADADDR(.dynstr)); LONG(SIZEOF(.dynstr));
  LONG(LOADADDR(.hash)); LONG(SIZEOF(.hash));
  LONG(LOADADDR(.inits)); LONG(SIZEOF(.inits));
  ```
  The hosted `firmware::link_context` constructor parses this 10-word table directly to find main's dynamic section bounds.

### 2.3 Cross-Library Trampoline Routing
To support cross-library calls (e.g., `lib_a` calling `lib_b` or `lib_b` invoking main's `printf`), both loaders route external calls through the assembly-implemented `xip_cross_call` trampoline. The trampoline preserves integer/VFP registers, swaps `r10` to the target library's GOT base, jumps to the destination, and restores the caller's `r10` on return.

However, the mechanism for activating this routing differs:
* **`xip-dynamic-nolib`**: Cross-library calls are toggled explicitly at runtime via `fw::dl_enable_cross_call(true)`. Unresolved GOT slots of loaded libraries are lazily redirected to `xip_cross_call`.
* **`xip-dynamic-newlib`**: GOT slots are eagerly patched at **provision time** (before flashing, while the ELF binary is read via semihosting) by calling `xip::newlib::patch_main_imports`. All external jumps (`JUMP_SLOT` relocations with `st_value == 0`) are hardcoded to point to the `xip_cross_call` trampoline address. At run time, `xip_cross_lookup` scans the registered handles to fetch the target address and Callee GOT.

---

## 3. Linker Script Requirements

The linker scripts for both loaders partition Flash and RAM sections, but the hosted loader has stricter requirements to support the `_reloc` symbol layout:

1. **`xip-dynamic-nolib` (`sections-flatfs-hosted.ld` for nolib)**:
   Only requires standard `.dynsym`, `.dynstr`, and `.hash` sections in Flash, and relies on `_DYNAMIC` to locate them.
2. **`xip-dynamic-newlib` (`sections-flatfs-hosted.ld` for newlib)**:
   Must declare the `_reloc` table at the beginning of the `.inits` section, referencing the physical load addresses (via `LOADADDR`) and sizes of the dynamic tables:
   ```ld
     .inits : ALIGN(4)
     {
       _reloc = .;
       LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
       LONG(LOADADDR(.dynsym)); LONG(SIZEOF(.dynsym));
       LONG(LOADADDR(.dynstr)); LONG(SIZEOF(.dynstr));
       LONG(LOADADDR(.hash)); LONG(SIZEOF(.hash));
       LONG(LOADADDR(.inits)); LONG(SIZEOF(.inits));
       ...
     } >FLASH
   ```

---

## 4. Build Toolchain and C++ Exceptions

* **Toolchain selection**: 
  - `nolib` is built with `cmake/arm-none-eabi.cmake` (freestanding target, no default OS libraries).
  - `newlib` adapter (`scl-xip-dynamic-newlib`) builds with `cmake/arm-none-eabi.cmake`; the test app additionally requires `cmake/arm-none-eabi-hosted.cmake` with `-DSCL_HF=ON` for newlib startup code and hard-float ABI support.
* **C++ Exceptions and RTTI**:
  - In `nolib` tests, C++ exceptions thrown across library boundaries are not fully supported due to standard library unwinder limitations.
  - In `newlib` tests, `libstdc++` provides the standard unwinder (`__gnu_Unwind_Find_exidx`). The `xip_cross_call` trampoline is annotated with `.fnstart`, `.save`, and `.pad` directives, allowing the unwinder to step through the assembly frame and propagate C++ exceptions correctly back to catch blocks in the calling library or main application.

---

## 5. Didactic GOT/PLT Walkthrough & Address Region Markers

To help developers verify and visualize the memory layout of loaded segments under the split XIP execution model, the complex walkthrough (`walkthrough_complex.cpp`) executes a didactic before-and-after resolution dump of the Global Offset Table (GOT) and Procedure Linkage Table (PLT) slots. 

To make it immediately clear where each symbol, slot, and resolved function lives, the outputs are explicitly annotated with:
* **`(*F*)`**: Denotes addresses residing in on-chip **Flash** memory (e.g., executing in place, in the `0x08xxxxxx` range).
* **`(*R*)`**: Denotes addresses residing in **SRAM / RAM** (e.g., mutable variables, the relocated GOT slots, or RAM-based bss segments, in the `0x20xxxxxx` range).

This trace details:
1. **Initial GOT values BEFORE relocation**: Shows the raw bytes as compiled in the ELF file in Flash, annotated with `(*F*)` / `(*R*)` if they contain static target addresses.
2. **Patched GOT values AFTER relocation**: Shows the dynamic memory slots in SRAM (`(*R*)`) after the loader rebases relative addresses and resolves external references to main or peer libraries.
3. **PLT JUMP_SLOT routing**: Displays each Procedure Linkage Table import slot mapped to the final resolved functions (e.g. standard library math functions `sinf`/`cosf`, memory allocators, or print utilities) detailing the exact address hops.


# Cortex-M Shared library Loader (SCL) Codebase Analysis

This document provides a comprehensive codebase and architectural analysis of the **Cortex-M Source Code Libraries (SCL)**, designed for loading shared libraries on ARM Cortex-M microcontrollers (with a reference target of the WeAct BlackPill STM32F411CE board).

---

## 1. Architectural Architecture & Core Design

The SCL project acts as a bare-metal, micro-scale equivalent of Linux's dynamic linker (`ld.so`/`dlopen`). It is built as a set of **INTERFACE source libraries** using CMake. Rather than compiling into static `.a` archives or dynamic `.so` libraries, SCL propagates its C/C++ source files directly to the consuming firmware target. This ensures the firmware compiles the loader code with its own compiler flags (optimizations, CPU flags, floating-point ABI) exactly once.

```text
        ┌───────────────────────────────────────────────┐
        │        Consuming Application Firmware         │
        └───────┬───────────────┬───────────────┬───────┘
                │               │               │
                │ (links)       │ (links)       │ (links)
                ▼               ▼               ▼
        ┌───────────────┐┌───────────────┐┌───────────────┐
        │scl::dynamic-  ││scl::dynamic-  ││ scl::overlay  │
        │    nolib      ││    newlib     ││               │
        └───────┬───────┘└───────┬───────┘└───────┬───────┘
                │               │               │
                │ (depends)     │ (depends)     │ (depends)
                └───────────────┼───────────────┘
                                ▼
        ┌───────────────────────────────────────────────┐
        │                  scl::common                  │
        │  (Portable C++20 parsing & resolution core)   │
        │ ┌───────────────────────────────────────────┐ │
        │ │  byte_source       ──  Storage Abstraction │ │
        │ │  elf_image         ──  ELF Parser          │ │
        │ │  symbol_resolver   ──  SysV Hash Lookup    │ │
        │ │  reloc_applier     ──  Eager relocator     │ │
        │ │  LazyResolve      ──  Lazy GOT Binder     │ │
        │ │  region_allocator  ──  RAM Arena Allocator │ │
        │ └───────────────────────────────────────────┘ │
        └───────────────────────────────────────────────┘

        Also depending on scl::common:
        scl::static, scl::reloc, scl-xip-common

        XIP adapter targets:
        scl-xip-dynamic-nolib  → scl-xip-common + scl::dynamic-nolib
        scl-xip-dynamic-newlib → scl-xip-common + scl::dynamic-newlib
        scl-xip-static         → scl-xip-common + scl::static
```

### 1.1 The Library Blocks

The SCL has **ten** library targets: five core loaders, one shared XIP core, and four XIP adapter targets. See `README.md` §2.

1. **`scl::common`**: The shared, portable core. Written in portable C++20, it contains the ELF parsing, symbol resolution, basic relocation mapping, and memory management classes. It compiles and runs both on the host (for unit testing) and on the Cortex-M target.
1. **`scl::dynamic-nolib`**: The runtime dynamic loader (`dlopen`-style). It operates under a strict position-independent code (PIC) model. By reserving register `r10` as the static base (GOT pointer) for loaded libraries, it allows code segments to reside and execute from arbitrary locations in RAM.
3. **`scl::dynamic-newlib`**: A hybrid loader for hosted firmware (using `newlib-nano` and heap allocation). It performs link-time symbol binding and runtime GOT patching with a complete **`patch_journal`** fallback. Unloading a library reverts all modifications to the host and library images, ensuring a byte-exact rollback. Uses a weak `scl_lookup_firmware_symbol` hook for fallback symbol resolution.
4. **`scl::overlay`**: A minimal static linker-overlay window manager. It provides a lightweight framework to swap compiled overlays in and out of a shared SRAM region, executing memory copies with appropriate Cortex-M memory and instruction barriers (`DSB; ISB`).
5. **`scl::static`**: Standalone ELF32 static patcher. Loads PIC `.so`, patches GOT, handles DT_NEEDED dependencies.
6. **`scl::reloc`**: ET_REL module loader. Loads relocatable `.o` files with `load`/`load_set` for cyclic dependencies. No GOT/PLT/r10.
7. **`scl-xip-common`**: Shared XIP core (segment-map, relocate, compact, cross-call trampoline).
8. **`scl-xip-dynamic-nolib`**: XIP adapter for freestanding dlopen.
9. **`scl-xip-dynamic-newlib`**: XIP adapter for hosted dlopen.
10. **`scl-xip-static`**: XIP adapter for static patcher.

---

## 2. Shared Core Components (`scl::common`)

The common core defines the interface contract for memory representation and storage.

* **`byte_source`** ([byte-source.hpp](../common-elf-reader/include/loader/byte-source.hpp)): Hides the storage medium. Relies on `read_at(offset, span)` to stream bytes. Example backends:
  * `semihost_file`: Opens a file on the host machine via the ST-Link GDB server interface.
  * `Flashbyte_source`: Maps an address offset to an on-chip, memory-mapped flash location.
  * `Memorybyte_source`: Direct wrapper around a host memory buffer (for testing).
* **`elf_image`** ([elf-image.cpp](../common-elf-reader/src/elf-image.cpp)): Parses 32-bit ELF headers (`elf::Ehdr`) and Program Headers (`elf::Phdr`). Computes memory boundaries using segment virtual addresses:
  $$\text{Image Size} = \max(\text{Segment } v\_addr + \text{Segment } memsz)$$
* **`symbol_resolver`** ([symbol-resolver.cpp](../src/symbol-resolver.cpp)): Resolves symbol names using the SysV ELF hash table (`.hash` section).
* **`reloc_applier`** ([reloc-applier.cpp](../src/reloc-applier.cpp)): Applies relocation entries from `.rel.dyn` and `.rel.plt` directly onto memory spans without heap allocation.
* **`region_allocator`** ([region-allocator.cpp](../src/region-allocator.cpp)): Manages fixed-size RAM slots in a reserved arena for loaded binary segments.

---

## 3. The Dynamic Loader (`scl::dynamic-nolib`)

Exposes a standard interface: `dlopen_source()`, `dlsym()`, and `dlclose()` ([dl.cpp](../src/dynamic-nolib/dl.cpp)).

### 3.1 Relocation Types Handled
Relocation happens in-place on the allocated RAM image segments:

| Relocation Code | Relocation Name | Action taken in `reloc_applier` |
| :--- | :--- | :--- |
| `23` | `R_ARM_RELATIVE` | Adjusts address offset relative to the RAM load base. |
| `21` / `22` | `R_ARM_GLOB_DAT` / `R_ARM_JUMP_SLOT` | Writes the absolute address of the resolved symbol. |
| `2` | `R_ARM_ABS32` | Adds the absolute symbol address to the existing in-place addend. |
| `3` | `R_ARM_REL32` | Computes relative jump: $S + A - P$ (where $P$ is the absolute patch location). |
| `20` | `R_ARM_COPY` | Copies a specified block size directly from the main firmware's definition into the library's local variable slot. |

### 3.2 Position-Independent Code Execution (r10 register)
To execute code that has been loaded anywhere in RAM, binaries must be built with PIC support. The SCL loader expects:
* Consuming firmware links with `-pie -Wl,-E` (exporting dynamic symbols) and `-Wl,-Bsymbolic`.
* Loaded libraries compiled with `-fpic -msingle-pic-base -mpic-register=r10`.
* Calls from the application to the library route through the **`pic_call_*`** trampolines ([pic-trampoline.c](../src/port/pic-trampoline.c)). This sets register `r10` to the target library's Global Offset Table (GOT) base before jumping.

```
Application call -> pic_call_u32(func, got_base, arg)

                      |
                      v
             [pic_trampoline.S]
             - Push R10, LR
             - Set R10 = got_base
             - Call func
             - Pop R10, PC
```

### 3.3 Advanced Features
* **Lazy Binding**: To avoid slow lookups over semihosting during `dlopen`, the loader redirects unresolved `JUMP_SLOT` GOT entries to `dl_runtime_resolve` ([lazy-resolve.c](../src/port/lazy-resolve.c)). Upon first invocation, `dl_fixup` ([dl.cpp:L465](../src/dynamic-nolib/dl.cpp#L465)) resolves the symbol, patches the GOT slot, and jumps to the target.
* **Cross-library Calls (r10 routing)**: When multiple libraries call one another, register `r10` must change dynamically. The loader patches external `JUMP_SLOT` entries to point to `dl_cross_call` ([dl-cross-call.c](../src/port/dl-cross-call.c) — a thin wrapper over the shared [xip-cross-call.c](../src/xip/xip-cross-call.c)), which dynamically resolves the callee, swaps `r10` to the callee's GOT base, runs the function, and restores the caller's `r10` upon return.
* **Exceptions & Dynamic Casts**: Registers `.ARM.exidx` frame boundaries via `dl_exidx_register` ([unwind-find-exidx.c](../src/port/unwind-find-exidx.c)) so the C++ exception unwinder can locate exception handling tables (`.ARM.extab`) within the dynamically loaded modules. It also resolves dynamic casts and dynamic type info across module boundaries.

### 3.4 POSIX C++ Class Facade (`class dl`)
SCL provides a global `class dl` that mimics the Linux POSIX dynamic shared loading API precisely:

```cpp
class dl {
public:
    static void* dlopen(const char* filename, int flag) noexcept;
    static void* dlsym(void* handle, const char* symbol) noexcept;
    static int dlclose(void* handle) noexcept;
    static char* dlerror() noexcept;
};
```

#### Under the Hood:
1. **Dynamic Backend Mapping**: The static method `dl::dlopen` calls a weak function `scl_impl_dlopen(filename, flag)`. The target platform provides the strong definition of this helper, routing the request to `fw::dlopen` (semihosting backend) or `fw::dlopen_fs` (FlatFS on-chip flash backend) automatically based on compile-time linkage.
2. **Error Translation**: The class translates internal `loader::Error` return codes into descriptive error messages. Calling `dl::dlerror()` retrieves the last formatted error description and automatically clears the active error flag (so subsequent calls return `nullptr`, matching Linux's exact behavior).
3. **Types & Flags**: The standard POSIX macros `RTLD_LAZY`, `RTLD_NOW`, `RTLD_GLOBAL`, and `RTLD_LOCAL` are mapped and defined as flags.

---

## 4. The Static Loader (`scl::dynamic-newlib`)

The static loader is designed for hosted applications (with newlib heap) where memory layout is static, but dynamic additions are required on demand ([dynamic-linker.cpp](../src/dynamic-newlib/dynamic-linker.cpp)).

* **Bidirectional GOT Patching**: The loader can patch the main firmware's GOT with definitions from the loaded library (`patch_main_got`). This allows direct C/C++ function calls from main into the library.
* **The `_reloc` Linker Table**: The static loader does not parse `_DYNAMIC`. Instead, it uses a linker-provided `_reloc` array mapping `.rel.dyn`, `.dynsym`, `.dynstr`, and `.hash` segments, enabling fast symbol lookups.

### 4.1 Necessity and Usage of `patch_journal`

The [patch_journal](../include/dynamic-newlib/patch-journal.hpp) ([patch-journal.cpp](../src/dynamic-newlib/patch-journal.cpp)) acts as a transaction-log ledger of all memory patches performed by the loader.

#### Necessity:
1. **Undoing Outside Edits**: The loader links the library bidirectionally, meaning it patches the **main firmware's own GOT/PLT** (`patch_main_got`). Simply freeing the library's memory would leave dangling pointers in the main application's GOT/PLT.
2. **Preventing Contamination**: When reloading libraries or swapping hot-swappable plugins into the same RAM region, any leftovers from previous libraries in the application's global tables would point to freed memory, leading to crashes/`HardFaults`.
3. **Mid-Failure Rollback**: Allows safe recovery from loading failures by fully reverting all patches applied up to the point of failure.

#### Lifecycle Usage:
* **During Loading (`dynamic_linker::load_from_source`)**:
  Before overwriting any word during `apply_plan` or `patch_main_got`, the loader logs the target address and original value using `patch_journal::record()` or `patch_journal::record_bytes()`.
* **During Unloading (`dynamic_linker::unload`)**:
  Replays the journal entries in reverse order via `patch_journal::revert()`, restoring the original state of all modified memory regions to a byte-exact match prior to the library being loaded.

---

## 5. Summary of Test Infrastructure

The project features a highly complete test suite designed for both host validation and target execution:

1. **Host Unit Tests** (6 suites, `SCL_BUILD_TESTS=ON`):
   * Run on the build machine without hardware. Validate the ELF parser, symbol resolver, relocation applier, and overlay manager.

2. **On-Target Freestanding Apps** (`test-dynamic/` and `test-overlay/`):
   * `scl-fw-dyn1`: Basic loading, reload, inter-library imports, C++ objects, cross-boundary exceptions.
   * `scl-fw-dyn2`: 16-test cross-module battery (r10 routing, lazy binding, exceptions, heap, deep recursion).
   * `scl-fw-ovl1`: Side-by-side comparison of `scl::overlay` and `scl::dynamic-nolib`.
   * `scl-fw-flatfs`: On-chip flash filesystem loading (sectors 6–7).
   * `scl-fw-gotplt-demo`: GOT/PLT lazy-binding didactic walkthrough.

3. **On-Target Hosted Apps** (`test-dynamic/test-dynamic-newlib/`):
   * `scl-fw-dynamic-newlib1`/`scl-fw-dynamic-newlib2`: Hosted dynamic loading with journal rollback verification (soft/hard float).

4. **Static Patcher Tests** (`test-static/`):
   * `f411ce-sh`, `static-cpp`, `static-direct`, `drv`: Standalone ELF32 static patcher with various scenarios.

5. **Relocatable Module Tests** (`test-relocatable/`):
   * `scl-fw-reloc`: 9-check ET_REL battery with cyclic dependencies.
   * `scl-fw-classic-reloc`: Classic relocatable demo with flatfs.
   * `scl-fw-classic-spi-reloc`: SPI flash storage loading.
   * `scl-fw-reloc-interrupt`: ET_REL with interrupt handler registration.

6. **XIP Tests** (`test-xip/`, 8 targets):
   * `scl-fw-xip-nolib1`: Freestanding dlopen + XIP.
   * `scl-fw-xip-newlib1`: Hosted dlopen + XIP.
   * `scl-fw-xip-static-semihosting`: Static loader + XIP.
   * `scl-fw-xip-static-direct`: Direct API calls + XIP.
   * `scl-fw-xip-interrupt1`: XIP + SysTick interrupt trampoline.
   * `scl-fw-xip-interrupt-static`: XIP + TIM2 interrupt + static.
   * `scl-fw-xip-interrupt-static-direct`: Direct calls + interrupts.
   * `scl-fw-xip-static-spi-flash`: SPI flash → internal flash → XIP.

---

## 6. Integrating SCL into a New Project

To add SCL to an application, link with the namespaced CMake targets:

```cmake
add_subdirectory(cortexm-shared-loader SCL)
target_link_libraries(my_firmware PRIVATE scl::dynamic-nolib) # or scl::overlay, scl::dynamic-newlib
```

### 6.1 Application Requirements
1. **Startup & Platform**: The application must configure CPU clocks, initialize RAM/Flash, and define startup entry points.
2. **Linker Script Setup**:
   * For **`scl::dynamic-nolib`**: Reserve the RAM arena boundaries `_simage_arena_start` and `_simage_arena_end`. Link with `-pie -Wl,-E` and provide `-Wl,-u,<sym>` anchors for kept symbols.
   * For **`scl::dynamic-newlib`**: Emit the `_reloc` table mapping the dynamic tables.
   * For **`scl::overlay`**: Implement an `OVERLAY : NOCROSSREFS` memory region block.
3. **Logging Back-end**: Implement the weak logger hooks in your code to capture SCL diagnostics:
   ```c
   #include "scl/log.h"
   void scl_log_puts(const char* s) { my_uart_puts(s); }
   void scl_log_putc(char c)        { my_uart_putc(c); }
   ```

---

## 7. Function Call Graph and Control Flows

The following Unicode text diagrams illustrate the runtime execution pipelines, relocation routines, lazy bindings, and clean rollback mechanics across the entire SCL codebase (common core, dynamic/static loaders, overlays, and host/target test architectures).

### 7.1 Dynamic Load Flow Chart (dl::dlopen)

This diagram details the sequence triggered when a target test application requests a library to be loaded dynamically:

```text
 [ main() / Test App ] ──► dl::dlopen(path)
                              │
                              ▼
                      scl_impl_dlopen()
                              │
                              ▼ (semihost vs flatfs)
                      fw::dlopen()  or  fw::dlopen_fs()
                              │            │
                              │            ▼
                              │         fs_find() / Flashbyte_source()
                              │            │
                              ▼            ▼
                      fw::dlopen_source(byte_source&, name)
                              │
                              ├───────────────────────────────┐
                              ▼                               ▼
                      elf_image::parse()               alloc_handle()
                              │
                              ├───────────────────────────────┐
                              ▼                               ▼
                      elf_image::image_size()          image_arena().alloc()
                              │                               │
                              ▼                               ▼
                      elf_image::image_base()          region_allocator::alloc()
                              │
                              ▼ (loop segments)
                      elf_image::program_header()
                              │
                              ▼ (PT_LOAD segments copy)
                      byte_source::read_at()
                              │
                              ▼ (load section headers once)
                      section_cache::load()
                              │
                              ├───────────────────────────────┬───────────────────────────────┐
                              ▼                               ▼                               ▼
                      section_cache::find()            sec_in_image()                  combined_resolver()
                      (for .dynsym, .dynstr, .hash)   (retrieve segment spans)        (links dependencies)
                              │
                              ▼ (loop tables: .rel.dyn, .rel.plt)
                      reloc_applier::apply_rel()
                              │
                              ├───────────────────────────────┬───────────────────────────────┐
                              ▼                               ▼                               ▼
                      symbol_source()                  symbol_resolver::address_of()    link_context::resolve()
                      (combined thunk lookup)         (SysV hash .dynsym search)      (firmware fallback)
                              │
                              ▼ (configure lazy binding)
                      dl_runtime_resolve() (weak)
                              │
                              ▼ (dynamic r10 cross-call veneers)
                      dl_cross_call() (weak)
                              │
                              ├───────────────────────────────────────────────────────────────┐
                              ▼ (unwinding registration)                                      ▼ (ctors call)
                      dl_exidx_register()                                             pic_call_void()
                      (registers .ARM.exidx ranges)                                   (loops .init_array)
```

### 7.2 Dynamic Symbol Resolution & Invocation Flow Chart

This diagram maps how symbol lookup (`dlsym`), direct library execution via assembly trampolines, and lazy/deferred resolution happen during library runtime:

```text
 [ main() / App ] ──► dl::dlsym(handle, name)
                         │
                         ▼
                      fw::dlsym(handle&, name)
                         │
                         ▼
                      symbol_resolver::address_of(name)
                         │
                         ▼ (resolved address)
                     Return (void* target)

 [ App Function Call ] ──► pic_call_u32(fn_addr, got_base, arg)
                              │
                              ▼ (trampoline.S)
                           pic_call_void / pic_call_u32 / pic_call_u32_2
                              │
                              ▼ (sets R10 = got_base, jumps to fn_addr)
                           [ Loaded Shared library Code ]

 [ Lazy Call (First) ] ──► JUMP_SLOT slot jumps via PLT ──► dl_runtime_resolve()
                                                               │
                                                               ▼ (saves registers)
                                                            dl_fixup(slot_addr)
                                                               │
                                                               ├───────────────────────────────┐
                                                               ▼                               ▼
                                                            link_context::from_self()        lazy_resolve_slot()
                                                               │                               │
                                                               ▼                               ▼
                                                            combined_resolver::thunk()       symbol_resolver::
                                                            (resolves absolute target)       address_of()
                                                               │
                                                               ▼ (patches GOT slot)
                                                            *slot = target_addr
                                                               │
                                                               ▼ (returns target_addr)
                                                            Tail-call target function

 [ Cross-Lib Call ]    ──► JUMP_SLOT routes via PLT ───► dl_cross_call()
                                                           │
                                                           ▼ (saves registers)
                                                        dl_cross_lookup(slot_addr)
                                                           │
                                                           ├───────────────────────────────┐
                                                           ▼                               ▼
                                                        Caller handle prov lookup       link_context::resolve()
                                                           │                               │
                                                           ▼                               ▼
                                                        symbol_resolver::address_of()    Address target returned
                                                           │
                                                           ▼ (returns callee target + callee got_base)
                                                        Swap R10 to callee got_base, call callee, restore R10
```

### 7.3 Static Load & Relocation Flow Chart (dynamic_linker::load_from_source)

This diagram details the static loader pipeline, including bidirectional main got-patching and transactional auditing:

```text
 [ main() / App ] ──► dl::dlopen(path) / load_semihost()
                         │
                         ▼
                      dynamic_linker::load_from_source(byte_source&)
                         │
                         ├───────────────────────────────┐
                         ▼                               ▼
                      elf_image::parse()               link_context::dynsym() / dynstr() / hash()
                         │                            (reads boot reloc table)
                         ▼
                      elf_image::image_base()
                         │
                         ▼ (heap image segment load)
                      image_memory::load_segments()
                         │
                         ├───────────────────────────────┐
                         ▼                               ▼
                      elf_image::collect_relocations()  elf_image::find_section()
                      (returns relocation vector)     (reads .dynsym, .dynstr)
                         │
                         ▼
                      relocator::compute(relocations, dynsym, dynstr)
                      (builds relocation plan vector)
                         │
                         ▼
                      dynamic_linker::apply_plan(plan, base_addr)
                         │
                         ▼ (loops plan entries)
                      patch_journal::record() / record_bytes()
                      (logs original pre-patch word for undo safety)
                         │
                         ▼ (updates GOT/PLT slot)
                      *patch_site = resolved_symbol_address
                         │
                         ▼
                      dynamic_linker::patch_main_got()
                         │
                         ▼ (loops main relocs)
                      patch_journal::record()  ──►  *main_got_site = library_symbol_address
                         │
                         ▼
                      dynamic_linker::run_constructors() (loops .init_array)
```

### 7.4 Static Unload Flow Chart

This diagram maps the bit-exact rollback process executed on `dlclose`:

```text
 [ main() / App ] ──► dl::dlclose(handle) / dynamic_linker::unload(lib)
                         │
                         ▼
                      patch_journal::revert()
                         │
                         ▼ (loops logged entries backwards)
                      Restores original values: *address = original_value
                         │
                         ▼ (prunes pending relocations)
                      Erase pending references
                         │
                         ▼
                      Release heap buffer (destroys image segments)
```

### 7.5 Overlay Load Flow Chart

This diagram maps static overlay switches in SRAM:

```text
 [ main() / App ] ──► scl_overlay_load(region, index)
                         │
                         ▼ (check residency)
                      scl_overlay_current(region)
                         │
                         ▼ (if not resident)
                      memcpy(VMA_destination, LMA_flash_source, size)
                         │
                         ▼ (Cortex-M memory/pipeline barriers)
                      DSB (Data Synchronization Barrier)
                         │
                         ▼
                      ISB (Instruction Synchronization Barrier)
```

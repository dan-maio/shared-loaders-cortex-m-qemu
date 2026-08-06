# SCL Codebase Analysis: Five Loader Functionalities

**Cortex-M Source Code Libraries (SCL)** — reusable shared-library loading infrastructure for Cortex-M microcontrollers (STM32F411CE (WeAct BlackPill)).

---

## 1. Architecture Overview

\newpage

### Layer Diagram

```
                    ┌─────────────────────────────────────┐
                    │          Application (firmware)      │
                    └─────────────────────────────────────┘
                                      │
                                      │ links one of:
                                      ▼
    ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
    │  scl::   │  │  scl::   │  │  scl::   │  │  scl::   │  │  scl::   │
    │  static  │  │ dynamic- │  │ dynamic- │  │ overlay  │  │  reloc   │
    │          │  │  nolib   │  │  newlib  │  │          │  │          │
    │ ET_DYN   │  │ ET_DYN   │  │ ET_DYN   │  │ Raw bin  │  │ ET_REL   │
    │ GOT patch│  │ dlopen   │  │ hosted   │  │ memcpy   │  │ ABS32    │
    └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘
         │             │             │             │             │
         │     ┌───────┴─────────────┴─────────────┴───────┐     │
         │     │            scl-xip-common                  │     │
         │     │  (segment-map, relocate, compact)          │     │
         │     └───────────────────┬───────────────────────┘     │
         │                        │                             │
         └────────────────────────┼─────────────────────────────┘
                                  │
                                  ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                         scl-common                              │
    │  ┌─────────────┐ ┌──────────────┐ ┌─────────────┐ ┌─────────┐ │
    │  │ dynamic_info │ │symbol_resolver│ │reloc_applier│ │lazy_res.│ │
    │  └─────────────┘ └──────────────┘ └─────────────┘ └─────────┘ │
    │  ┌─────────────┐ ┌──────────────┐ ┌─────────────┐ ┌─────────┐ │
    │  │region_alloc. │ │ image_memory │ │link_context │ │   log   │ │
    │  └─────────────┘ └──────────────┘ └─────────────┘ └─────────┘ │
    └─────────────────────────────────┬───────────────────────────────┘
                                      │
                                      ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                      common-elf-reader                          │
    │  ┌─────────────┐ ┌──────────────┐ ┌─────────────┐ ┌─────────┐ │
    │  │  elf_image   │ │ byte_source  │ │  elf_types  │ │ error/  │ │
    │  │              │ │              │ │             │ │ result  │ │
    │  └─────────────┘ └──────────────┘ └─────────────┘ └─────────┘ │
    └─────────────────────────────────────────────────────────────────┘
```

\newpage

### Loader Dependency Graph

```
    scl::static ──────────┐
    scl::dynamic-nolib ───┤
    scl::dynamic-newlib ──┼──▶ scl-common ──▶ common-elf-reader
    scl::overlay ─────────┤
    scl::reloc ───────────┘

    scl-xip-static ──────────▶ scl-xip-common + scl::static
    scl-xip-dynamic-nolib ───▶ scl-xip-common + scl::dynamic-nolib
    scl-xip-dynamic-newlib ──▶ scl-xip-common + scl::dynamic-newlib
```

### ELF Type vs Loader Matrix

```
                     ┌─────────────┬─────────────┬─────────────┐
                     │   ET_DYN    │   ET_REL    │  Raw binary │
                     │   (.so)     │    (.o)     │   (overlay) │
    ┌────────────────┼─────────────┼─────────────┼─────────────┤
    │ scl::static    │     ✓       │             │             │
    │ scl::dynamic-  │     ✓       │             │             │
    │        nolib   │             │             │             │
    │ scl::dynamic-  │     ✓       │             │             │
    │        newlib  │             │             │             │
    │ scl::overlay   │             │             │      ✓      │
    │ scl::reloc     │             │      ✓      │             │
    │ scl-xip-*      │     ✓       │             │             │
    └────────────────┴─────────────┴─────────────┴─────────────┘
```

All five loader flavors share:
- **common-elf-reader**: ELF32 parser (git submodule)
- **scl-common**: Symbol resolver, relocation applier, dynamic info parser, logging

Each loader is an **INTERFACE source-code library** — CMake never produces `.a` archives; sources propagate to each consuming target.

---

## 2. `scl::static` — Standalone ELF32 Static Patcher

### Purpose
Load PIC `.so` into RAM, patch GOT, run constructors. For hosted/newlib environments.

### Source Files

| Component | Files |
|-----------|-------|
| ELF adapter | `src/static/elf-reader.cpp`, `include/static/elf-reader.h` |
| Loader | `src/static/shlib-loader.cpp`, `include/static/shlib-loader.h` |
| Logger | `src/static/logger.cpp`, `include/static/logger.h` |

### Loading Pipeline

```
1. load_bin()                 → Copy PT_LOAD segments to RAM buffer
2. populate_helper_sections() → Locate .rel.dyn, .dynsym, .dynstr, .hash, .inits
3. find_*_symbols_to_patch()  → Build patch arrays
4. patch_shlib()              → Fix library GOT (RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32)
5. patch_main()               → Fix main's GOT with library symbols
6. execute_constructors()     → Run .init_array
```

### Key Data Structures

```cpp
struct symbol_patch {
    std::string_view name;
    uint32_t got_address;      // GOT slot to patch
    uint32_t virtual_address;  // Symbol's st_value (0 = external)
    uint8_t type;              // R_ARM_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32
    uint8_t binding;           // STB_GLOBAL, STB_WEAK, STB_LOCAL
};

section_map m_shlib;  // Library's helper sections
section_map m_main;   // Main's helper sections (from _reloc table)
```

### Main's `_reloc` Table

Main is a `.hex` (raw binary), not ELF. The linker script must emit a `_reloc` table so the loader can find main's dynamic sections:

```ld
_reloc = .;
LONG(LOADADDR(.rel.dyn));   LONG(SIZEOF(.rel.dyn));
LONG(LOADADDR(.dynsym));    LONG(SIZEOF(.dynsym));
LONG(LOADADDR(.dynstr));    LONG(SIZEOF(.dynstr));
LONG(LOADADDR(.hash));      LONG(SIZEOF(.hash));
LONG(LOADADDR(.inits));     LONG(SIZEOF(.inits));
```

| Entry | Purpose |
|-------|---------|
| `.rel.dyn` | Main's GOT entries needing patching when library exports symbols |
| `.dynsym` | Main's exported symbols (malloc, free) for library imports |
| `.dynstr` | String table for symbol name lookup |
| `.hash` | SysV hash table for O(1) symbol resolution |
| `.inits` | Constructor pointers (set but not called by loader) |

**Why needed**: Library `.so` is a real ELF (parsed at runtime). Main is `.hex` (raw binary, no ELF headers). The `_reloc` table is a 40-byte substitute for ELF headers that main lost during `objcopy`.

### `.rel.plt` Handling

`.rel.plt` (JUMP_SLOT entries for PLT stubs) is **not** in main's `_reloc` table because:
- Main is non-PIC — no PLT stubs, no JUMP_SLOT relocations
- `.rel.plt` exists only in the **library's** ELF headers
- The loader reads it from the library directly via `efr_.read_helper_sections(".rel.plt", hs)`

### Relocation Patching Logic

```cpp
// shlib-loader.cpp:374-386
if (s.binding == STB_GLOBAL) {
    if (s.virtual_address == 0) {
        // External symbol — lookup in main
        val = global_syms->find(s.name);
        if (!val) val = efr_.find_symbol_address(s.name, m_main);
    } else {
        // Self-defined — use library's own address
        val = s.virtual_address + uintptr_t(buffer);
    }
}
```

- `virtual_address` comes from `sym.st_value` in the library's `.dynsym`
- For symbols defined in the library: `st_value != 0` → library uses its own implementation
- For external symbols: `st_value == 0` → resolved from main or global symbol table

---

## 3. `scl::dynamic-nolib` — Freestanding dlopen Loader

### Purpose
Runtime dynamic loading without libc. For freestanding firmware (`-fpic -ffreestanding`).

### Source Files

| Component | Files |
|-----------|-------|
| Loader | `src/dynamic-nolib/dl.cpp`, `include/dynamic-nolib/dl.hpp` |
| PIC trampoline | `src/port/pic-trampoline.c` |
| Cross-call veneer | `src/port/dl-cross-call.c` |
| Lazy resolve stub | `src/port/lazy-resolve.c` |
| EXIDX unwind | `src/port/unwind-find-exidx.c` |
| Image memory | `src/image-memory.cpp`, `include/scl/image-memory.hpp` |
| Link context | `src/link-context.cpp`, `include/scl/link-context.hpp` |

### Loading Pipeline

```
1. dlopen_source(byte_source&) → Parse ELF, allocate arena slot
2. Copy PT_LOAD segments       → RAM arena
3. Apply relocations           → RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32
4. Register .ARM.exidx         → EHABI unwind tables
5. Run .init_array             → Constructors
```

### Key Data Structures

```cpp
struct handle {
    uint32_t base;        // Image base (load bias)
    uint32_t size;        // Allocated size
    uint32_t got_base;    // r10 value for entering library
    uint32_t got_off;     // .got offset
    loader::symbol_resolver resolver;
    uint32_t refcount;    // Cross-library references
    uint8_t deps[4];      // Dependencies
    loader::lazy_tables lazy;     // .rel.dyn JUMP_SLOT spans
    loader::lazy_tables lazy_plt; // .rel.plt JUMP_SLOT spans
    uint32_t exidx_base;  // .ARM.exidx address
    char name[12];        // Provenance name for tracing
};
```

### PIC Model

- Compiled with `-fpic -msingle-pic-base -mpic-register=r10`
- Each library has its own `got_base` (r10 value)
- Entering a library: `pic_call_void(fn, got_base)` sets r10 before branch

### Cross-Library Calls

Without cross-call enabled:
```
Library A → GOT[JUMP_SLOT] → Library B's function (B's r10)
```
Problem: A's r10 is active, not B's.

With `dl_enable_cross_call(true)`:
```
Library A → GOT[JUMP_SLOT] → dl_cross_call veneer → swaps r10 → Library B's function
```

### API

```cpp
// Core
loader::result<handle*> dlopen_source(loader::byte_source& src, const char* rawname);
loader::result<void*> dlsym(handle& h, const char* name);
void dlclose(handle& h);

// Cross-call control
void dl_enable_cross_call(bool on);
void dl_cross_trace(int level);  // 0=off, 1=cross-only, 2=verbose

// Diagnostics
void dl_dump_image(const handle& h);
void dl_dump_got_walkthrough(const handle& h);
```

---

## 4. `scl::dynamic-newlib` — Hosted/Newlib Loader

### Purpose
Dynamic loading with newlib heap, revertible GOT patching. For hosted CMSIS/HAL environments.

### Source Files

| Component | Files |
|-----------|-------|
| Linker | `src/dynamic-newlib/dynamic-linker.cpp`, `include/dynamic-newlib/dynamic-linker.hpp` |
| Patch journal | `src/dynamic-newlib/patch-journal.cpp`, `include/dynamic-newlib/patch-journal.hpp` |
| Image memory | `src/dynamic-newlib/image-memory.cpp`, `include/dynamic-newlib/image-memory.hpp` |
| Link context | `src/dynamic-newlib/link-context.cpp`, `include/dynamic-newlib/link-context.hpp` |

### Loading Pipeline

```
1. load_from_source(byte_source&) → Parse ELF
2. Copy PT_LOAD segments          → Heap-allocated memory
3. apply_plan()                   → Build relocation plan (std::vector)
4. patch_main_got()               → Patch firmware's GOT with library symbols
5. run_constructors()             → Execute .init_array
```

### Key Data Structures

```cpp
struct library {
    image_memory   image;         // Loaded ELF image in RAM
    patch_journal  journal;       // Patch journal for dlclose revert
    uint32_t base_va{};           // Base virtual address
    uint32_t dynsym_va{};         // .dynsym section VA
    uint32_t dynstr_va{};         // .dynstr section VA
    uint32_t hash_va{};           // .hash section VA
    void*    init_array{};        // .init_array in loaded RAM
    bool     constructed{false};
};

class dynamic_linker {
    const link_context& ctx_;                    // Firmware's _DYNAMIC accessor
    std::vector<std::unique_ptr<library>> libs_; // All loaded libraries
    std::vector<pending_reloc> pending_;         // Deferred cross-lib patches
};
```

### Patch Journal (Revert on Unload)

Every patched word is recorded (address + original value). `unload()` reverts all patches:

```cpp
void dynamic_linker::unload(library* lib) {
    // Revert every patched word to original value
    lib->journal.revert_all();
    // Free memory, remove from libs_ vector
}
```

This allows clean reload cycles — the second load sees pristine state.

### Symbol Resolution Order

1. Firmware's own symbols (via `_reloc` table)
2. Already-loaded libraries (cross-library search)
3. Known-symbol fallback (libc: printf, malloc, memcpy, etc.)

### API

```cpp
// Class API
dynamic_linker(const link_context& ctx);
loader::result<library*> load_from_source(loader::byte_source& src);
void unload(library* lib);
loader::result<void*> lookup(library* lib, const char* name);

// POSIX-style facade (dl-impl.hpp)
dl::handle h = dl::dlopen("/tmp/mylib1.so");
auto fn = (unsigned(*)(unsigned))dl::dlsym(h, "library_function");
dl::dlclose(h);
const char* err = dl::dlerror();
```

---

## 5. `scl::overlay` — Linker-Overlay Window Manager

### Purpose
Time-multiplex one RAM window between modules. Simplest loader — pure memcpy.

### Source Files

| Component | Files |
|-----------|-------|
| Manager | `src/overlay/overlay.c`, `include/overlay/overlay.h` |
| Linker template | `src/overlay/ldscripts/overlay-fragment.ld` |

### Data Structures

```c
typedef struct scl_overlay_module {
    void*       vma;   // Shared window run address (same for ALL modules)
    const void* lma;   // Flash load address of this module
    uint32_t    size;  // Module size in bytes
    const char* name;  // For tracing
} scl_overlay_module;

typedef struct scl_overlay_region {
    const scl_overlay_module* modules;  // Descriptor table
    int count;                          // Number of modules
    int resident;                       // Currently loaded index, or SCL_OVERLAY_NONE
} scl_overlay_region;
```

### API

```c
scl_overlay_init(&region, modules, count);  // Initialize descriptor table
scl_overlay_load(&region, index);           // memcpy flash→window + DSB/ISB
scl_overlay_current(&region);               // Returns resident index or SCL_OVERLAY_NONE
```

### Loading Implementation

```c
int scl_overlay_load(scl_overlay_region* r, int index) {
    if (r->resident == index) return 0;  // Already resident
    memcpy(modules[index].vma, modules[index].lma, modules[index].size);
    __asm__ volatile("dsb\nisb" ::: "memory");  // Drain write buffer, flush pipeline
    r->resident = index;
    return 0;
}
```

### Linker Script Requirements

The application's linker script must define an `OVERLAY : NOCROSSREFS` block:

```ld
MEMORY {
    OVL (rwx) : ORIGIN = 0x20018000, LENGTH = 16K
}

OVERLAY : NOCROSSREFS
{
    .ov1 { _ov1_lma = LOADADDR(.ov1); _ov1_size = SIZEOF(.ov1); *(.ov1) }
    .ov2 { _ov2_lma = LOADADDR(.ov2); _ov2_size = SIZEOF(.ov2); *(.ov2) }
    .ov3 { _ov3_lma = LOADADDR(.ov3); _ov3_size = SIZEOF(.ov3); *(.ov3) }
} > OVL AT> FLASH
```

### Key Constraints

- Modules must be compiled with `-mlong-calls -fno-pic` (no GOT, no PLT)
- All modules share the same VMA (window address)
- Only one module is valid at a time
- Modules must not reference each other (NOCROSSREFS)

---

## 6. `scl::reloc` — ET_REL Module Loader

### Purpose
Load relocatable `.o` files (kernel-module style). No GOT/PLT/r10.

### Source Files

| Component | Files |
|-----------|-------|
| Loader | `src/reloc/reloc-loader.cpp`, `include/reloc/reloc-loader.hpp` |

### Data Structures

```cpp
struct module {
    std::span<std::byte> arena;           // RAM sections placed in
    std::span<const std::byte> image;     // Original ET_REL bytes
    uint32_t sec_base[MAX_SECTIONS];      // Runtime base per section index
    uint16_t shnum;                       // Section header count
    uint16_t symtab_idx;                  // .symtab section index
    uint32_t used;                        // Bytes of arena consumed
    bool valid;
};

struct module_set {
    module mods[MAX_SET];  // Up to 4 modules
    int n;
};
```

### Loading Pipeline (Single Module)

```
1. load(image, arena, resolve) → Parse ET_REL ELF
2. Place SHF_ALLOC sections   → Arena (contiguous RAM)
3. Resolve .symtab             → Defined: section_base + st_value
                               → Undefined: call resolve()
4. Apply relocations           → R_ARM_ABS32, TARGET1, REL32, THM_CALL
5. Run .init_array             → Constructors
```

### Loading Pipeline (load_set — Cyclic Dependencies)

```
Phase 1: Place ALL modules' sections (now every symbol has an address)
Phase 2: Resolve ALL modules' relocations against the whole set first,
         then resolve() for anything no module defines
Phase 3: Run ALL constructors
```

### Relocation Types Handled

| Type | Application |
|------|-------------|
| `R_ARM_ABS32` | Absolute address (most common) |
| `R_ARM_TARGET1` | Same as ABS32 |
| `R_ARM_REL32` | PC-relative |
| `R_ARM_THM_CALL` | Thumb BL/BLX |

### Code Model

```
No GOT, no PLT, no _DYNAMIC, no r10
-fno-pic → fixed absolute addresses
-mlong-calls → removes BL-range limitation
External refs: ldr rN, =target ; blx rN
```

### API

```cpp
// Single module
loader::result<module> load(std::span<const std::byte> image,
                            std::span<std::byte> arena,
                            loader::symbol_source resolve);
std::uint32_t sym(const module& m, std::string_view name);

// Module set (cyclic dependencies)
loader::result<module_set> load_set(
    std::span<const std::span<const std::byte>> images,
    std::span<std::byte> arena,
    loader::symbol_source resolve);
std::uint32_t sym(const module_set& s, std::string_view name);
```

---

## 7. `scl-xip-*` — Execute-In-Place Adapters

### Purpose
Leave R-X segment in flash (executed in place), copy only RW segment to RAM.

### Source Files

| Component | Files |
|-----------|-------|
| Segment classifier | `src/xip/segment-map.cpp`, `include/xip/segment-map.hpp` |
| Relocator | `src/xip/relocate.cpp`, `include/xip/relocate.hpp` |
| Compact | `src/xip/compact.cpp`, `include/xip/compact.hpp` |
| Static adapter | `src/xip/static/adapter.cpp` |
| Dynamic-nolib adapter | `src/xip/dynamic-nolib/adapter.cpp` |
| Dynamic-newlib adapter | `src/xip/dynamic-newlib/adapter.cpp` |

### Two-Base Model

\newpage
```
┌─────────────────────────────────────────────────────────────┐
│                     Flash (R-X)                             │
│  .text   ─── executed in place, never copied                │
│  .rodata ─── read-only data, never copied                   │
└─────────────────────────────────────────────────────────────┘
        text_base = flash address + text_off

┌─────────────────────────────────────────────────────────────┐
│                     RAM (RW)                                │
│  .got    ─── patched at load time                           │
│  .data   ─── copied from flash                              │
│  .bss    ─── zeroed                                         │
└─────────────────────────────────────────────────────────────┘
        data_base = RAM address
```

### Segment Classification

```cpp
bool classify_segments(std::span<const std::byte> image, SegmentMap& out) {
    // Classify PT_LOAD segments:
    // R-X (exec, !write) → text segment (flash candidate)
    // RW (write)         → data segment (RAM)
    // Requires: VA == file_offset for both segments
}
```

### R-X Immutability Safety Scan

```cpp
bool text_is_immutable(std::span<const std::byte> image, const SegmentMap& seg) {
    // For each relocation entry:
    // - If target is in writable section → legitimate RW fixup (skip)
    // - If target is in R-X section → disqualifies XIP (return false)
    // Returns true if no relocations target R-X code/rodata
}
```

### XIP Relocation Apply

```cpp
loader::result<void> apply_rel_xip(const SegmentMap& seg,
                                   std::uintptr_t text_base,  // Flash
                                   std::uintptr_t data_base,  // RAM
                                   std::span<std::byte> ram_data,
                                   /* relocation tables */) {
    // Only RW words are patched — R-X stays read-only in flash
    // Symbols routed to correct base:
    //   in_text(st_value) ? text_base + offset : data_base + offset
}
```

### XIP Adapter Decision

```cpp
void* dlopen(std::span<const std::byte> so_image, const char* name) {
    SegmentMap seg;
    const bool can_xip = classify_segments(so_image, seg) && seg.split &&
                         text_is_immutable(so_image, seg);

    if (!can_xip) {
        // Fallback: copy everything to RAM (standard dynamic loading)
    } else {
        // XIP: copy only RW segment to RAM, text stays in flash
    }
}
```

### XIP Handle

```cpp
struct xip_handle {
    bool in_use, xip;
    char name[16];
    uintptr_t text_base;      // Flash address
    uint32_t text_vaddr, text_size;
    uintptr_t data_base;      // RAM address
    uintptr_t got_base;       // GOT base for r10
    uint32_t data_vaddr, data_memsz;
    uintptr_t flash_data_base; // Original flash data location
    loader::symbol_resolver resolver;
    std::span<const std::byte> reldyn, relplt, dynsym, dynstr;
    std::byte* ram;           // Allocated RW memory
};
```

---

## 8. Shared Components (scl-common)

| Component | Header | Purpose | Used By |
|-----------|--------|---------|---------|
| `dynamic_info` | `loader/dynamic-info.hpp` | Parse `_DYNAMIC` segment (DT_HASH, DT_SYMTAB, ...) | dynamic-nolib, dynamic-newlib |
| `symbol_resolver` | `loader/symbol-resolver.hpp` | SysV hash lookup over (.dynsym, .dynstr, .hash) | static, dynamic-nolib, dynamic-newlib, xip |
| `reloc_applier` | `loader/reloc-applier.hpp` | Allocation-free relocation application (freestanding) | dynamic-nolib, xip |
| `relocator` | `loader/relocator.hpp` | std::vector relocation plan (SCL_HOSTED only) | dynamic-newlib |
| `lazy_resolve` | `loader/lazy-resolve.hpp` | Lazy PLT binding fixup | dynamic-nolib |
| `region_allocator` | `loader/region-allocator.hpp` | Fixed-arena slot allocator | dynamic-nolib |
| `image_memory` | `scl/image-memory.hpp` | Heap or arena memory allocation | dynamic-nolib, dynamic-newlib |
| `link_context` | `scl/link-context.hpp` | `_DYNAMIC` / `_reloc` table parsing | dynamic-nolib, dynamic-newlib |
| `log` | `scl/log.h` | Centralized logging hook | All |
| `barrier` | `scl/barrier.h` | DSB/ISB memory barrier | static, overlay |
| `pic-trampoline` | `scl/pic-trampoline.h` | Enter library with r10 set | dynamic-nolib, xip |
| `unwind-exidx` | `scl/unwind-exidx.h` | EHABI .ARM.exidx registration | dynamic-nolib, xip |

---

## 9. Comparison Table

\newpage
| Feature | static | dynamic-nolib | dynamic-newlib | overlay | reloc |
|---------|--------|---------------|----------------|---------|-------|
| **ELF type** | ET_DYN (.so) | ET_DYN (.so) | ET_DYN (.so) | Raw binary | ET_REL (.o) |
| **Code model** | PIC | PIC (r10) | PIC | Non-PIC | Non-PIC |
| **Symbol table** | .dynsym/.hash | .dynsym/.hash | .dynsym/.hash | N/A | .symtab |
| **Relocations** | GOT patch | GOT patch | GOT patch | memcpy | ABS32/REL32 |
| **GOT/PLT** | Yes | Yes | Yes | No | No |
| **Heap required** | No | No | Yes (std::vector) | No | No |
| **Unloading** | No | Yes (refcount) | Yes (journal revert) | N/A | No |
| **Cross-lib** | Manual | r10 swap | Automatic | N/A | load_set() |
| **XIP support** | Via adapter | Via adapter | Via adapter | Native | No |
| **Float ABI** | Neutral | Hard (VFP asm) | Soft/Hard | Neutral | Neutral |
| **C++ support** | Full | Full (no RTTI/EH) | Full | N/A | Full |
| **DT_NEEDED** | Auto-load | Manual | Manual | N/A | N/A |

---

## 10. Build Configuration

### CMake Options

| Option | Default | Meaning |
|--------|---------|---------|
| `SCL_HOSTED` | ON on host, OFF on ARM | Build hosted parts (std::vector): relocator, dynamic-newlib |
| `SCL_BUILD_TESTS` | ON when top-level | Build host unit-test suite |
| `COMMON_ELF_READER_DIR` | `./common-elf-reader` | Path to ELF32 parser submodule |

### Toolchain Files

| File | Environment | Key Flags |
|------|-------------|-----------|
| `cmake/arm-none-eabi.cmake` | Freestanding firmware | Cortex-M4 hard-float, `-fpic`, `-ffreestanding` |
| `cmake/arm-none-eabi-hosted.cmake` | Hosted newlib firmware | Cortex-M4 soft-float, non-PIC, `-fno-exceptions` |

### Consumption Pattern

```cmake
add_subdirectory(shared-loaders)
target_link_libraries(app PRIVATE scl::static)        # or scl::dynamic-nolib, etc.
```

---

## 11. `-Bsymbolic` and Symbol Interposition

### What `-Bsymbolic` Does

With `-Bsymbolic`, library internal calls bind to the library's own definitions:

```bash
arm-none-eabi-gcc -shared -Wl,-Bsymbolic -o lib.so module.o
```

### In Bare-Metal Context

| Aspect | With `-Bsymbolic` | Without `-Bsymbolic` |
|--------|-------------------|---------------------|
| Internal calls | Direct (no GOT indirection) | Through GOT/PLT |
| Performance | Slightly faster | Slightly slower |
| GOT entries | Fewer to patch | More to patch |
| Interposition | Prevented | Allowed |

### Symbol Resolution Logic

```cpp
// In patch_shlib():
if (s.virtual_address == 0) {
    // External: lookup in main or global table
    val = global_syms->find(s.name);
    if (!val) val = efr_.find_symbol_address(s.name, m_main);
} else {
    // Self-defined: use library's own address
    val = s.virtual_address + uintptr_t(buffer);
}
```

- `virtual_address` = `sym.st_value` from library's `.dynsym`
- Library defines `malloc`: `st_value != 0` → library uses its own
- Library doesn't define `malloc`: `st_value == 0` → resolved from main

---

## 12. Test Applications

| App | Loader | What It Tests |
|-----|--------|---------------|
| `scl-fw-dyn1` | dynamic-nolib | Demo: dlopen/dlsym/call/dlclose, C++, EH across boundary |
| `scl-fw-dyn2` | dynamic-nolib | 16-test cross-module battery (c1/c2/cpp3, cross-lib calls) |
| `scl-fw-ovl1` | overlay + dynamic-nolib | Overlays-vs-dynamic comparison (6 tests) |
| `scl-fw-flatfs` | dynamic-nolib | Storage-backend proof (flash archive) |
| `scl-fw-dynamic-newlib1` | dynamic-newlib | Static loader battery (journal revert proof) |
| `scl-fw-dynamic-newlib2` | dynamic-newlib | Full static-loader suite |
| `f411ce-sh` | static | Static patcher demo (libsh1/2, cpp_features) |
| `test-relocatable-*` | reloc | ET_REL module loader (9-check battery) |
| `test-xip-*` | xip-* | XIP adapters (static, dynamic-nolib, dynamic-newlib) |

---

## 13. File Layout

\newpage
```
shared-loaders/
├── CMakeLists.txt                    # SCL library package (targets + aliases)
├── common-elf-reader/                # ELF32 parser (git submodule)
│   ├── include/loader/               #   elf-image.hpp, byte-source.hpp, elf-types.hpp
│   └── src/elf-image.cpp
├── include/
│   ├── loader/                       # Common core headers
│   │   ├── dynamic-info.hpp          #   _DYNAMIC parsing
│   │   ├── symbol-resolver.hpp       #   SysV hash lookup
│   │   ├── reloc-applier.hpp         #   Allocation-free relocation
│   │   ├── relocator.hpp             #   std::vector relocation plan
│   │   ├── lazy-resolve.hpp          #   Lazy PLT binding
│   │   └── region-allocator.hpp      #   Fixed-arena allocator
│   ├── scl/                          # Firmware-level headers
│   │   ├── image-memory.hpp          #   Memory allocation
│   │   ├── link-context.hpp          #   _DYNAMIC/_reloc parsing
│   │   ├── log.h                     #   Logging hook
│   │   ├── barrier.h                 #   DSB/ISB
│   │   ├── pic-trampoline.h          #   r10 trampoline
│   │   └── unwind-exidx.h            #   EHABI registration
│   ├── static/                       # Static patcher API
│   ├── dynamic-nolib/                # dlopen loader API
│   ├── dynamic-newlib/               # Hosted loader API
│   ├── overlay/                      # Overlay manager API
│   ├── reloc/                        # ET_REL loader API
│   └── xip/                          # XIP adapter API
├── src/
│   ├── static/                       # Static patcher implementation
│   ├── dynamic-nolib/                # dlopen implementation
│   ├── dynamic-newlib/               # Hosted implementation
│   ├── overlay/                      # Overlay implementation
│   ├── reloc/                        # ET_REL implementation
│   ├── xip/                          # XIP adapter implementations
│   │   ├── adapter-base.hpp          #   Shared template helpers (dlsym, get_got_base, etc.)
│   │   └── xip-cross-call.c          #   Shared cross-library call trampoline
│   ├── port/                         # Cortex-M assembly (VFP, trampolines)
│   ├── dynamic-info.cpp              # Shared core: _DYNAMIC parsing
│   ├── symbol-resolver.cpp           # Shared core: SysV hash lookup
│   ├── reloc-applier.cpp             # Shared core: allocation-free relocation
│   ├── relocator.cpp                 # Shared core: std::vector relocation plan (SCL_HOSTED)
│   ├── lazy-resolve.cpp              # Shared core: lazy PLT binding
│   ├── region-allocator.cpp          # Shared core: fixed-arena allocator
│   ├── image-memory.cpp              # Shared core: arena-backed image memory
│   ├── link-context.cpp              # Shared core: _DYNAMIC self-parsing
│   ├── log.c                         # Shared core: weak no-op logging
│   └── scl-log-put-hex.c             # Shared core: hex printing helper
├── test/                             # Shared test infrastructure
│   ├── platforms/                    # Board platform libraries
│   ├── tools/                        # flash-run.sh, flash-run-sf.sh, flash-run-hf.sh
│   ├── common/                       # Shared test support headers/sources
│   └── fixtures/                     # Committed prebuilt ARM .so fixtures
├── test-dynamic/                     # Dynamic loader test tree (8 targets)
├── test-overlay/                     # Overlay test tree (1 target)
├── test-static/                      # Static patcher test tree (4 targets)
├── test-relocatable/                 # ET_REL test tree (4 targets) — TOP LEVEL
└── test-xip/                         # XIP test tree (8 targets)
```

---

*Generated from SCL codebase analysis — Cortex-M Source Code Libraries*

---

## 14. ELF Reader Deep Dive (common-elf-reader)

### Architecture

The `common-elf-reader` is a **git submodule** providing a standalone ELF32 parser shared by all loader flavors. It follows the "source code library" pattern — never produces a `.a` archive; sources propagate to each consumer.

```
common-elf-reader/
├── include/loader/
│   ├── elf-image.hpp          # Main parser class
│   ├── byte-source.hpp        # Abstract I/O interface
│   ├── elf-types.hpp          # ARM ELF32-LE structures
│   ├── error.hpp              # result<T> type
│   ├── detail/
│   │   ├── bytes.hpp          # Bounds-checked byte accessors
│   │   └── elf-hash.hpp       # SysV hash implementation
│   └── sources/
│       ├── fd-byte-source.hpp # File descriptor backend
│       └── memory-byte-source.hpp  # In-memory backend
└── src/
    └── elf-image.cpp          # Parser implementation
```

### Core Classes

#### `byte_source` — Storage Abstraction

```cpp
class byte_source {
public:
    virtual ~byte_source() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};
```

The loader **never assumes** how bytes are stored. Backends are app-provided:

| Backend | Source | Use Case |
|---------|--------|----------|
| `fd_byte_source` | `sources/fd-byte-source.hpp` | Semihosting file I/O |
| `memory_byte_source` | `sources/memory-byte-source.hpp` | In-memory buffer (host tests, flash blobs) |
| `Flashbyte_source` | `test/.../flash-byte-source.hpp` | Memory-mapped flash (flat-fs) |
| `semihost_file` | `test/.../semihost-file.cpp` | Cortex-M semihosting |

#### `elf_image` — Validated ELF32 View

```cpp
class elf_image {
public:
    static result<elf_image> parse(byte_source& src) noexcept;

    const elf::Ehdr& header() const noexcept;
    result<void> program_header(elf::half index, elf::Phdr& out) const noexcept;
    result<void> section_header(elf::half index, elf::Shdr& out) const noexcept;
    result<elf::Shdr> find_section(std::string_view name) const noexcept;
    result<uint32_t> image_size() const noexcept;
    result<uint32_t> image_base() const noexcept;

#if defined(SCL_HOSTED)
    result<std::vector<std::byte>> collect_relocations() const noexcept;
#endif

    byte_source& source() const noexcept;
};
```

**Validation on parse:**
- Magic: `0x7f 'E' 'L' 'F'`
- Class: `ELFCLASS32` (32-bit)
- Data: `ELFDATA2LSB` (little-endian)
- Machine: `EM_ARM` (40)
- Type: `ET_DYN` (3) for shared objects

#### `result<T>` — C++20 Error Handling

```cpp
template <class T>
class result {
    std::optional<T> val_{};
    error err_ = error::OK;
public:
    bool has_value() const noexcept;
    T& operator*() noexcept;
    error err() const noexcept;
    T value_or(U&& fallback) const;
};
```

Error codes:
```cpp
enum class error {
    OK, FILE_NOT_FOUND, SHORT_READ, NOT_ELF, NOT_ARM32,
    NOT_SHARED_OBJECT, BAD_SECTION, SYMBOL_NOT_FOUND,
    UNSUPPORTED_RELOC, OUT_OF_MEMORY
};
```

### ELF32 Structures (elf-types.hpp)

```cpp
struct Ehdr {        // 52 bytes
    unsigned char e_ident[16];
    half e_type;     // ET_DYN expected
    half e_machine;  // EM_ARM expected
    addr e_entry;    // 0 for libraries
    off  e_phoff;    // Program header table offset
    off  e_shoff;    // Section header table offset
    half e_phnum;    // Program header count
    half e_shnum;    // Section header count
    half e_shstrndx; // Section string table index
};

struct Phdr {        // 32 bytes
    word p_type;     // PT_LOAD
    off  p_offset;   // File offset
    addr p_vaddr;    // Virtual address
    word p_filesz;   // File size
    word p_memsz;    // Memory size (>= filesz for BSS)
    word p_flags;    // R/W/X flags
};

struct Sym {         // 16 bytes
    word st_name;    // String table index
    addr st_value;   // Symbol value (0 = external)
    word st_size;    // Symbol size
    unsigned char st_info;  // Binding + type
};

struct Rel {         // 8 bytes
    addr r_offset;   // Location to patch
    word r_info;     // Symbol index + type
};
```

### Bounds-Checked Byte Accessors (detail/bytes.hpp)

```cpp
// Header-only, never scans past table boundaries
load_word(span, offset)    // Read 4 bytes LE
load_byte(span, offset)    // Read 1 byte
store_word(span, offset, val)  // Write 4 bytes LE
name_at(strtab, size, idx)     // NUL-terminated string read
```

### SysV Hash Lookup (detail/elf-hash.hpp)

```cpp
inline uint32_t elf_hash(std::string_view name) noexcept {
    uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
```

Used by `symbol_resolver` for O(1) lookup in `.hash` table.

---

## 15. How Libraries Arrive on Cortex-M

### Storage Backends

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Library Storage Options                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                   │
│  │ Semihosting  │    │  Flash FS    │    │  Raw Flash   │                   │
│  │              │    │  (flat-fs)   │    │  (linked-in) │                   │
│  │ Host PC file │    │ On-chip flash│    │  Fixed addr  │                   │
│  │ via debug IF │    │ sectors 6-7  │    │              │                   │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘                   │
│         │                   │                   │                           │
│         ▼                   ▼                   ▼                           │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                   │
│  │semihost_file │    │Flashbyte_src │    │memory_byte_  │                   │
│  │(fd-based)    │    │(memcpy)      │    │source        │                   │
│  └──────────────┘    └──────────────┘    └──────────────┘                   │
│         │                   │                   │                           │
│         └───────────────────┴───────────────────┘                           │
│                             │                                               │
│                             ▼                                               │
│                      ┌──────────────┐                                       │
│                      │ byte_source  │  ← Abstract interface                │
│                      │ read_at()    │                                       │
│                      └──────────────┘                                       │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1. Semihosting (Debug/Development)

Semihosting allows the Cortex-M target to access host PC files through the debug interface (CMSIS-DAP/JTAG).

```cpp
// test/platforms/board/src/semihost-file.cpp
class semihost_file final : public loader::byte_source {
    int handle_;
    std::size_t size_;
public:
    static result<semihost_file> open(const char* path) noexcept {
        int h = semihost_open(path, 1);  // mode 1 == "rb"
        long n = semihost_flen(h);
        return semihost_file(h, n);
    }

    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept {
        semihost_seek(handle_, offset);
        semihost_read(handle_, out.data(), out.size());
    }
};
```

**Flow:**
```
Firmware calls semihost_open("/tmp/mylib1.so")
    → Debug probe (CMSIS-DAP) intercepts
    → Opens file on host PC
    → Returns file handle to firmware

Firmware calls semihost_read(handle, buf, size)
    → Debug probe reads from host file
    → Transfers bytes to target RAM
    → Returns data to firmware
```

**Use case:** Development, testing, rapid iteration.

### 2. On-Chip Flash Filesystem (flat-fs)

Libraries stored in flash sectors 6-7 as raw blobs. Memory-mapped at `0x08060000`.

```cpp
// test-dynamic/test-dynamic-nolib/test3/include/flash-byte-source.hpp
class Flashbyte_source final : public loader::byte_source {
    const std::byte* base_;  // Flash address
    std::size_t size_;
public:
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept {
        std::memcpy(out.data(), base_ + offset, out.size());  // Memory-mapped flash
    }
};
```

**Flow:**
```
First boot: Provision flash
    → Read .so files via semihosting
    → Program into flash sectors 6-7
    → Write TOC (table of contents) with CRC

Subsequent boots:
    → Check flash magic/version
    → If valid: use memory-mapped blobs directly
    → No semihosting needed at runtime
```

**Archive format:**
```cpp
struct fs_superblock {
    uint32_t magic;      // 0x46534C41 ("FSLA")
    uint32_t version;
    uint32_t entry_count;
    uint32_t crc;
};

struct fs_entry {
    char name[16];       // "c1.so"
    uint32_t offset;     // Flash offset
    uint32_t size;       // Blob size
    uint32_t crc;        // Per-blob CRC
};
```

**Use case:** Production, no debug probe needed.

### 3. XIP from Flash (Execute-In-Place)

Code stays in flash, only RW data copied to RAM.

```
Flash (R-X):
    0x08060000 + text_off → library .text (executed in place)

RAM (RW):
    Allocated by loader → library .got/.data/.bss (copied + patched)
```

**Requirements:**
- Library must have split `R-X | RW` segments
- R-X segment must have no relocations
- Flash must be memory-mapped (normal for STM32)
- Storage must preserve alignment and contiguity

---

## 16. Interrupt Handling in Loaded Modules

### The r10 Problem

In PIC code compiled with `-msingle-pic-base -mpic-register=r10`, all global/static variable access goes through `r10` (GOT base). When an interrupt fires:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Interrupt Entry Conflict                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. CPU executing library code (r10 = library's GOT base)                   │
│  2. Hardware interrupt fires                                                │
│  3. CPU auto-pushes {r0-r3, r12, lr, pc, xPSR} to MSP                      │
│  4. CPU branches to ISR (r10 UNCHANGED — hardware doesn't touch it)         │
│  5. ISR code tries to access globals using r10                              │
│  6. r10 points to library's GOT, not ISR's GOT                              │
│  7. → UsageFault / BusFault / HardFault                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Solution Patterns

#### Pattern A: Main Firmware Trampoline (Recommended for dynamic-*)

```c
// Main firmware vector table
extern void* g_fixture_got_base;         // Library's got_base
extern void (*g_fixture_isr_handler)(void); // Library's ISR

void USART1_IRQHandler(void) {
    if (g_fixture_isr_handler && g_fixture_got_base) {
        __asm__ volatile (
            "mov r4, r10\n"           // Save current r10
            "mov r10, %0\n"           // Load library's GOT base
            "blx %1\n"                // Call library's ISR
            "mov r10, r4\n"           // Restore r10
            :
            : "r"(g_fixture_got_base), "r"(g_fixture_isr_handler)
            : "r4", "lr", "memory"
        );
    }
}
```

**Pros:** Zero ISR latency, simple.
**Cons:** Main must know about library's ISR.

#### Pattern B: Relocatable Objects (scl::reloc — No r10)

```c
// ET_REL modules: -fno-pic -mlong-calls -r
// No GOT, no r10 dependency — absolute addresses only

void register_dynamic_irq(IRQn_Type irq, void (*isr)(void)) {
    uint32_t* ram_vectors = (uint32_t*)SCB->VTOR;
    ram_vectors[irq + 16] = (uint32_t)isr;  // Direct absolute address
    NVIC_EnableIRQ(irq);
}
```

**Pros:** Zero overhead, direct vector table patching.
**Cons:** No PIC, no GOT, module must be relocatable.

#### Pattern C: Module-Side Assembly Wrapper

```assembly
// Inside the .so, naked assembly wrapper
my_module_isr_wrapper:
    push {r4, lr}
    ldr r4, =_GLOBAL_OFFSET_TABLE_  // Load GOT base
    mov r10, r4                      // Set r10
    bl my_module_isr_c_handler       // Call C handler
    pop {r4, pc}
```

**Pros:** Module self-contained.
**Cons:** Extra wrapper overhead.

#### Pattern D: RAM Vector Table + Trampoline (Recommended for XIP)

```c
__attribute__((aligned(512))) static uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
    uint32_t* flash_vectors = (uint32_t*)SCB->VTOR;
    for (int i = 0; i < 128; i++)
        g_ram_vector_table[i] = flash_vectors[i];
    SCB->VTOR = (uint32_t)g_ram_vector_table;
    __DSB(); __ISB();
}

void register_tim2_isr(void (*isr)(void), uint32_t got_base) {
    g_ram_vector_table[TIM2_IRQn + 16] = (uint32_t)trampoline;
    g_fixture_got_base = got_base;
    g_fixture_isr_handler = isr;
    NVIC_EnableIRQ(TIM2_IRQn);
}
```

**Pros:** Dynamic registration, works with XIP.
**Cons:** RAM vector table uses 512 bytes.

### Loader-Specific Recommendations

| Loader | Recommended Pattern | Notes |
|--------|---------------------|-------|
| `scl::dynamic-nolib` | A (Trampoline) | r10 swap already in cross-call |
| `scl::dynamic-newlib` | A (Trampoline) | Hosted environment |
| `scl::static` | A or D | Static loader + trampoline |
| `scl::reloc` | B (Direct) | No r10 dependency |
| `scl-xip-*` | D (RAM vectors) | XIP needs dynamic registration |

---

## 17. Deep Dive: Dynamic Loading (scl::dynamic-nolib)

### Full Loading Pipeline

```
dlopen_source(byte_source& src, const char* rawname)
    │
    ├── 1. Parse ELF headers
    │       elf_image::parse(src)
    │       Validate: magic, class, machine, type
    │
    ├── 2. Calculate image size
    │       image_size() = sum of PT_LOAD memsz
    │
    ├── 3. Allocate arena slot
    │       image_arena().alloc(total_size)
    │       Returns RAM buffer pointer
    │
    ├── 4. Copy PT_LOAD segments
    │       For each PT_LOAD:
    │           memcpy(ram + (ph.p_vaddr - min_va), src + ph.p_offset, ph.p_filesz)
    │           memset remaining to 0 (BSS)
    │
    ├── 5. Snapshot GOT (pre-relocation)
    │       Copy .got contents to handle.got_raw[]
    │       For didactic walkthrough only
    │
    ├── 6. Parse _DYNAMIC segment
    │       dynamic_info::parse() → DT_HASH, DT_SYMTAB, DT_STRTAB
    │
    ├── 7. Build symbol resolver
    │       symbol_resolver(dynsym, dynstr, hash)
    │       SysV hash lookup for O(1) resolution
    │
    ├── 8. Apply relocations
    │       For each .rel.dyn + .rel.plt entry:
    │           RELATIVE: *P += base
    │           GLOB_DAT: *P = resolve(name)
    │           JUMP_SLOT: *P = resolve(name) or lazy_stub
    │           ABS32: *P = S + A
    │
    ├── 9. Register .ARM.exidx
    │       dl_exidx_register(start, end, base, 0)
    │       For EHABI exception unwinding
    │
    ├── 10. Run .init_array constructors
    │        For each function pointer in .init_array:
    │            pic_call_void(fn, got_base)
    │
    └── 11. Return handle*
            Contains: base, size, got_base, resolver, deps, lazy tables
```

### PIC Call Mechanism

```cpp
// include/scl/pic-trampoline.h
void pic_call_void(void* fn, void* got_base);
uint32_t pic_call_u32(void* fn, void* got_base, uint32_t arg0);
```

Assembly implementation:
```asm
pic_call_void:
    push {r4, lr}
    mov r4, r10          // Save current r10
    mov r10, r1          // Set library's GOT base
    blx r0               // Call function
    mov r10, r4          // Restore r10
    pop {r4, pc}
```

### Cross-Library Call Routing

```
Library A calls Library B's function:

Without cross-call:
    A's code → GOT[JUMP_SLOT] → B's function
    Problem: r10 = A's GOT, but B needs its own GOT

With dl_enable_cross_call(true):
    A's code → GOT[JUMP_SLOT] → dl_cross_call veneer
                                    │
                                    ├── Save A's r10
                                    ├── Load B's r10
                                    ├── Call B's function
                                    ├── Restore A's r10
                                    └── Return to A
```

### Lazy Binding

```
First call to foo@plt:
    GOT[foo] → lazy_resolve_stub
        │
        ├── Look up "foo" in .dynsym/.dynstr
        ├── Resolve address
        ├── Patch GOT[foo] with resolved address
        └── Jump to foo

Subsequent calls:
    GOT[foo] → foo's address (direct jump)
```

---

## 18. Deep Dive: Static Loading (scl::static)

### The _reloc Table Contract

Main is a `.hex` (raw binary, no ELF headers). The `_reloc` table tells the loader where main's sections are:

```ld
/* sections-sh.ld */
.inits : ALIGN(4)
{
    _reloc = .;
    LONG(LOADADDR(.rel.dyn));   LONG(SIZEOF(.rel.dyn));
    LONG(LOADADDR(.dynsym));    LONG(SIZEOF(.dynsym));
    LONG(LOADADDR(.dynstr));    LONG(SIZEOF(.dynstr));
    LONG(LOADADDR(.hash));      LONG(SIZEOF(.hash));
    LONG(LOADADDR(.inits));     LONG(SIZEOF(.inits));
} >FLASH
```

Parsed by `link_context`:

```cpp
struct init_lds {
    uint32_t reldyn, reldyn_size;
    uint32_t dynsym, dynsym_size;
    uint32_t dynstr, dynstr_size;
    uint32_t hash, hash_size;
    uint32_t inits, inits_size;
};

auto* init = reinterpret_cast<const init_lds*>(&_reloc);
```

### Symbol Resolution Logic

```cpp
// patch_shlib() — library's GOT
if (s.virtual_address == 0) {
    // External: lookup in global table, then main
    val = global_syms->find(s.name);
    if (!val) val = efr_.find_symbol_address(s.name, m_main);
} else {
    // Self-defined: library's own address
    val = s.virtual_address + uintptr_t(buffer);
}

// patch_main() — main's GOT
auto resolve = [&](std::string_view name) -> uint32_t {
    uint32_t a = global_syms->find(name);      // Check loaded libs
    if (!a) {
        a = efr_.find_symbol_address(name, m_shlib);  // Check library
        if (a) a += uintptr_t(buffer);          // Add load bias
    }
    return a;
};
```

### Relocation Types Handled

| Type | Code | Application |
|------|------|-------------|
| `R_ARM_RELATIVE` | 23 | `*P += base` (local data) |
| `R_ARM_GLOB_DAT` | 21 | `*P = sym_addr` (global GOT slot) |
| `R_ARM_JUMP_SLOT` | 22 | `*P = sym_addr` (PLT entry) |
| `R_ARM_ABS32` | 2 | `*P = S + A` (absolute address) |
| `R_ARM_COPY` | 20 | `memcpy(P, sym, size)` (data copy) |

### DT_NEEDED Auto-Loading

```cpp
// shlib-loader.cpp:274-295
loader_error shlib_loader::resolve_dependencies(
    symbol_table* syms,
    loader_error (*load_dep)(std::string_view, shlib_loader&)) {
    for (const auto& dep : dependencies_) {
        if (syms->has(dep)) continue;  // Already loaded
        auto holder = std::make_unique<shlib_loader>();
        load_dep(dep, *holder);         // Callback to app
        loaded_deps_.push_back(std::move(holder));
    }
}
```

---

## 19. Deep Dive: Relocatable Loading (scl::reloc)

### ET_REL vs ET_DYN

| Aspect | ET_DYN (.so) | ET_REL (.o) |
|--------|--------------|-------------|
| Code model | PIC (`-fpic`) | Non-PIC (`-fno-pic`) |
| Symbol table | `.dynsym` + `.hash` | `.symtab` + `.strtab` |
| Relocations | GOT/PLT patching | Absolute patching |
| `_DYNAMIC` | Yes | No |
| GOT/PLT | Yes | No |
| r10 register | Used (GOT base) | Not used |
| External calls | `bl sym@PLT` | `ldr r0, =sym; blx r0` |

### Loading Pipeline (Single Module)

```
load(image, arena, resolve)
    │
    ├── 1. Parse ELF headers
    │       Verify: ET_REL, EM_ARM, ELF32
    │
    ├── 2. Read section headers
    │       For each section:
    │           Record sh_addr, sh_size, sh_flags, sh_type
    │
    ├── 3. Place SHF_ALLOC sections in arena
    │       For each section with SHF_ALLOC:
    │           Copy section contents to arena
    │           Record sec_base[section_index] = arena_offset
    │
    ├── 4. Resolve .symtab
    │       For each symbol in .symtab:
    │           If defined (st_shndx != SHN_UNDEF):
    │               addr = sec_base[st_shndx] + st_value
    │           If undefined:
    │               addr = resolve(name)  // External lookup
    │
    ├── 5. Apply relocations
    │       For each .rel section:
    │           For each Rel entry:
    │               P = sec_base[section] + r_offset
    │               S = resolved symbol address
    │               A = current value at P
    │               Apply based on type:
    │                   ABS32: *P = S + A
    │                   REL32: *P = S + A - P
    │                   THM_CALL: patch BL/BLX target
    │
    └── 6. Run .init_array
            For each constructor:
                Call function
```

### load_set() for Cyclic Dependencies

```
load_set(images[], arena, resolve)
    │
    ├── Phase 1: PLACE ALL
    │       For each module:
    │           Place SHF_ALLOC sections in arena
    │           Now every symbol has an address
    │
    ├── Phase 2: RESOLVE ALL
    │       For each module:
    │           Resolve against whole set first
    │           Then against external resolve()
    │
    └── Phase 3: CONSTRUCT ALL
            For each module:
                Run .init_array
```

This allows **mutual/cyclic imports**:
```
Module A: extern int b_var;
Module B: extern int a_var;

load_set({A, B}, arena, resolve)
    Phase 1: Place both → a_var and b_var have addresses
    Phase 2: A's b_var resolved from B, B's a_var resolved from A
```

---

## 20. Deep Dive: XIP Execution

### Two-Base Model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           XIP Memory Layout                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Flash (R-X)                          RAM (RW)                              │
│  ┌─────────────────────┐              ┌─────────────────────┐               │
│  │ .text               │              │ .got                │               │
│  │   library code      │              │   patched at load   │               │
│  │   executed in place │              │                     │               │
│  │                     │              │ .data               │               │
│  │ .rodata             │              │   copied from flash │               │
│  │   constants         │              │                     │               │
│  │   vtables           │              │ .bss                │               │
│  │                     │              │   zeroed            │               │
│  └─────────────────────┘              └─────────────────────┘               │
│          │                                      │                           │
│          │ text_base                             │ data_base (= got_base)   │
│          │ = flash_addr + text_off               │ = ram_addr               │
│          │                                      │                           │
│          └────────────┬─────────────────────────┘                           │
│                       │                                                     │
│                       ▼                                                     │
│              Symbol Resolution                                              │
│              in_text(vaddr) ? text_base : data_base                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Segment Classification

```cpp
// segment-map.cpp
bool classify_segments(std::span<const std::byte> image, SegmentMap& out) {
    for (each PT_LOAD segment) {
        if (exec && !write)  → text segment (R-X)
        if (write)           → data segment (RW)
    }
    // Requires: VA == file_offset for both
    // (PC-relative GOT calculations must match)
    out.split = have_text && have_data &&
                text_vaddr == text_off &&
                data_vaddr == data_off;
}
```

### R-X Immutability Scan

```cpp
// relocate.cpp
bool text_is_immutable(std::span<const std::byte> image, const SegmentMap& seg) {
    // Collect writable section ranges
    // For each relocation:
    //   If target in writable section → OK (legit RW fixup)
    //   If target in R-X section → FAIL (disqualifies XIP)
    return true;  // No relocations target R-X
}
```

### XIP Relocation Apply

```cpp
// relocate.cpp
loader::result<void> apply_rel_xip(const SegmentMap& seg,
                                   uintptr_t text_base,
                                   uintptr_t data_base,
                                   std::span<std::byte> ram_data,
                                   /* relocation tables */) {
    // Only RW words are patched — R-X stays read-only in flash
    auto sym_target = [&](uint32_t st_value, uint32_t st_name) -> uint32_t {
        if (st_value != 0)
            return seg.in_text(st_value) ? text_base + (st_value - seg.text_vaddr)
                                         : data_base + (st_value - seg.data_vaddr);
        return resolve(name);  // External symbol
    };

    for (each relocation) {
        if (r_offset not in RW range) → ERROR
        switch (type) {
            RELATIVE: *P += in_text(addend) ? text_bias : data_bias
            GLOB_DAT: *P = sym_target()
            JUMP_SLOT: *P = sym_target()
            ABS32: *P += sym_target()
            REL32: *P += sym_target() - P
        }
    }
}
```

### XIP Decision Flow

```
dlopen(so_image)
    │
    ├── classify_segments() → can_xip?
    │       ├── Split R-X | RW? (not fused RWX)
    │       └── VA == file_offset?
    │
    ├── text_is_immutable() → safe?
    │       └── No relocations target R-X
    │
    ├── if (!can_xip || !safe):
    │       Fallback: copy everything to RAM
    │       (standard dynamic loading)
    │
    └── if (can_xip && safe):
            XIP load:
            ├── Allocate RAM for RW segment only
            ├── Copy RW from flash to RAM
            ├── Apply relocations to RAM only
            ├── text_base = flash_addr + text_off
            ├── data_base = ram_addr
            └── Execute .text from flash
```

### XIP RAM Savings

| Module | Full RAM | XIP RAM | Savings |
|--------|----------|---------|---------|
| Code-heavy (10KB .text, 500B .data) | 10.5 KB | 500 B | 95% |
| Balanced (5KB .text, 2KB .data) | 7 KB | 2 KB | 71% |
| Data-heavy (1KB .text, 8KB .data) | 9 KB | 8 KB | 11% |

### XIP Constraints

1. **Segment layout**: Must have split `R-X | RW` (not fused `RWX`)
2. **No R-X relocations**: All fixups must target RW sections
3. **Storage alignment**: Flash must preserve segment alignment
4. **No compression**: R-X segment must be stored raw/contiguous
5. **Memory-mapped flash**: CPU must be able to execute from flash address

---

## 21. Shared Component Deep Dive

### `reloc_applier` — Allocation-Free Relocation

```cpp
// Freestanding firmware (no std::vector)
class reloc_applier {
public:
    static result<void> apply_rel(
        std::span<std::byte> image,  // Loaded image
        uint32_t base,               // Runtime base address
        std::span<const std::byte> reldyn,  // .rel.dyn contents
        std::span<const std::byte> dynsym,  // .dynsym contents
        std::span<const std::byte> dynstr,  // .dynstr contents
        symbol_source resolve) noexcept;    // External resolver
};
```

Used by `scl::dynamic-nolib` and XIP adapters (freestanding, no heap).

### `relocator` — std::vector Relocation Plan

```cpp
// Hosted firmware (SCL_HOSTED=1)
struct reloc_entry {
    uint32_t offset;
    uint32_t info;
    uint32_t addend;
};

struct relocation_plan {
    std::vector<reloc_entry> entries;
};
```

Used by `scl::dynamic-newlib` and host tests (needs heap).

### `symbol_resolver` — SysV Hash Lookup

```cpp
class symbol_resolver {
    std::span<const std::byte> dynsym_;
    std::span<const std::byte> dynstr_;
    std::span<const std::byte> hash_;
public:
    result<uint32_t> address_of(std::string_view name) const noexcept;
};
```

**Hash table format:**
```
[nbucket] [nchain]
[bucket[0]] [bucket[1]] ... [bucket[nbucket-1]]
[chain[0]] [chain[1]] ... [chain[nchain-1]]
```

Lookup: `bucket[elf_hash(name) % nbucket]` → chain walk until match.

### `region_allocator` — Fixed-Arena Slot Allocator

```cpp
class region_allocator {
    std::byte* base_;
    std::size_t size_;
    std::size_t used_;
public:
    result<std::byte*> alloc(std::size_t size);
    void free(std::byte* ptr);
};
```

Used by `scl::dynamic-nolib` for image arena management.

### `patch_journal` — Revert on Unload

```cpp
class patch_journal {
    struct entry { uint32_t* addr; uint32_t orig; };
    std::vector<entry> entries_;
public:
    void record(uint32_t* addr, uint32_t orig);
    void revert_all();
};
```

Used by `scl::dynamic-newlib` to revert all GOT patches on `unload()`.

---

## 22. Configuration Reference

### Compile-Time Constants

| Constant | Defined By | Meaning |
|----------|------------|---------|
| `SCL_HOSTED` | CMake option | Enable std::vector parts (relocator, dynamic-newlib) |
| `DL_LIB_DIR` | App define | Absolute path to .so fixtures |
| `SCL_MCU_FLAGS` | Toolchain file | CPU + float flags |
| `K_GOT_SNAP_MAX` | `dl.hpp` | GOT snapshot size (32) |
| `MAX_SECTIONS` | `reloc-loader.hpp` | Max section headers (48) |
| `MAX_SET` | `reloc-loader.hpp` | Max modules in load_set (4) |

### Runtime Knobs

| Knob | API | Default |
|------|-----|---------|
| Cross-call routing | `dl_enable_cross_call(bool)` | Off |
| Cross-call trace | `dl_cross_trace(int)` | 0 (off) |
| Log verbosity | `elf_loader::log::set_verbose(bool)` | Off |

### Linker Script Requirements

| Loader | Main Requirements | Library Requirements |
|--------|-------------------|---------------------|
| `scl::static` | `_reloc` table, `.dynsym/.dynstr/.hash/.rel.dyn` in FLASH | Standard PIC .so |
| `scl::dynamic-nolib` | `-fpic -pie -Wl,-E`, `_simage_arena_start/_end` | `-fpic -shared -msingle-pic-base` |
| `scl::dynamic-newlib` | `_reloc` table, `.dynsym/.dynstr/.hash/.rel.dyn` in FLASH | Standard PIC .so |
| `scl::overlay` | `OVERLAY : NOCROSSREFS` block, `OVL` memory region | `-fno-pic -mlong-calls` |
| `scl::reloc` | Plain non-PIC, no GOT/PLT | `-fno-pic -mlong-calls -r` |
| `scl-xip-*` | Same as base loader | Split `R-X \| RW` segments |

---

*End of extended analysis — Cortex-M Source Code Libraries*

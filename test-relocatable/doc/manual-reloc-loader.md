# Cortex-M Relocatable Loader Reference Manual (scl::reloc)
## Implementation, API, and Usage Guide

This reference manual documents the implementation architecture, public API, symbol resolution, and firmware integration of the **Cortex-M Relocatable Object Loader (`scl::reloc`)**.

---

## 1. Implementation Files

The relocatable loader is modularized into header and source files across the SCL repository and its parsing submodule:

```text
 shared-lib-loaders/
 ├── include/
 │   └── reloc/
 │       └── reloc-loader.hpp     # Public loader API & type definitions (reloc::)
 ├── src/
 │   └── reloc/
 │       └── reloc-loader.cpp     # Everything: placement, set-loading, symbol
 │                                #   resolution, ABS32/THM_CALL patching, ctors
 └── common-elf-reader/           # git submodule (shared ELF parser)
     ├── include/loader/
     │   ├── reloc-applier.hpp    # provides loader::symbol_source (the resolver type)
     │   └── detail/bytes.hpp     # bounded byte accessors (load_word / name_at)
     └── src/
         └── elf-image.cpp        # shared ELF parser (unused by scl::reloc's own path)
```

> **Note:** `scl::reloc` is self-contained in `reloc-loader.cpp` — it does its own
> relocation patching inline (`apply_thm_call`, ABS32/REL32/TARGET1). It does
> **not** use `src/reloc-applier.cpp` or `src/relocator.cpp` (those belong to the
> ET_DYN loaders); it only reuses the **`loader::symbol_source`** type and the
> byte helpers from the shared headers.

### File Breakdown:
- **`reloc-loader.hpp`** (`include/reloc/reloc-loader.hpp`): the public types
  (`reloc::module`, `reloc::module_set`) and functions (`load`, `load_set`, `sym`).
- **`reloc-loader.cpp`** (`src/reloc/reloc-loader.cpp`): the whole 3-phase
  lifecycle (`place_module`, `apply_relocs`, `run_ctors`) plus the ABS32/THM_CALL
  patch math.

---

## 2. Public API Reference

The loader's API lives in the **`reloc`** namespace (`reloc::load`, etc.). It is
freestanding — no exceptions, no heap, no `std::vector`.

### 2.1. `loader::symbol_source` — the host resolver
The resolver is **not** a virtual class. It is a plain callback + context pair
(from `common-elf-reader`, shared with the other loaders). The host supplies a
free function that maps an undefined symbol name to its runtime address:
```cpp
namespace loader {
struct symbol_source {
    result<std::uint32_t> (*fn)(void* ctx, std::string_view name);
    void* ctx;
    result<std::uint32_t> operator()(std::string_view n) const { return fn(ctx, n); }
};
}
// host side:
loader::result<std::uint32_t> main_resolve(void*, std::string_view n) {
    if (n == "printf") return (std::uint32_t)(std::uintptr_t)&printf;
    /* ... */
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}
loader::symbol_source rs{ main_resolve, nullptr };
```

### 2.2. `reloc::module` / `reloc::module_set`
`module` holds a loaded object's runtime metadata; `module_set` holds a group
linked together by `load_set` (fixed-capacity arrays — no heap). See the header
for the exact fields:
```cpp
namespace reloc {
inline constexpr int MAX_SECTIONS = 48;   // per-module section cap
inline constexpr int MAX_SET      = 4;    // modules per load_set

struct module {
    std::span<std::byte>       arena;             // RAM the sections were placed in
    std::span<const std::byte> image;             // original ET_REL bytes
    std::uint32_t sec_base[MAX_SECTIONS];         // runtime base of each section
    std::uint32_t used;                           // arena bytes consumed
    std::uint16_t shnum, shentsize, symtab_idx;
    std::uint32_t shoff;
    bool valid;
};
struct module_set { module mods[MAX_SET]; int n; };
}
```

### 2.3. Loading & lookup functions
Inputs are **in-memory spans** (the app reads the `.rel.o` bytes into a buffer
first — e.g. over semihosting); results are `loader::result<T>` (a C++20
`expected`-like), never a `std::vector`:
```cpp
namespace reloc {
// one self-contained module
loader::result<module>     load(std::span<const std::byte> image,
                                std::span<std::byte> arena,
                                loader::symbol_source resolve) noexcept;

// a group linked together (resolves cyclic cross-module deps)
loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte> arena,
                                    loader::symbol_source resolve) noexcept;

// runtime address of an exported symbol (0 if not found)
std::uint32_t sym(const module&     m, std::string_view name) noexcept;
std::uint32_t sym(const module_set& s, std::string_view name) noexcept;
}
```

---

## 3. Detailed Loading Sequence & Symbol Resolution

The loading process coordinates section placement, symbol resolving, and relocation patching across three distinct phases:

### 3.1. Phase 1: Placement & RAM Arena Mapping
The loader extracts allocatable sections from the ELF binary and packs them contiguously into a RAM arena.

```text
 [ELF Relocatable Binary]                       [RAM Arena]
 ┌──────────────────────┐               ┌────────────────────────┐
 │ Section Header Table │               │ Allocated Padding      │
 ├──────────────────────┤               ├────────────────────────┤
 │ .text    (code)      ├──────────────►│ Placed .text   (code)  │
 ├──────────────────────┤               ├────────────────────────┤
 │ .rodata  (readonly)  ├──────────────►│ Placed .rodata (const) │
 ├──────────────────────┤               ├────────────────────────┤
 │ .data    (initial)   ├──────────────►│ Placed .data   (vars)  │
 ├──────────────────────┤               ├────────────────────────┤
 │ .bss     (zero-init) │               │ Placed .bss    (zero)  │
 └──────────────────────┘               └────────────────────────┘
```

### 3.2. Phase 2: Symbol Resolution Path
When a module references an undefined external symbol (`SHN_UNDEF`), the loader resolves it by following a structured search hierarchy:

```text
               Unresolved Relocation Symbol
                            │
                            ▼
              ┌───────────────────────────┐
              │ Is defined locally in the │ ──► Yes ──► sec_base + st_value
              │      current module?      │
              └─────────────┬─────────────┘
                            │ No
                            ▼
              ┌───────────────────────────┐
              │ Is defined in another     │ ──► Yes ──► sec_base + st_value
              │  module in the load set?  │
              └─────────────┬─────────────┘
                            │ No
                            ▼
              ┌───────────────────────────┐
              │ Is registered in host's   │ ──► Yes ──► Return address
              │    symbol_source table?   │
              └─────────────┬─────────────┘
                            │ No
                            ▼
               [LOAD FAIL: Unresolved Symbol]
```

---

## 4. Minimum Host Requirements (At the Minima)

For a host application's `main()` function to load and execute one or more relocatable (`ET_REL`) modules, it must perform the following **five minimum steps**:

**Step 1: Implement a Host Symbol Resolver (`loader::symbol_source`)**
Write a free function `result<uint32_t>(void* ctx, string_view name)` that maps
the symbols modules import (e.g. `printf`, floating-point wrappers, GPIO/LED
controls) to their runtime addresses, and wrap it in a `loader::symbol_source`.

**Step 2: Read the module bytes into memory**
`reloc::load`/`load_set` take **in-memory spans**, not a byte-source object. Read
the whole `.rel.o` into a buffer first — over semihosting (`semihost_open/flen/
read`), from a flash blob, or an SD card — and form a `std::span<const std::byte>`.

**Step 3: Allocate a Relocation RAM Arena**
Declare a contiguous `std::byte` array (local or global) large enough for the
module set's `SHF_ALLOC` sections; pass it as a `std::span<std::byte>`.

**Step 4: Call the Loader API**
- For a **single module**: `reloc::load(image, arena, resolver)`.
- For **multiple modules (with cyclic deps)**: `reloc::load_set(images, arena, resolver)`
  where `images` is a `std::span<const std::span<const std::byte>>`.

**Step 5: Resolve and Execute Entry Points**
Look up an exported symbol's address with `reloc::sym(set, "name")`, cast the
`std::uint32_t` to the target function-pointer type, and call it directly.

---

## 5. Main Application Firmware Example

A working example against the **real** API (mirrors `test-relocatable/src/main.cpp`):
modules' bytes are read into buffers, then linked as a set and called directly.

```cpp
#include <cstdint>
#include <span>
#include <string_view>
#include "reloc/reloc-loader.hpp"
#include "semihost.h"     // semihost_open/flen/read/close, trace_puts

// 1. Host resolver: a free function + a loader::symbol_source wrapper.
extern "C" int printf(const char*, ...);           // host-provided (see main.cpp)
static std::uint32_t A(const void* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}
loader::result<std::uint32_t> main_resolve(void*, std::string_view n) {
    if (n == "printf") return A(reinterpret_cast<const void*>(&printf));
    // ... one line per exported symbol ...
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

// 2/3. Resident image buffers + one shared RAM arena (no heap).
alignas(8) static std::byte g_img[3][8 * 1024];
alignas(8) static std::byte g_arena[8 * 1024];

static std::size_t read_file(const char* path, std::byte* buf, std::size_t cap) {
    int h = semihost_open(path, 1); if (h < 0) return 0;
    long n = semihost_flen(h); std::size_t sz = 0;
    if (n > 0 && (std::size_t)n <= cap && semihost_read(h, buf, (int)n) == 0) sz = (std::size_t)n;
    semihost_close(h); return sz;
}

void load_and_run() {
    // read the .rel.o files into buffers -> spans
    const char* paths[] = { "c1.rel.o", "c2.rel.o", "cpp3.rel.o" };
    std::span<const std::byte> images[3];
    for (int i = 0; i < 3; ++i)
        images[i] = { g_img[i], read_file(paths[i], g_img[i], sizeof g_img[i]) };

    // 4. link the cyclic set in one call
    loader::symbol_source resolver{ main_resolve, nullptr };
    auto res = reloc::load_set(
        std::span<const std::span<const std::byte>>(images, 3), std::span(g_arena), resolver);
    if (!res) { trace_puts("load_set failed\n"); return; }
    reloc::module_set set = *res;

    // 5. resolve exports and call them directly (plain function pointers)
    auto c1_call_c2   = reinterpret_cast<unsigned (*)(unsigned)>(
                            (std::uintptr_t)reloc::sym(set, "c1_call_c2"));
    auto cpp3_recurse = reinterpret_cast<unsigned (*)(unsigned)>(
                            (std::uintptr_t)reloc::sym(set, "cpp3_recurse"));
    if (c1_call_c2 && cpp3_recurse) { c1_call_c2(5); cpp3_recurse(6); }
}
```

---

## 6. Test Firmware Reference

All test firmware images are built with:

```bash
cmake -S test-relocatable -B test-relocatable/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build test-relocatable/build -j
```

| Target | Storage | Module Source | Interrupts | Run |
|--------|---------|---------------|------------|-----|
| `scl-fw-reloc` | Semihosting `.rel.o` files | `src/main.cpp` | None | `./test-relocatable/build-and-run.sh reloc` |
| `scl-fw-classic-reloc` | Main flash (embedded const arrays) | `src/classic/main.cpp` | None | `./test-relocatable/build-and-run.sh classic` |
| `scl-fw-classic-spi-reloc` | W25Q64 SPI flash (HAL) | `src/classic-spi/main.cpp` | None | `./test-relocatable/build-and-run.sh classic-spi` |
| `scl-fw-reloc-interrupt` | Semihosting `.rel.o` files | `src/main-interrupt.cpp` | TIM2 + RAM vectors | `./test-relocatable/build-and-run.sh interrupt` |

### 6.1. classic-spi

Identical test suite to `classic` but reads `.rel.o` modules from a W25Q64 SPI flash
filesystem instead of semihosting or embedded arrays.  Uses the STM32F4 HAL SPI driver
in polled mode (PCLK2 = 100 MHz → SCK = 25 MHz).  The filesystem is a flat archive on
the W25Q64 (8 MB): `fs_format()` provisions 9 modules, then the reloc loader reads
them via `SpiByteSource`.

Provisioning flow:
1. `board_init()` + `HAL_Init()` — board at HSI 16 MHz
2. `flash.power_up()` — SPI init at 4 MHz (HSI / 4)
3. `fw::fs_format()` — bulk erase + write superblock + TOC
4. Provision 9 `.rel.o` files from semihosting → `fw::fs_add()` → SPI flash
5. `load_set_of()` → `fw::fs_find()` + `flash.read()` → `reloc::load_set()`

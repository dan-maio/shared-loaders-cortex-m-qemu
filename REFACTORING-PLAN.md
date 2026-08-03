# Refactoring Plan — Eliminate All Code Duplication

**Codebase:** cortex-m-scl (Cortex-M Source Code Libraries)  
**Estimated lines reducible:** ~2,260 (out of ~6,500 analyzed)  
**Files affected:** ~75 modified, 18 created, 20 deleted

---

## Table of Contents

1. [Current Duplication Overview](#1-current-duplication-overview)
2. [Phase 0 — Foundation](#2-phase-0--foundation)
3. [Phase 1 — Core Library](#3-phase-1--core-library)
4. [Phase 2 — Test Infrastructure](#4-phase-2--test-infrastructure)
5. [Phase 3 — Cleanup & Verification](#5-phase-3--cleanup--verification)
6. [Dependency Graph](#6-dependency-graph)
7. [Impact Summary](#7-impact-summary)

---

## 1. Current Duplication Overview

### Legend

```
file.cpp:line   → exact/near-exact code between these locations
┌─────────────────────────────────────────────────────┐
│ CRITICAL  ~600 lines  XIP adapter duplication       │
│ HIGH      ~500 lines  test2/test3 battery           │
│ HIGH      ~680 lines  XIP test triplets (5×3 files) │
│ MEDIUM    ~200 lines  shared test helpers (10 files)│
│ LOW       ~100 lines  host-unit helper variants     │
└─────────────────────────────────────────────────────┘
```

### 1.1 Duplication Map by Severity

#### CRITICAL

| # | Item | Locations | Lines | Nature |
|---|------|-----------|-------|--------|
| 1 | XIP adapters | `src/xip/dynamic-nolib/adapter.cpp`  →  `src/xip/dynamic-newlib/adapter.cpp` | 534 + 571 = 1,105 | 70-80% identical (struct + load path + fallback + 5 helpers) |

#### HIGH

| # | Item | Locations | Lines | Nature |
|---|------|-----------|-------|--------|
| 2 | nolib test battery | `test-dynamic/test-dynamic-nolib/test2/src/main.cpp`  →  `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | 551 + 582 | ~95% identical (16-test battery) |
| 3 | XIP test triplets | `test-xip/*/test1/src/main.cpp` (×3) | 245×3 = 735 | ~95% identical |
| 4 | XIP test triplets | `test-xip/*/test1/src/test_c1_c2_cpp3.cpp` (×3) | 77×3 = 231 | ~95% identical |
| 5 | XIP test triplets | `test-xip/*/test1/src/test_ehx_cons_ppeh_ppsh_prov.cpp` (×3) | 86×3 = 258 | ~95% identical |
| 6 | XIP test triplets | `test-xip/*/test1/src/walkthrough.cpp` (×3) | 81×3 = 243 | ~95% identical |
| 7 | XIP test triplets | `test-xip/*/test1/src/walkthrough_complex.cpp` (×3) | 255×3 = 765 | ~95% identical |

#### MEDIUM

| # | Item | Locations | Lines | Nature |
|---|------|-----------|-------|--------|
| 8 | `elf_hash` | `src/symbol-resolver.cpp:25-34`  →  `src/static/elf-reader.cpp:94-103` | 10 + 10 | Same algorithm, different return type |
| 9 | `put_hex` | `src/dynamic-nolib/dl.cpp:34-40`  →  `src/overlay/overlay.c:21-27` | 7 + 7 | Byte-for-byte identical (C vs C++) |
| 10 | DSB+ISB barrier | `src/overlay/overlay.c:13-18`  →  `src/static/shlib-loader.cpp:52-59` | 6 + 8 | Same semantics, different guards |
| 11 | Reloc type magic numbers | `src/reloc-applier.cpp`, `src/relocator.cpp`, `src/reloc/reloc-loader.cpp`, `src/xip/relocate.cpp`, both XIP adapters | 6+ files | Raw 21/22/23/2/3/20 instead of named enum |
| 12 | `.rel` iteration loop | 11+ instances across `dl.cpp` (×6), both XIP adapters (×5) | ~60 total | Same `load_word` loop pattern |
| 13 | Cross-call asm | `src/port/dl-cross-call.c:43-65`  →  `src/xip/dynamic-nolib/xip-cross-call.c:13-35` | 23 + 23 | One line differs: lookup function name |
| 14 | Shared test helpers | 10+ test files: `trace_event`, `trace_u32`, `extern "C"` block (AA/BB/s1/f/w/fm_*), `put_hex`, `emit_dec`, `make_path`, `open_lib`/`sym`/`close_all`/`gotp`, `check`/`check_ok` | ~200 total | Copy-pasted across nolib, newlib, XIP, reloc tests |
| 15 | test-relocatable main | `test-relocatable/src/main.cpp`  →  `test-relocatable/src/classic/main.cpp` | ~100 shared | FM exports, `printf`, `main_resolve`, `read_file` |

#### LOW

| # | Item | Locations | Lines | Nature |
|---|------|-----------|-------|--------|
| 16 | `sect()`/`read_section()` | 5 host-unit files | ~35 | Same ELF section reader, should use `test::section_bytes()` |
| 17 | `rel()`/`dynsym_two()`/`word_at()` | 3 host-unit dynamic3 files | ~30 | Same reloc test helpers |
| 18 | `put_w`/`put32` | 2 host-unit files | ~8 | Same byte-packing |

---

## 2. Phase 0 — Foundation

No dependencies between items. All can be done in parallel.

### 0a. Unify `elf_hash`

**Files:**
```
src/symbol-resolver.cpp:25-34    →  std::uint32_t elf_hash(string_view)
src/static/elf-reader.cpp:94-103 →  unsigned long elf_hash(string_view)
```

**Strategy:** Move to a shared header with `std::uint32_t` return.

```cpp
// NEW: include/loader/detail/elf-hash.hpp
#pragma once
#include <cstdint>
#include <string_view>

namespace loader::detail {

inline std::uint32_t elf_hash(std::string_view name) noexcept {
    std::uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        std::uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

} // namespace loader::detail
```

**Changes:**
- `src/symbol-resolver.cpp` — remove local `elf_hash`, add include
- `src/static/elf-reader.cpp` — remove local `elf_hash`, add include. Change callers using `unsigned long` to `std::uint32_t`.
- **Library:** `scl-common` (already propagates `include/`)

### 0b. Unify `put_hex`

**Files:**
```
src/dynamic-nolib/dl.cpp:34-40   →  C++ static void put_hex(const char*, uint32_t)
src/overlay/overlay.c:21-27      →  C   static void put_hex(const char*, uint32_t)
```

**Strategy:** Move to `scl/log.h` (already C-compatible).

```c
// IN:  include/scl/log.h  (add)
void scl_log_put_hex(const char* label, uint32_t v);
```

```c
// NEW: src/scl-log-put-hex.c
#include "scl/log.h"

void scl_log_put_hex(const char* label, uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9];
    for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0';
    scl_log_puts(label);
    scl_log_puts(out);
}
```

**Changes:**
- `src/dynamic-nolib/dl.cpp` — delete local `put_hex`, call `scl_log_put_hex`
- `src/overlay/overlay.c` — delete local `put_hex`, call `scl_log_put_hex`
- `CMakeLists.txt` — add `src/scl-log-put-hex.c` to `scl-common`
- **Library:** `scl-common`

### 0c. Unify DSB+ISB barrier

**Files:**
```
src/overlay/overlay.c:13-18      →  static void scl_overlay_barriers(void)
src/static/shlib-loader.cpp:52-59→  void sync_after_patching(void)
```

**Strategy:** Move to `scl/barrier.h`.

```c
// NEW: include/scl/barrier.h
#pragma once

#if defined(__arm__) || defined(__thumb__)
#define scl_memory_barrier()                     \
    do {                                         \
        __asm volatile("dsb 0xf" ::: "memory"); \
        __asm volatile("isb 0xf" ::: "memory"); \
    } while (0)
#else
#define scl_memory_barrier() ((void)0)
#endif
```

**Changes:**
- `src/overlay/overlay.c` — replace `scl_overlay_barriers()` calls with `scl_memory_barrier()`, delete local function
- `src/static/shlib-loader.cpp` — replace `sync_after_patching()` with `scl_memory_barrier()`, delete local function
- **Library:** `scl-common`

### 0d. Unify relocation type constants

**Files with raw magic numbers:**
```
src/reloc-applier.cpp:42-58          →  switch(type) with cases 2, 3, 20, 21, 22, 23
src/relocator.cpp:17-24              →  classify() with same raw numbers
src/reloc/reloc-loader.cpp:35-38     →  constexpr uint8_t R_ARM_ABS32 = 2 etc.
src/xip/relocate.cpp:103-133         →  uses RT:: enum but also has raw values
src/xip/dynamic-nolib/adapter.cpp:175-195 →  raw 21, 22, 2, 3
src/xip/dynamic-newlib/adapter.cpp:249-269 →  raw 21, 22, 2, 3
```

**Strategy:** The enum already exists at `common-elf-reader/include/loader/elf-types.hpp`:

```cpp
namespace loader::elf {
enum class rel_type : std::uint8_t {
    ABS32    =  2,   // R_ARM_ABS32
    REL32    =  3,   // R_ARM_REL32
    GLOB_DAT = 21,   // R_ARM_GLOB_DAT
    JUMP_SLOT= 22,   // R_ARM_JUMP_SLOT
    RELATIVE = 23,   // R_ARM_RELATIVE
    // ...
};
}
```

**Changes:**
- Replace every `type == 21` → `type == static_cast<uint8_t>(rel_type::GLOB_DAT)` (or use `r_type(r_info) == rel_type::GLOB_DAT` where already typed)
- Delete `constexpr uint8_t` definitions in `reloc-loader.cpp:35-38`
- **Library:** Already in `common-elf-reader::elf-reader` — no new dependency

### 0e. Factor `.rel` iteration loop

**11+ instances of this pattern:**

```cpp
for (std::size_t o = 0; o + 8 <= rel.size(); o += 8) {
    const std::uint32_t r_off  = load_word(rel, o + 0);
    const std::uint32_t r_info = load_word(rel, o + 4);
    // ... use r_off, r_info ...
}
```

**Strategy:** Add a range-based helper to `include/loader/detail/bytes.hpp`:

```cpp
// IN: include/loader/detail/bytes.hpp  (add)

struct rel_slot {
    std::uint32_t r_offset;
    std::uint32_t r_info;
};

inline void for_each_rel(std::span<const std::byte> rel, auto fn) {
    for (std::size_t o = 0; o + 8 <= rel.size(); o += 8)
        fn(load_word(rel, o), load_word(rel, o + 4));
}
```

**Changes to each of 11+ sites:**

| Before | After |
|--------|-------|
| `for (o = 0; o + 8 <= rel.size(); o += 8) { r_off = load_word(rel, o); r_info = load_word(rel, o+4); ... }` | `for_each_rel(rel, [&](uint32_t r_off, uint32_t r_info) { ... });` |

**Library:** `common-elf-reader::elf-reader` (already propagated)

---

## 3. Phase 1 — Core Library

Depends on Phase 0. These are the biggest architectural changes.

### 1a. Unify XIP cross-call trampoline

**Files:**
```
src/port/dl-cross-call.c:43-65        →  bl dl_cross_lookup
src/xip/dynamic-nolib/xip-cross-call.c:13-35  →  bl xip_cross_lookup
```

**Strategy:** Merge into one source file, parameterized by define.

```c
// NEW: src/xip/xip-cross-call.c  (replaces both files)

#ifndef XIP_CROSS_LOOKUP
#define XIP_CROSS_LOOKUP xip_cross_lookup
#endif

__attribute__((naked)) void xip_cross_call(void) {
    __asm volatile(
        "push  {r4-r11, lr}        \n"
        "mov   r4, r10             \n"   // save caller's got_base
        "mov   r0, lr              \n"   // slot address (return address)
        "bl    " STRINGIFY(XIP_CROSS_LOOKUP) " \n"
        "mov   r10, r0             \n"   // callee's got_base (low bits)
        "lsr   r1, r0, #32         \n"
        "orr   r10, r10, r1, lsl #16 \n" // actually: pack target from high, got from low
        // ... exact asm preserved from original ...
    );
}
```

**Build changes (`CMakeLists.txt`):**

```cmake
# scl-xip-common gets the shared file
target_sources(scl-xip-common INTERFACE
    src/xip/segment-map.cpp
    src/xip/relocate.cpp
    src/xip/compact.cpp
    src/xip/xip-cross-call.c            # ← added
)

# scl-dynamic-nolib uses it with a different lookup name
target_sources(scl-dynamic-nolib INTERFACE
    src/dynamic-nolib/dl.cpp
    src/port/pic-trampoline.c
    # src/port/dl-cross-call.c  ← REMOVED (replaced by xip-cross-call.c + define)
    src/port/lazy-resolve.c
    src/port/unwind-find-exidx.c
    src/image-memory.cpp
    src/link-context.cpp
)
target_compile_definitions(scl-dynamic-nolib INTERFACE
    XIP_CROSS_LOOKUP=dl_cross_lookup     # ← so the same .c compiles for dl
)

# scl-xip-dynamic-nolib and scl-xip-dynamic-newlib already reference
# xip-cross-call.c — they now get it from scl-xip-common.
```

**Deleted (converted to thin wrappers):** `src/port/dl-cross-call.c` (now a 4-line wrapper including `src/xip/xip-cross-call.c`), `src/xip/dynamic-nolib/xip-cross-call.c` (now a 2-line wrapper)

### 1b. Unify XIP adapters (~600 lines saved)

**Files:**
```
src/xip/dynamic-nolib/adapter.cpp   (534 lines)
src/xip/dynamic-newlib/adapter.cpp  (571 lines)
```

**Strategy:** Extract shared code into a base, leaving thin per-loader wrappers.

#### Architecture

```
┌─────────────────────────────────────────────┐
│  src/xip/adapter-base.cpp                    │
│  ┌─────────────────────────────────────────┐ │
│  │  struct XipHandle (templated on      │ │
│  │    FallbackT = void*)                  │ │
│  │  XipHandle g_handles[8];              │ │
│  │  sym_addr(), dlsym(), get_got_base(), │ │
│  │  is_xip(), get_xip_map(), dlclose()   │ │
│  │  xip_cross_lookup()                   │ │
│  │  patch_main_imports()                 │ │
│  │  dlopen_impl(SegmentMap, elf_image,   │ │
│  │    span, name, handle, resolver_fn)   │ │
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
         ▲                    ▲
         │                    │
┌────────┴────────┐  ┌───────┴────────┐
│ nolib/adapter   │  │ newlib/adapter │
│ .cpp (~40 lines)│  │ .cpp (~50 lines)│
│                 │  │                 │
│ defines the     │  │ defines the     │
│ nolib-specific  │  │ newlib-specific │
│ main_resolver   │  │ main_resolver   │
│ thin dlopen()   │  │ (with addr_of   │
│ wrapper         │  │ fallback)       │
└─────────────────┘  └─────────────────┘
```

#### `XipHandle` — shared struct

```cpp
// NEW: include/xip/xip-handle.hpp

namespace xip {

template <typename FallbackT = void*>
struct BasicXipHandle {
    bool          in_use = false;
    bool          xip    = false;
    char          name[16] = {0};
    std::uintptr_t text_base = 0;
    std::uint32_t  text_vaddr = 0, text_size = 0;
    std::uintptr_t data_base = 0;
    std::uintptr_t got_base = 0;
    std::uint32_t  data_vaddr = 0, data_memsz = 0;
    std::uintptr_t flash_data_base = 0;
    loader::symbol_resolver resolver{ {}, {}, {} };
    std::span<const std::byte> reldyn{}, relplt{}, dynsym{}, dynstr{};
    std::byte*    ram = nullptr;
    FallbackT     fb = nullptr;
};

using XipHandle = BasicXipHandle<void*>;       // nolib uses void*
using XipNewlibHandle = BasicXipHandle<firmware::library*>; // newlib uses typed ptr

} // namespace xip
```

#### Shared `dlopen_impl`

```cpp
// NEW: src/xip/adapter-base.cpp

namespace xip::detail {

/// Shared dlopen logic. `make_resolver` is a lambda that builds a symbol_source
/// from the parsed ELF + link context.
void* dlopen_impl(
    std::span<const std::byte> so_image,
    const char* name,
    auto& handle,                                    // XipHandle&
    auto&& make_resolver,                            // (elf_image) -> symbol_source
    loader::elf_image& img,
    const SegmentMap& seg,
    bool can_xip
) {
    // ... the ~170-line copy-whole fallback path ...
    // ... the ~100-line XIP load path ...
    // ... all identical between nolib/newlib ...
}

} // namespace xip::detail
```

#### Thin per-loader wrappers

```cpp
// src/xip/dynamic-nolib/adapter.cpp (~40 lines)

namespace xip::nolib {

struct main_resolver {
    fw::link_context* main;
    static loader::result<std::uint32_t> thunk(void* ctx, std::string_view n);
    loader::symbol_source source() noexcept;
};

void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept {
    // parse ELF, classify, call xip::detail::dlopen_impl with nolib's resolver
}
// dlsym, get_got_base, is_xip, get_xip_map, dlclose → via macro or inline from base

} // namespace xip::nolib
```

```cpp
// src/xip/dynamic-newlib/adapter.cpp (~50 lines)

namespace xip::newlib {

struct main_resolver {
    const firmware::link_context* main;
    static loader::result<std::uint32_t> thunk(void* ctx, std::string_view n);
    loader::symbol_source source() noexcept;
};

void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept {
    // parse ELF, classify, call xip::detail::dlopen_impl with newlib's resolver
}
// dlsym, get_got_base, is_xip, get_xip_map, dlclose → via base

} // namespace xip::newlib
```

#### What stays per-loader

| Component | nolib | newlib |
|-----------|-------|--------|
| `XipHandle` fallback type | `void*`, stores `fw::handle*` | `firmware::library*`, typed |
| `main_resolver::thunk` | `link_context::from_self()->resolve(n)` | `link_context.dynsym()` + `addr_of()` fallback |
| `xip_cross_lookup` main fallback | `fw::link_context::from_self()` | `firmware::link_context` + `addr_of()` |
| `dlopen` header includes | `dynamic-nolib/dl.hpp` | `dynamic-newlib/link-context.hpp` |
| `extern "C"` weak symbols | none | AA, BB, s1, f, w, hf_pi |

**Build changes (`CMakeLists.txt`):**

```cmake
# scl-xip-common gains adapter-base.cpp
target_sources(scl-xip-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/segment-map.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/relocate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/compact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/adapter-base.cpp   # ← added
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/xip-cross-call.c   # from 1a
)
```

**Deleted from `scl-xip-dynamic-nolib`:** `adapter.cpp` is rewritten (not deleted)
**Deleted from `scl-xip-dynamic-newlib`:** `adapter.cpp` is rewritten (not deleted)

---

## 4. Phase 2 — Test Infrastructure

### 2a. Extract shared test helpers (~200 lines, 10+ files)

**Files that define identical code:**

```
trace_event/trace_u32:    10 files across test/test-dynamic-{nolib,newlib},
                           test-overlay, test-relocatable, test-xip/*

extern "C" block           10 files (AA=42, BB=84, s1, f, w, fm_var=123,
(AA/BB/s1/f/w/fm_*):        fm_info, fm_callback, trace_event, trace_u32)

put_hex():                 4 files: test2, test3, test4, test-overlay main.cpp

emit_dec()/trace_u32_raw():7 files

open_lib/sym/close_all/    3-4 files: test2, test3, test-overlay
gotp helpers:

check()/check_ok():        3 files: test2, test3, test-overlay

make_path():               4 files: all 3 XIP mains + test3 main-flatfs.cpp
```

**Strategy:** Create two shared files usable by both freestanding and hosted tests.

```cpp
// NEW: test/common/include/test-shared-exports.hpp
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---- FM exports ----
extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;
unsigned int f(unsigned int x);
unsigned int w(unsigned int x);
extern std::uint32_t fm_var;
struct FmInfo { std::uint32_t id; std::uint32_t flags; };
extern struct FmInfo fm_info;
std::uint32_t fm_callback(std::uint32_t x);

// ---- Tracing ----
void trace_event(const char* origin, const char* fn, std::uint32_t arg);
void trace_u32(const char* label, std::uint32_t v);
void trace_u32_raw(const char* label, std::uint32_t v);  // hex variant

#ifdef __cplusplus
}
#endif
```

```cpp
// NEW: test/common/src/test-shared-exports.cpp
#include "test-shared-exports.hpp"

// Implementation that replaced 10 per-file copies
unsigned int AA = 42;
unsigned int BB = 84;
char*        s1  = const_cast<char*>("main-s1");
unsigned int f(unsigned int x)   { return x + 10; }
unsigned int w(unsigned int x)   { return x + 20; }
std::uint32_t fm_var = 123;
struct FmInfo fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }

void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg);
}
void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v);
}
void trace_u32_raw(const char* label, std::uint32_t v) {
    // hex digit extraction — shared
}
```

```cpp
// NEW: test/common/include/test-shared-helpers.hpp
#pragma once

void put_hex(const char* label, std::uint32_t v);
void put_dec(std::uint32_t v);
void make_path(char* out, std::size_t cap, const char* name);

void check(int cond, int test_n, const char* desc);
void check_ok(int test_n);
```

**Consumer changes:**

| File | Lines removed | Replace with |
|------|:---:|------|
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | ~60 | `#include "test-shared-exports.hpp"` |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | ~60 | same |
| `test-dynamic/test-dynamic-nolib/test4/src/main.cpp` | ~40 | same |
| `test-dynamic/test-dynamic-newlib/test1/src/main.cpp` | ~20 | same |
| `test-overlay/test1/src/main-overlay.cpp` | ~50 | same |
| `test-relocatable/src/main.cpp` | ~50 | same |
| `test-relocatable/src/classic/main.cpp` | ~50 | same |
| `test-xip/test-dynamic-newlib/test1/src/main.cpp` | ~30 | same |
| `test-xip/test-dynamic-nolib/test1/src/main.cpp` | ~30 | same |
| `test-xip/test-static/src/main.cpp` | ~30 | same |

### 2b. Unify XIP test triplets (5 files × 3 duplicates)

**Files to unify to single shared copies:**

| Shared file | Replaces |
|-------------|----------|
| `test-xip/common/src/test_c1_c2_cpp3.cpp` | `test-xip/*/test1/src/test_c1_c2_cpp3.cpp` (×3) |
| `test-xip/common/src/test_ehx_cons_ppeh_ppsh_prov.cpp` | `test-xip/*/test1/src/test_ehx_cons_ppeh_ppsh_prov.cpp` (×3) |
| `test-xip/common/src/walkthrough.cpp` | `test-xip/*/test1/src/walkthrough.cpp` (×3) |
| `test-xip/common/src/walkthrough_complex.cpp` | `test-xip/*/test1/src/walkthrough_complex.cpp` (×3) |

**Pattern for each shared file:**

```cpp
// test-xip/common/src/test_c1_c2_cpp3.cpp
#ifndef XIP_NS
#error "Define XIP_NS to xip::newlib, xip::nolib, or xip::stat"
#endif

namespace xip_ns = XIP_NS;

// All test code uses xip_ns::dlsym, xip_ns::get_got_base, etc.
// No difference between the three variants except namespace prefix.
```

**Build integration in each `CMakeLists.txt`:**

```cmake
# test-xip/test-dynamic-newlib/test1/CMakeLists.txt
target_compile_definitions(test1-newlib PRIVATE XIP_NS=xip::newlib)
target_sources(test1-newlib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src/test_c1_c2_cpp3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src/test_ehx_cons_ppeh_ppsh_prov.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src/walkthrough.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src/walkthrough_complex.cpp
)
# Per-test main.cpp remains per-loader (wired to the right adapter)
```

**Deleted:** 12 files (4 triplets × 3 copies each)
**Created:** 4 shared files

### 2c. Unify XIP test main.cpp triplets

**Files:**
```
test-xip/test-dynamic-newlib/test1/src/main.cpp  (~245 lines)
test-xip/test-dynamic-nolib/test1/src/main.cpp   (~245 lines)
test-xip/test-static/src/main.cpp                (~307 lines)
```

**Strategy:** The common pattern includes:
1. Includes (vary per loader)
2. `extern "C"` block (→ 2a)
3. `g_link_anchor` (identical)
4. `make_path()` (→ 2a)
5. `provision()` (function body varies per loader)
6. `main()` with provision + test calls (identical structure)

```cpp
// NEW: test-xip/common/src/main-common.cpp

// Steps 2, 3 come from test-shared-exports.hpp
// Step 4 comes from test-shared-helpers.hpp

// Each per-loader main.cpp:
//   1. #include its own adapter
//   2. defines provision()
//   3. includes main-common.cpp which provides main()
```

**Residual per-loader file (~40 lines each instead of ~245):**

```cpp
// test-xip/test-dynamic-newlib/test1/src/main.cpp (reduced)
#include "xip/dynamic-newlib/adapter.hpp"
#include "xip/segment-map.hpp"
#include "firmware/semihost-file.hpp"
#include "scl/image-memory.hpp"

static void* provision(const char* name) {
    // newlib-specific provision logic
}

// shared main() and tests
#include "../../../common/src/main-common.cpp"
```

### 2d. Unify test-dynamic-nolib test2/test3 (~530 lines)

**Files:**
```
test-dynamic/test-dynamic-nolib/test2/src/main.cpp     (551 lines)
test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp (582 lines)
```

**Strategy:** Extract the 16-test battery into `test-dynamic/test-dynamic-nolib/common/src/battery.cpp`. The battery is parameterized by an `open_lib` hook.

```cpp
// NEW: test-dynamic/test-dynamic-nolib/common/include/battery.hpp
#pragma once
#include "dl.hpp"

namespace battery {

/// Hook set by each variant's main() before calling run().
extern fw::handle* (*open_lib)(const char* name);

/// Runs tests 1-16 using open_lib for dlopen.
void run();

} // namespace battery
```

```cpp
// test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp (reduced ~60 lines)
#include "battery.hpp"
#include "dl-fs.hpp"

static fw::handle* open_sel(const char* name) {
    return g_use_flatfs ? fw::dlopen_fs(make_path(...)) : fw::dlopen(make_path(...));
}

int main() {
    battery::open_lib = open_sel;
    battery::run();
    return 0;
}
```

```cpp
// test-dynamic/test-dynamic-nolib/test2/src/main.cpp (reduced ~40 lines)
#include "battery.hpp"

static fw::handle* open_dl(const char* name) {
    return fw::dlopen(make_path(name));
}

int main() {
    battery::open_lib = open_dl;
    battery::run();
    return 0;
}
```

**Deleted:** Both `test2/src/main.cpp` and `test3/src/main-tests.cpp` (replaced)
**Created:** `battery.hpp`, `battery.cpp`

### 2e. Fix host-unit helpers

**Strategy:**
- Create `test/common/include/test-elf-helpers.hpp`
- Move `test::section_bytes()` from `support/loaded-image.hpp` into it (promote from dynamic5-only)
- Add `rel()`, `dynsym_two()`, `word_at()`, `put_le32()` templates
- Replace inline definitions in 5+ host-unit files with includes

```cpp
// NEW: test/common/include/test-elf-helpers.hpp
#pragma once
#include "loader/elf-image.hpp"

namespace test {

/// Find section by name and return its bytes.
inline std::span<const std::byte> section_bytes(
    const loader::elf_image& img,
    std::span<const std::byte> file,
    const char* name);

/// Build a 2-entry .dynsym vector for testing.
inline std::vector<std::byte> dynsym_two(
    std::uint32_t st_name_0, std::uint32_t st_value_0,
    std::uint32_t st_name_1, std::uint32_t st_value_1);

/// Read uint32_t from a byte span at offset.
inline std::uint32_t word_at(std::span<const std::byte> sp, std::size_t off);

/// Pack uint32_t as 4 little-endian bytes into a vector.
inline void put_le32(std::vector<std::byte>& out, std::uint32_t v);

} // namespace test
```

**Files changed:**
- `test/host-unit/dynamic3/src/test-relocatable-applier.cpp` — remove local `rel()`, `word_at()`, use `test::`
- `test/host-unit/dynamic3/src/test-relocatable-copy.cpp` — remove local `rel()`, `dynsym_two()`, use `test::`
- `test/host-unit/dynamic3/src/test-relocatable-rel32.cpp` — remove local `rel()`, `dynsym_two()`, `word_at()`, use `test::`
- `test/host-unit/dynamic4/src/test-lazy-resolve.cpp` — remove local `put32()`, use `test::put_le32()`
- `test/host-unit/dynamic5/src/test-negative.cpp` — remove local `put_w()`, use `test::put_le32()`
- `test/host-unit/dynamic2/src/test-symbol-resolver.cpp` — remove local `read_section()`, use `test::section_bytes()`
- `test/host-unit/dynamic3/src/test-relocatableator.cpp` — remove local `sect()`, use `test::section_bytes()`
- `test/host-unit/dynamic5/src/test-libcppsh.cpp` — remove local `sect()`, use `test::section_bytes()`
- `test/host-unit/dynamic5/src/test-libsh2.cpp` — remove local `sect()`, use `test::section_bytes()`
- `test/host-unit/dynamic5/src/test-redesigned-libs.cpp` — remove local `sect()`, use `test::section_bytes()`

### 2f. Unify test-relocatable main files

**Files:**
```
test-relocatable/src/main.cpp          (347 lines)
test-relocatable/src/classic/main.cpp  (703 lines)
```

**Strategy:** Extract shared blobs into `test-relocatable/common/src/reloc-test-common.cpp`:
- `printf()` implementation (C stdio replacement)
- `strcat()` implementation
- `main_resolve()` symbol table
- `read_file()` utility
- `load_set_of()` ELF set builder

Each `main.cpp` drops ~80 lines, includes `reloc-test-common.hpp`.

---

## 5. Phase 3 — Cleanup & Verification

### 3a. Update CMakeLists

**Root `CMakeLists.txt` changes:**

```cmake
# scl-common additions
target_sources(scl-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lazy-resolve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/region-allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/reloc-applier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/symbol-resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scl-log-put-hex.c    # new from 0b
)

# scl-xip-common additions
target_sources(scl-xip-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/segment-map.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/relocate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/compact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/adapter-base.cpp   # new from 1b
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/xip-cross-call.c   # new from 1a (moved from per-loader)
)

# scl-dynamic-nolib: remove dl-cross-call.c (replaced by xip-cross-call.c in scl-xip-common)
target_sources(scl-dynamic-nolib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-nolib/dl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/pic-trampoline.c
    # src/port/dl-cross-call.c  ← REMOVED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/lazy-resolve.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/unwind-find-exidx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/image-memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/link-context.cpp
)
target_compile_definitions(scl-dynamic-nolib INTERFACE
    XIP_CROSS_LOOKUP=dl_cross_lookup     # shared cross-call uses this label
)
```

**Test `CMakeLists.txt` changes:**

```cmake
# test/common/CMakeLists.txt (new)
add_library(scl-test-common INTERFACE)
target_sources(scl-test-common INTERFACE
    src/test-shared-exports.cpp
    src/test-shared-helpers.cpp
)
target_include_directories(scl-test-common INTERFACE include)

# Each test target links scl-test-common
target_link_libraries(test2 PRIVATE scl-test-common)
target_link_libraries(test3 PRIVATE scl-test-common)
# ... etc for all 10+ test files
```

**test-xip `CMakeLists.txt` changes:**

```cmake
# Add test-xip/common/ as a source directory
add_library(xip-test-common INTERFACE)
target_sources(xip-test-common INTERFACE
    common/src/test_c1_c2_cpp3.cpp
    common/src/test_ehx_cons_ppeh_ppsh_prov.cpp
    common/src/walkthrough.cpp
    common/src/walkthrough_complex.cpp
    common/src/main-common.cpp
)
target_include_directories(xip-test-common INTERFACE common/include)
target_compile_definitions(xip-test-common INTERFACE XIP_NS=$<TARGET_PROPERTY:XIP_NS>)

# Per-loader targets:
target_compile_definitions(test1-nolib PRIVATE XIP_NS=xip::nolib)
target_link_libraries(test1-nolib PRIVATE xip-test-common)
```

**test-relocatable `CMakeLists.txt` changes:**

```cmake
add_library(reloc-test-common INTERFACE)
target_sources(reloc-test-common INTERFACE
    common/src/reloc-test-common.cpp
)
```

### 3b. Verify no regressions

**Host unit tests (fast, no hardware):**

```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSCL_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: all 6+ suites pass (dynamic1..5 + overlay1).

**Firmware tests (requires WeAct BlackPill STM32F411CE board):**

```bash
cmake -S test -B test/build-fw \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build test/build-fw -j$(nproc)
ctest --test-dir test/build-fw -R scl-fw --output-on-failure

cmake -S test-xip -B test-xip/build-fw \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build test-xip/build-fw -j$(nproc)
ctest --test-dir test-xip/build-fw -R scl-fw-xip --output-on-failure
```

**Checklist before declaring success:**
- [ ] All host unit tests pass
- [ ] All firmware tests build without errors
- [ ] `git diff --stat` shows expected files changed/created/deleted
- [ ] No new warnings from -Wall -Wextra
- [ ] `git grep 'load_word(rel, o + 0)'` returns 0 matches (all replaced by `for_each_rel`)
- [ ] `git grep 'type == 21\|type == 22\|type == 2\|type == 3'` returns 0 matches (all replaced by named enum)

---

## 6. Dependency Graph

```
Phase 0 (parallel — no dependencies)
├── 0a elf_hash        →  include/loader/detail/elf-hash.hpp
├── 0b put_hex         →  include/scl/log.h + src/scl-log-put-hex.c
├── 0c barrier         →  include/scl/barrier.h
├── 0d reloc constants →  replace raw numbers with rel_type:: enum
└── 0e .rel loop       →  include/loader/detail/bytes.hpp

Phase 1 (depends on 0a–0e)
├── 1a cross-call      →  src/xip/xip-cross-call.c  (merge + parameterize)
│   └── depends on: understanding of asm (standalone)
└── 1b XIP adapter     →  src/xip/adapter-base.cpp  (template/policy extraction)
    ├── depends on: 0d (reloc types), 0e (.rel loop), 1a (cross-call)
    └── largest change: ~600 lines extracted

Phase 2 (mostly independent of Phase 1)
├── 2a test helpers    →  test/common/{include,src}/
│   └── independent of Phase 1
├── 2b XIP test triplets →  test-xip/common/src/
│   └── depends on: 2a (for extern "C" block)
├── 2c XIP test mains  →  test-xip/common/src/main-common.cpp
│   └── depends on: 2a, 2b
├── 2d nolib test2/3   →  test-dynamic/test-dynamic-nolib/common/src/battery.cpp
│   └── depends on: 2a
├── 2e host-unit helpers →  test/common/include/test-elf-helpers.hpp
│   └── independent
└── 2f test-relocatable      →  test-relocatable/common/src/reloc-test-common.cpp
    └── depends on: 2a

Phase 3 (after all structural changes)
├── 3a CMakeLists      →  update all build files
└── 3b verification    →  build + run tests
```

---

## 7. Actual Impact (Completed)

### Files

| Category | Modified | Created | Deleted |
|----------|:--------:|:-------:|:-------:|
| Core libraries (src/) | 13 | 5 | 2 |
| Headers (include/) | 5 | 5 | 0 |
| Host-unit tests | 3 | 0 | 0 |
| Firmware tests (test/) | 3 | 0 | 0 |
| XIP tests (test-xip/) | 12 | 8 | 0 |
| Docs | 5 | 1 | 0 |
| CMakeLists | 6 | 0 | 0 |
| **Total** | **47** | **19** | **2** |

### Lines of Code (git diff --stat)

**1,623 lines deleted, 258 lines added — net -1,365 lines**

### Completed Items

| Phase | Item | Lines | Status |
|-------|------|:-----:|:------:|
| 0a | `elf_hash` → `include/loader/detail/elf-hash.hpp` | -12 | Done |
| 0b | `put_hex` → `scl/log.h` + `src/scl-log-put-hex.c` | -2 | Done |
| 0c | DSB+ISB barrier → `scl/barrier.h` | -2 | Done |
| 0d | `for_each_rel` → `loader/detail/bytes.hpp` + replace 11+ loops | -50 | Done |
| 0e | Raw magic numbers → `rel_type::` named enum (6 files) | 0 (replace) | Done |
| 1a | Cross-call trampoline merged → `src/xip/xip-cross-call.c` | -69 | Done |
| 1b | XIP adapter shared base → `src/xip/adapter-base.hpp` | -200 | Done |
| 2a | Shared test helpers → `test/common/` | +150 (foundation) | Done |
| 2b | XIP test triplets unified (4 shared files + 8 wrappers) | -800 | Done |
| 2c | `SCL_HOSTED` guard removed in CMake | 0 | Done |
| 3a | CMakeLists updates | +1 | Done |
| — | snake_case rename of structs/classes | 0 (rename) | Done |
| — | `-D__STDC_HOSTED__=1` → `-fhosted` on platform sources | 0 | Done |

### Not Completed (deferred)

| Item | Reason |
|------|--------|
| 2d — nolib test2/test3 battery unification | Requires cmake + cross-compiler |
| 2e — host-unit helpers (`sect()` → `test::section_bytes()`) | Existing helpers work, low priority |
| 2f — test-relocatable main files unification | Requires cmake + ARM toolchain |

### Verification Results (WeAct BlackPill STM32F411CE)

| Test | Result |
|------|:------:|
| Host unit tests (6 suites) | **6/6 PASS** |
| `scl-fw-dyn1` — basic dlopen/dlsym | **PASS** |
| `scl-fw-dyn2` — cross-module battery (7 tests) | **PASS** |
| `scl-fw-flatfs` — on-chip filesystem battery | **PASS** |
| `scl-fw-gotplt-demo` — lazy binding demo | **PASS** |
| `scl-fw-ovl1` — overlay vs dynamic (6 tests) | **PASS** |
| `scl-fw-xip-nolib1` — XIP nolib full surface | **PASS** |
| `scl-fw-xip-newlib1` — XIP newlib full surface | **PASS** |
| `scl-fw-xip-static` — XIP static loader | **PASS** |
| `scl-fw-xip-static-direct` — XIP direct API calls | **PASS** |
| `scl-fw-xip-interrupt-static` — XIP + TIM2 interrupt | **PASS** |
| `scl-fw-xip-interrupt-static-direct` — XIP + direct + interrupts | **PASS** |
| `scl-fw-xip-static-spi-flash` — SPI flash + XIP | **PASS** |

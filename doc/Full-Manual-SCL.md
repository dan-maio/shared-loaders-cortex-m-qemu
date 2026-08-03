# SCL Cortex-M Loaders — Complete Reference Manual

**Full API, Internal Functions, Execution Paths, and Template Applications**

*Version 1.0 — Generated from source tree `shared-lib-loaders/`*

---

## Table of Contents

1. [Overview & Architecture](#1-overview--architecture)
2. [scl::common — Full Shared Core API](#2-sclcommon--full-shared-core-api)
3. [scl::dynamic-nolib — Full API & Internals](#3-scldynamic-nolib--full-api--internals)
4. [scl::dynamic-newlib — Full API & Internals](#4-scldynamic-newlib--full-api--internals)
5. [scl::static — Full API & Internals](#5-sclstatic--full-api--internals)
   - 5.11 [Direct Calls via GOT/PLT (Build Options)](#511-direct-calls-via-gotplt-build-options)
6. [scl::overlay — Full API & Internals](#6-scloverlay--full-api--internals)
7. [scl::reloc — Full API & Internals](#7-sclreloc--full-api--internals)
8. [scl::xip — Full API & Internals](#8-sclxip--full-api--internals)
9. [common-elf-reader — Full ELF Parsing API](#9-common-elf-reader--full-elf-parsing-api)
10. [Template Application](#10-template-application)
11. [Execution Path / Call Stack Analysis](#11-execution-path--call-stack-analysis)
12. [Text Schemas & Diagrams](#12-text-schemas--diagrams)
13. [Build System](#13-build-system--firmware-tests-and-fixtures)
14. [Advanced Interrupt Handling and Context Switching](#14-advanced-interrupt-handling-and-context-switching)

---

## 1. Overview & Architecture

### 1.1 The Problem

A standard Cortex-M firmware is one frozen binary. To run external code at runtime — plug-ins, downloaded modules, A/B algorithms — you need answers to:

1. **Where do the bytes come from?** (semihosting file, on-chip flash archive, RAM buffer)
2. **Where does the code run?** (RAM arena slot, heap buffer, shared overlay window, flash in-place)
3. **How are names connected?** (GOT/PLT, absolute patching, link-time binding, lazy resolution)

The SCL provides **ten library targets** that implement different answers, plus a shared ELF toolbox (`scl::common`).

### 1.2 The Ten Libraries

**Note:** `scl-dynamic-newlib` and `scl-xip-dynamic-newlib` are now built unconditionally when targeting ARM (no longer guarded by `SCL_HOSTED`). The XIP dynamic-newlib test (`scl-fw-xip-newlib1`) also no longer requires `SCL_HOSTED` — see `test-xip/build_and_run.sh newlib1`.

The six core loaders are augmented by four XIP adapter targets that wrap the base loaders with execute-in-place support:

| Library | Loads | Code Model | Linking | Memory | Float ABI | Needs Heap? |
|---------|-------|-----------|---------|--------|-----------|-------------|
| **`scl::dynamic-nolib`** | ET_DYN `.so` | `-fpic -shared` | GOT/PLT lazy/cross | Fixed arena | Hard-float | No |
| **`scl::dynamic-newlib`** | ET_DYN `.so` | `-fpic -shared` | GOT patching + journal | Heap | Soft/Hard | Yes |
| **`scl::static`** | ET_DYN `.so` (PIC) | `-fpic -shared` | GOT patching + main patch | Static/fixed | Soft/Hard | Yes |
| **`scl::overlay`** | Linker overlays | Non-PIC `-mlong-calls` | Link-time binding | Shared SRAM window | Any | No |
| **`scl::reloc`** | ET_REL `.o` | `-mlong-calls -r` | Absolute ABS32 patching | Contiguous arena | Any | No |
| **`scl::xip-*`** | ET_DYN `.so` | Split R-X/RW | Split-base relocate | Flash (text) + RAM (data) | Varies | Varies |

**XIP adapter targets (wrapping base loaders):**

| Target | Wraps | Description |
|--------|-------|-------------|
| `scl-xip-common` | — | Shared XIP core: segment-map, relocate, compact, cross-call trampoline |
| `scl-xip-dynamic-nolib` | `scl::dynamic-nolib` | XIP adapter for freestanding dlopen |
| `scl-xip-dynamic-newlib` | `scl::dynamic-newlib` | XIP adapter for hosted dlopen |
| `scl-xip-static` | `scl::static` | XIP adapter for static patcher |

### 1.3 Source Layout (paths relative to `../`)

```
shared-lib-loaders/
  include/loader/         -- common core headers (dynamic_info, symbol_resolver, etc.)
  include/scl/            -- firmware-level headers (log, trampoline, arena, link-context)
  include/dynamic-nolib/  -- dlopen public API (dl.hpp, lazy-plt.hpp)
  include/dynamic-newlib/ -- static loader public API (dynamic-linker.hpp, etc.)
  include/static/         -- static patcher API (shlib-loader.h, elf-reader.h)
  include/reloc/          -- ET_REL loader API (reloc-loader.hpp)
  include/overlay/        -- overlay API (overlay.h)
  include/xip/            -- XIP API (segment-map.hpp, relocate.hpp, compact.hpp, xip-handle.hpp)
                           -- XIP adapters: dynamic-nolib/adapter.hpp, dynamic-newlib/adapter.hpp, static/adapter.hpp
  src/                    -- common core implementations
  src/port/               -- Cortex-M assembly port layer
  src/dynamic-nolib/      -- dl.cpp implementation
  src/dynamic-newlib/     -- dynamic-linker, patch-journal, image-memory, link-context
  src/static/             -- shlib-loader, elf-reader, logger, module
  src/reloc/              -- reloc-loader.cpp
  src/overlay/            -- overlay.c + ldscripts/
  src/xip/                -- segment-map, relocate, compact, adapter-base.hpp + adapters per loader
                           -- xip-cross-call.c (shared cross-library trampoline)
  common-elf-reader/      -- git submodule: elf_image, byte_source, error/result
  doc/                    -- documentation (this file lives here)
```

### 1.4 File Path Convention

Throughout this manual, file paths are given **relative to `./doc`** (the directory containing this file). For example, `../src/dynamic-nolib/dl.cpp` means `<repo-root>/src/dynamic-nolib/dl.cpp`.

---

## 2. scl::common — Full Shared Core API

> **Location:** `../include/loader/` (headers), `../src/` (implementations), `../common-elf-reader/` (submodule)

scl::common is the **shared toolbox** that every loader links. Fully portable C++20, host-testable.

### 2.0 Architectural Theory: ELF Loading on Cortex-M

**The ELF standard** defines two relevant object types: `ET_REL` (relocatable `.o` files with unresolved symbol references) and `ET_DYN` (dynamic shared objects, `.so`, designed for load-time relocation). On a desktop OS the kernel ELF loader and the dynamic linker (`ld.so`) collaborate to map segments, resolve symbols, and apply relocations. On Cortex-M **there is no OS, no MMU, and no `ld.so`** — the firmware is the kernel, the loader, and the application all in one.

**Why this matters on Cortex-M:**
- **No MMU:** All addresses are physical. Loaded code must be placed at a real RAM address with no virtual-to-physical translation layer. Position-independent code (PIC) uses PC-relative addressing so the same bytes work at any base address — the loader just adds the runtime base to every absolute reference.
- **Thumb-2 only:** Cortex-M cores execute only Thumb-2 (16/32-bit mixed) instructions. All function addresses must have bit 0 set (Thumb bit). The loader must OR-in bit 0 on every resolved function pointer.
- **Hard-float ABI (optional):** Some Cortex-M4F/M7 cores have a single-precision FPU. The calling convention may pass float arguments in VFP registers (`s0`-`s15`). The loader's trampolines must save/restore these registers around cross-module calls.
- **No ELF loader in hardware:** The CPU does not understand ELF headers, program headers, or section headers. Every byte of parsing — finding `PT_LOAD` segments, iterating the `_DYNAMIC` array, walking the SysV hash chains — is done in software by the SCL common core.

**The Cortex-M memory model** used by all SCL loaders:

```
FLASH (0x08000000, read-only, execute-in-place):
  .text, .rodata, .init_array, .ARM.exidx  (firmware code and read-only data)

RAM (0x20000000, read-write):
  .data, .bss, stack, heap (mutables state)

ARENA (0x20010000, reserved region in RAM):
  Loaded library image slots (scl::dynamic-nolib)
```

The SCL common core provides the purely computational pieces: parse an ELF from bytes, look up a name in a hash table, apply a relocation list to a writable buffer. Every loader then assembles these pieces into a complete load pipeline suited to its trade-offs.

### 2.1 ELF32 Reader/Parser (git submodule)

#### `loader::byte_source` — Storage Abstraction
- **File:** `../common-elf-reader/include/loader/byte-source.hpp:1`
- Abstract random-access byte reader. The core never assumes where object bytes live.
- **Methods:**
  - `virtual bool read_at(uint64_t offset, std::span<std::byte> dest) noexcept = 0` — fill `dest` from `offset`
- **Provided implementations:**
  - `loader::memory_byte_source` — over an in-memory `std::span<const std::byte>` (`../common-elf-reader/include/loader/sources/memory-byte-source.hpp:1`)

#### `loader::elf_image` — ELF File Parser
- **File:** `../common-elf-reader/include/loader/elf-image.hpp:1`, `../common-elf-reader/src/elf-image.cpp:1`
- Validated view over an ELF `.so`: headers, program headers, sections.
- **Static factory:**
  - `static result<elf_image> parse(byte_source& src) noexcept` — parses and validates ELF header
- **Methods:**
  - `const elf::Ehdr& header() const noexcept` — ELF file header
  - `result<void> program_header(half i, elf::Phdr& out) const noexcept` — read i-th program header
  - `result<void> section_header(half i, elf::Shdr& out) const noexcept` — read i-th section header
  - `result<const elf::Shdr*> find_section(string_view name) const noexcept` — find section by name
  - `result<uint32_t> image_size() const noexcept` — total PT_LOAD VA range
  - `result<uint32_t> image_base() const noexcept` — lowest PT_LOAD vaddr
  - *(SCL_HOSTED only)* `result<vector<reloc_entry>> collect_relocations() const noexcept`

#### `loader::elf::` Types — ELF32 Structures
- **File:** `../common-elf-reader/include/loader/elf-types.hpp:1`
- `Ehdr`, `Phdr`, `Shdr`, `Sym`, `Rel`, `Dyn`, `rel_type` enum, `r_type()`, `r_sym()`
- Full ARM ELF32-LE constant definitions

#### `loader::error` / `loader::result<T>` — Error Handling
- **File:** `../common-elf-reader/include/loader/error.hpp:1`
- `enum class error { OK, NOT_ELF, NOT_ARM32, NOT_SHARED_OBJECT, BAD_SECTION, OUT_OF_MEMORY, SHORT_READ, UNSUPPORTED_RELOC, SYMBOL_NOT_FOUND, FILE_NOT_FOUND }`
- `template<typename T> class result<T>` — C++20 expected-like (value or error). Use `loader::unexpected(e)` to construct the failure alternative.

#### `loader::detail::bytes` — Bounded LE Accessors
- **File:** `../common-elf-reader/include/loader/detail/bytes.hpp:1`
- Header-only, bounds-checked little-endian access:
  - `uint32_t load_word(span<const byte>, size_t offset) noexcept`
  - `uint8_t load_byte(span<const byte>, size_t offset) noexcept`
  - `void store_word(span<byte>, size_t offset, uint32_t value) noexcept`
  - `string_view name_at(span<const byte> strtab, uint32_t offset) noexcept` — NUL-terminated string read (never scans past the table)

### 2.2 Dynamic Segment Parser

#### `loader::dynamic_info`
- **File:** `../include/loader/dynamic-info.hpp:1`, `../src/dynamic-info.cpp:1`
- Parses `_DYNAMIC` segment array (DT_HASH, DT_SYMTAB, DT_STRTAB, DT_STRSZ, DT_JMPREL, DT_INIT_ARRAY, ...)
- **Fields:**
  - `uint32_t hash` — DT_HASH value (`.hash` address)
  - `uint32_t symtab` — DT_SYMTAB value (`.dynsym` address)
  - `uint32_t strtab` — DT_STRTAB value (`.dynstr` address)
  - `uint32_t strsz` — DT_STRSZ value
- **Static factory:**
  - `static result<dynamic_info> parse(span<const byte> bytes) noexcept` — iterate DYN entries until DT_NULL
- **Error:** `error::BAD_SECTION` if hash/symtab/strtab all missing

### 2.3 Symbol Resolution

#### `loader::elf_hash`
- **File:** `../include/loader/symbol-resolver.hpp:1`, `../src/symbol-resolver.cpp:1`
- **Signature:** `[[nodiscard]] uint32_t elf_hash(string_view name) noexcept`
- Classic SysV ELF hash of a symbol name

#### `loader::symbol_resolver`
- **File:** `../include/loader/symbol-resolver.hpp:1`, `../src/symbol-resolver.cpp:1`
- Lookup symbols in a (`.dynsym`, `.dynstr`, `.hash`) triple
- **Constructor:**
  - `symbol_resolver(span<const byte> dynsym, span<const byte> dynstr, span<const byte> hash) noexcept`
- **Methods:**
  - `result<uint32_t> address_of(string_view name) const noexcept` — returns **image-relative** symbol value (0 if undefined)

### 2.4 Relocation

#### `loader::reloc_applier`
- **File:** `../include/loader/reloc-applier.hpp:1`, `../src/reloc-applier.cpp:1`
- Allocation-free relocation streaming — writes into loaded image directly (freestanding-safe)
- **Struct: `loader::symbol_source`** — function pointer + context for external resolution
  - `result<uint32_t> (*fn)(void* ctx, string_view name)` — callback
  - `result<uint32_t> operator()(string_view name) noexcept` — invokes callback
- **Static method:**
  ```cpp
  static result<void> apply_rel(
      span<byte> image,         // writable loaded image (vaddr 0 = image[0])
      uint32_t base,            // runtime address of image[0]
      span<const byte> reldyn,  // .rel.dyn table (elf::Rel array)
      span<const byte> dynsym,  // .dynsym (elf::Sym array)
      span<const byte> dynstr,  // .dynstr
      symbol_source resolve     // external symbol resolver
  ) noexcept;
  ```
- **Handles:** `R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`, `R_ARM_ABS32`, `R_ARM_REL32`, `R_ARM_COPY`

#### `loader::relocator`
- **File:** `../include/loader/relocator.hpp:1`, `../src/relocator.cpp:1`
- *Hosted-only* (requires `std::vector`): decodes `.rel.*` into a `relocation_plan`
- Used by host tests and `scl::dynamic-newlib`

### 2.5 Lazy Binding

#### `loader::lazy_tables`
- **File:** `../include/loader/lazy-resolve.hpp:1`
- **Fields:**
  - `span<const byte> jmprel` — JMPREL (JUMP_SLOT relocs, elf::Rel array)
  - `span<const byte> dynsym` — `.dynsym`
  - `span<const byte> dynstr` — `.dynstr`

#### `loader::lazy_resolve_slot`
- **File:** `../include/loader/lazy-resolve.hpp:1`, `../src/lazy-resolve.cpp:1`
- **Signature:**
  ```cpp
  [[nodiscard]] result<uint32_t> lazy_resolve_slot(
      uint32_t got_off,         // offset of GOT slot within image
      const lazy_tables& t,     // JMPREL + dynsym + dynstr tables
      symbol_source resolve     // resolver for external names
  ) noexcept;
  ```
- Scans JMPREL for entry with `r_offset == got_off`, reads symbol name, resolves, returns absolute target with Thumb bit set.

### 2.6 Memory Management

#### `loader::region_allocator`
- **File:** `../include/loader/region-allocator.hpp:1`, `../src/region-allocator.cpp:1`
- Fixed-capacity first-fit free-list allocator over an external region. 8-byte aligned, coalesces adjacent free blocks.
- **Constants:** `kMaxBlocks = 16`, `kAlign = 8`
- **Constructor:** `region_allocator(void* base, size_t size) noexcept`
- **Methods:**
  - `result<void*> alloc(size_t n) noexcept` — allocate (rounded up to kAlign)
  - `void free(void* p) noexcept` — free (no-op for nullptr)
- **Internal:**
  - `struct Block { uint32_t off, len; bool used, live; }`
  - `int find_free_slot() noexcept` — find reusable slot index
  - `void coalesce() noexcept` — merge adjacent free spans

### 2.7 Firmware-Level Helpers

#### `fw::image_arena()`
- **File:** `../include/scl/image-memory.hpp:1`, `../src/image-memory.cpp:1`
- **Signature:** `loader::region_allocator& fw::image_arena() noexcept`
- Returns singleton allocator over linker-defined `_simage_arena_start` / `_simage_arena_end`
- Used by `scl::dynamic-nolib` to allocate image buffers from the reserved RAM arena

#### `fw::link_context`
- **File:** `../include/scl/link-context.hpp:1`, `../src/link-context.cpp:1`
- Provides main firmware's exported symbol table (parsed from firmware's own `_DYNAMIC`)
- **Static factory:**
  - `static result<link_context> from_self() noexcept` — reads `_DYNAMIC` at a fixed link-time address
- **Methods:**
  - `symbol_source source() const noexcept` — get resolver callback
  - `result<uint32_t> resolve(string_view name) const noexcept` — absolute address in main

#### `scl_log` Hook
- **File:** `../include/scl/log.h:1`, `../src/log.c:1`
- **Signatures:**
  - `void scl_log_puts(const char* s)` — emit NUL-terminated string (weak no-op default)
  - `void scl_log_putc(char c)` — emit single character (weak no-op default)
- All SCL diagnostics go through this. Override with strong definitions for tracing.

#### `pic_call_void`, `pic_call_u32`, `pic_call_u32_2`
- **File:** `../include/scl/pic-trampoline.h:1`
- Declared extern "C" — implementations in `../src/port/pic-trampoline.c:1`
- Enter a loaded library with `r10 = got` (the GOT base register)
- **Signatures:**
  - `void pic_call_void(void* fn, void* got)`
  - `uint32_t pic_call_u32(void* fn, void* got, uint32_t a0)`
  - `uint32_t pic_call_u32_2(void* fn, void* got, uint32_t a0, uint32_t a1)`

#### `dl_exidx_register`, `dl_exidx_unregister`
- **File:** `../include/scl/unwind-exidx.h:1`, `../src/port/unwind-find-exidx.c:1`
- Register/unregister loaded-library `.ARM.exidx` ranges for C++ unwinding
- **Signatures:**
  - `void dl_exidx_register(uint32_t img_base, uint32_t img_size, uint32_t exidx_base, int exidx_count)`
  - `void dl_exidx_unregister(uint32_t img_base)`

---

## 3. scl::dynamic-nolib — Full API & Internals

> **Purpose:** Freestanding `dlopen`-style loader for PIC `.so` files. Zero allocation, hard-float, Cortex-M only.
> **Files:** `../include/dynamic-nolib/dl.hpp`, `../src/dynamic-nolib/dl.cpp`, `../src/port/{pic-trampoline.c,dl-cross-call.c,lazy-resolve.c}`

### 3.0 Architectural Theory: PIC, GOT/PLT, and r10 on Cortex-M

**The problem:** A library loaded at an arbitrary RAM address contains code that calls external functions like `printf`. The address of `printf` is not known when the library is compiled. How does the library find `printf` without having its bytes patched for every possible load address?

**Position-Independent Code (PIC):** The library is compiled `-fpic`. Instead of `bl printf` (a direct PC-relative call embedded in the instruction stream), the call goes through an indirection table called the **Global Offset Table (GOT)**. The compiler emits:

```asm
ldr r3, [r10, #offset_to_printf_GOT_slot]   ; r3 = *got_entry
blx r3                                       ; call through the GOT
```

The GOT is a table of 32-bit pointers, one per imported symbol, stored at a known offset from the PIC base register. The runtime loader fills in these pointers when the library is loaded. The library code itself is never modified — only the GOT (which lives in RAM) is patched.

**The GOT/PLT division:**
- **GOT** (`.got`): holds addresses of imported **data** symbols (global variables) and the runtime base address of the library itself. Entries are filled eagerly at load time.
- **PLT** (`.plt`): procedure linkage table — small code stubs that jump through GOT entries. On Cortex-M we use a simplified PLT: each PLTn entry does `add r12, pc, #offset ; ldr pc, [r12]`, where `r12` points to the corresponding `GOT[n]` slot.
- **GOT[0]:** `_DYNAMIC` address
- **GOT[1]:** link_map (unused on bare-metal)
- **GOT[2]:** resolver entry (points at `dl_runtime_resolve` for lazy binding)

**The r10 register convention:** The ARM EABI for PIC designates `r10` as the **static chain / PIC base register** (`-msingle-pic-base`). On Cortex-M, `r10` is a general-purpose register with no fixed role in the base ABI — we are free to use it as the GOT base pointer. The SCL convention:

```
r10 = got_base = library_base + offset_of(.got)
```

Every time the firmware enters a loaded library, r10 holds that library's `got_base`. The library reads all its imports relative to r10. This is what makes the library's code truly position-independent: the same bytes work at any RAM address; only r10 changes per library.

**Why `-fno-pic-data-is-text-relative` must be off:** The library must NOT assume its data segment is at a fixed offset from its text segment (because the loader may place them independently in the arena). By passing `-mno-pic-data-is-text-relative`, the compiler generates GOT-relative data accesses exclusively through r10.

**Specific Cortex-M implementation choices:**

1. **Register save convention:** The trampolines save/restore r10 and lr with `push {r10, lr}` / `pop {r10, pc}`. No other callee-saved registers are touched, so the call is nearly free (2 instructions overhead).

2. **Hard-float VFP save:** The cross-call trampoline (`dl_cross_call`) also saves/restores VFP registers `d0-d7` because a library call may pass float arguments. Without this, a lib→lib call would corrupt the caller's float arguments.

3. **No PLT0:** Standard ARM PLT0 computes the GOT[2] offset relative to the PLT base using a PC-relative calculation that assumes page-aligned segments. On Cortex-M segments are not page-aligned (no MMU). Instead of fixing PLT0, we bypass it entirely: every JUMP_SLOT GOT slot is pointed directly at `dl_runtime_resolve`, and the resolver learns which slot it is from `r12` (which PLTn sets via `add r12, pc, #offset`).

4. **lazy vs eager vs cross-call:** Three binding strategies co-exist:
   - **Eager:** JUMP_SLOT resolved at load time (default for intra-library references)
   - **Lazy:** JUMP_SLOT → `dl_runtime_resolve`, resolved on first call (`dl_fixup` patches the slot)
   - **Cross-call:** External JUMP_SLOT → `dl_cross_call` trampoline, which resolves at call time AND swaps r10 to the callee's got_base (required for lib→lib calls where each library has its own GOT)

### 3.1 Public API (`../include/dynamic-nolib/dl.hpp`)

#### `struct fw::handle`
- **Fields** (18 total):
  - `uint32_t base` — image load bias (runtime base address)
  - `uint32_t size` — allocated image size in arena
  - `uint32_t got_base` — r10 value for entering this library (= `base + .got_offset`)
  - `uint32_t got_off` — image-relative offset of `.got`
  - `uint16_t got_words` — number of 4-byte GOT entries snapshotted
  - `uint32_t got_raw[K_GOT_SNAP_MAX]` — GOT snapshot BEFORE relocation (didactic)
  - `uint32_t fini_array` — absolute address of `.fini_array` (0 if none)
  - `uint32_t fini_count` — number of `.fini_array` entries
  - `loader::symbol_resolver resolver` — this lib's own symbol resolver
  - `bool in_use` — handle slot occupied
  - `uint32_t refcount` — number of other loaded libs importing from this one
  - `uint32_t dep_count` — number of entries in `deps[]`
  - `uint8_t deps[4]` — indices into `g_handles[]` this lib imports from
  - `loader::lazy_tables lazy` — `.rel.dyn` JMPREL/dynsym/dynstr spans for JUMP_SLOT fixup
  - `loader::lazy_tables lazy_plt` — `.rel.plt` JMPREL/dynsym/dynstr spans
  - `uint32_t exidx_base` — runtime address of `.ARM.exidx` (0 if none)
  - `uint32_t exidx_count` — number of 8-byte EHABI entries
  - `char name[12]` — short provenance name (basename without `.so`)

#### `fw::dlopen_source`
- **File:** `../src/dynamic-nolib/dl.cpp:190`
- **Signature:**
  ```cpp
  [[nodiscard]] loader::result<handle*>
  dlopen_source(loader::byte_source& src, const char* rawname) noexcept;
  ```
- **Pipeline:**
  1. Parse ELF via `elf_image::parse(src)` — validated view of the `.so`
  2. `img.image_size()` — total VA range for arena allocation
  3. `fw::image_arena().alloc(size)` — allocate slot from fixed RAM arena
  4. Zero-fill arena slot
  5. Copy each `PT_LOAD` segment to its `p_vaddr` offset in arena
  6. Read section headers + `.shstrtab` into `section_cache` (2 semihosting reads vs ~8 per lookup)
  7. Extract `.dynsym`, `.dynstr`, `.hash` as in-image spans (`sec_in_image`)
  8. `fw::link_context::from_self()` — main's exported symbol table
  9. Allocate `handle` from fixed pool (`alloc_handle`)
  10. Build combined resolver (other loaded libs → main fallback)
  11. Snapshot `.got` BEFORE relocation (for didactic GOT walkthrough)
  12. `loader::reloc_applier::apply_rel()` on `.rel.dyn` and `.rel.plt`
  13. Update refcounts for dependency edges
  14. Set up lazy binding: patch JUMP_SLOT slots → `dl_runtime_resolve`
  15. (Optional) Route external JUMP_SLOTs → `dl_cross_call` for cross-library r10 swap
  16. Compute `got_base = base + .got_offset`
  17. Record `.ARM.exidx` range and register with C++ unwinder
  18. Fix Thumb bit on extab personality pointers
  19. Call constructors (`.init_array` / `.inits`) via `pic_call_void`
  20. Record `.fini_array` for dlclose

#### `fw::dlsym`
- **File:** `../src/dynamic-nolib/dl.cpp:392`
- **Signature:** `[[nodiscard]] loader::result<void*> dlsym(handle& h, const char* name) noexcept`
- Resolves `name` in `h`'s own `.dynsym` and returns absolute address (= `base + st_value`)

#### `fw::dlclose`
- **File:** `../src/dynamic-nolib/dl.cpp:398`
- **Signature:** `void dlclose(handle& h) noexcept`
- Refcount-guarded: refuses if another loaded lib still imports from this one
- Calls `.fini_array` destructors in reverse
- Decrements dependency refcounts
- `dl_exidx_unregister()` — removes EHABI registration
- `image_arena().free()` — returns arena slot
- Resets handle to zero

#### `fw::dl_enable_cross_call`
- **File:** `../src/dynamic-nolib/dl.cpp:558`
- **Signature:** `void dl_enable_cross_call(bool on) noexcept`
- Enables/disables cross-library r10 routing. When ON, external JUMP_SLOTs point at `dl_cross_call` (lib→lib calls swap r10 to callee's `got_base`).

#### `fw::dl_cross_trace`
- **File:** `../src/dynamic-nolib/dl.cpp:559`
- **Signature:** `void dl_cross_trace(int level) noexcept`
- GOT/PLT translation trace verbosity: 0=off, 1=cross-lib hops only, 2=verbose

#### `fw::dl_dump_image`
- **File:** `../src/dynamic-nolib/dl.cpp:567`
- **Signature:** `void dl_dump_image(const handle& h) noexcept`
- Dumps GOT/PLT relocation tables entry-by-entry (slot address, reloc type, symbol, current value, annotation)

#### `fw::dl_dump_main_provides`
- **File:** `../src/dynamic-nolib/dl.cpp:612`
- **Signature:** `void dl_dump_main_provides(const char* const* names, int n) noexcept`
- Prints flash addresses main provides for each name (the target side of lib→main GOT/PLT hops)

#### `fw::dl_dump_got_walkthrough`
- **File:** `../src/dynamic-nolib/dl.cpp:653`
- **Signature:** `void dl_dump_got_walkthrough(const handle& h) noexcept`
- Didactic BEFORE/AFTER GOT walkthrough: each slot annotated with what the loader did (reserved/rebased/bound/routed)

#### `fw::dl_fixup_count`
- **File:** `../src/dynamic-nolib/dl.cpp:417`
- **Signature:** `std::uint32_t dl_fixup_count() noexcept`
- Returns global count of lazy PLT fixups performed (used in test 11 for lazy-binding proof)

#### `class dl` — POSIX-style Facade
- **File:** `../src/dynamic-nolib/dl.cpp:795`
- Wraps `fw::` functions in POSIX `dlopen/dlsym/dlclose/dlerror` interface
- **Methods:**
  - `static void* dlopen(const char* filename, int flag) noexcept`
  - `static void* dlsym(void* handle, const char* symbol) noexcept`
  - `static int dlclose(void* handle) noexcept`
  - `static char* dlerror() noexcept`

### 3.2 Internal Functions (`../src/dynamic-nolib/dl.cpp`)

| Function | Line | Scope | Signature / Description |
|----------|------|-------|------------------------|
| `put_hex` | 35 | macro alias | `#define put_hex scl_log_put_hex` — prints `label0xXXXXXXXX` via the shared helper (`../src/scl-log-put-hex.c`), no libc |
| `put_dec` | 38 | anonymous ns | `void put_dec(uint32_t v)` — print decimal (for entry counts), no libc |
| `reloc_name` | 46 | anonymous ns | `const char* reloc_name(uint8_t t)` — human-readable name for ARM REL relocation type |
| `alloc_handle` | 60 | anonymous ns | `handle* alloc_handle()` — first not-in-use handle from `g_handles[4]`, or nullptr |
| `section_cache::load` | 84 | struct method | `result<void> load(elf_image& img, byte_source& src)` — read section headers + `.shstrtab` in 2 reads |
| `section_cache::find` | 107 | struct method | `const Shdr* find(string_view name)` — cached section by name |
| `section_cache::find_by_type` | 127 | struct method | `const Shdr* find_by_type(uint32_t type)` — first non-empty section of given type |
| `sec_in_image` | 144 | anonymous ns | `span<byte> sec_in_image(cache, mem, name, addr_out)` — allocated section as in-image span |
| `combined_resolver` | 158 | struct | Thunk: resolve against other loaded libs first, then main fallback. Records dependency edges |
| `combined_resolver::thunk` | 164 | struct method | `static result<uint32_t> thunk(void* ctx, string_view name)` — the resolver callback |
| `dl_fixup` | 456 | C linkage | `extern "C" void* dl_fixup(uint32_t* slot)` — lazy-binding resolver: maps GOT slot→JUMP_SLOT→resolve→patch |
| `log_fixup_miss` | 433 | namespace | `void log_fixup_miss(const handle*, uint32_t slot_addr, const char* why)` — log lazy fixup failure |
| `dl_cross_lookup` | 501 | C linkage | `extern "C" uint64_t dl_cross_lookup(uint32_t got_slot_addr)` — resolve GOT slot to {target, callee_got} |
| `got_slot_reloc` | 629 | static | `static uint8_t got_slot_reloc(handle&, uint32_t off, string_view& sym)` — find relocation targeting GOT offset |

### 3.3 Port Layer (`../src/port/`)

#### `pic_call_void`, `pic_call_u32`, `pic_call_u32_2`
- **File:** `../src/port/pic-trampoline.c:1`
- Naked asm trampolines. ABI: `r0=fn, r1=got, r2=a0, r3=a1`
- Pattern: `push {r10,lr}` → `mov r10, r1` → `blx r0` → `pop {r10,pc}`
- EHABI unwind directives (`.fnstart/.fnend`) so C++ exceptions unwind through the frame

#### `dl_cross_call`
- **File:** `../src/port/dl-cross-call.c:1`
- Entered with `r12(ip) = &GOT[n]` (set by PLTn's `add ip, pc`)
- Saves all scratch regs (r4-r7, r10, lr) + VFP (d0-d7)
- Calls `dl_cross_lookup(ip)` → returns `{target|1 (lo 32), callee_got (hi 32)}`
- Swaps `r10 = callee_got` → `blx target` → restores caller r10 → returns

#### `dl_runtime_resolve`
- **File:** `../src/port/lazy-resolve.c:1`
- Entered with `r12(ip) = &GOT[n]`, `lr = caller return`, r0-r3/d0-d7 = original args
- Saves args + VFP → `bl dl_fixup` → restores args → `bx r12` (tail-call target)
- GOT slot patched by `dl_fixup` so subsequent calls bypass the resolver

### 3.4 Handle Pool Internals

- `g_handles[4]` — fixed array of 4 `handle` structs (`../src/dynamic-nolib/dl.cpp:26`)
- `g_cross_call` — bool, `g_cross_trace` — int, `g_fixup_count` — uint32_t (all file-scope globals)
- `g_sc` — single `section_cache` instance reused per dlopen (single-threaded)

---

## 4. scl::dynamic-newlib — Full API & Internals

> **Purpose:** Hosted dynamic loader with patch journal for clean unload. Uses heap (`new[]`), bidirectional linking, `_reloc` table.
> **Files:** `../include/dynamic-newlib/`, `../src/dynamic-newlib/`

### 4.0 Architectural Theory: GOT Patching, _reloc Table, and Journaled Unload

**The problem with GOT indirection:** The `scl::dynamic-nolib` approach (r10-based GOT access) requires every call into a library to go through a `pic_call_*` trampoline (which swaps r10). This adds overhead and requires the firmware itself to be compiled `-fpic` with a reserved r10. What if you want to call loaded functions **directly**, without a trampoline, from a non-PIC firmware?

**GOT patching (eager binding):** Instead of using r10 at runtime, the loader **patches the GOT entries in place** at load time, writing the absolute address of each resolved symbol directly into the GOT slot. The library still reads its imports through the GOT (compiled `-fpic`), but the GOT now contains real absolute addresses. The caller (non-PIC firmware) can call the library with a plain `blx` — no r10 setup needed. However, the library's own r10-based data accesses still need r10 to point at its GOT, so the firmware must still set r10 before entering the library (or the library must be recompiled without `-msingle-pic-base` — a trade-off).

**The `_reloc` table vs `_DYNAMIC` segment:**

On Linux, the dynamic linker finds the firmware's symbol table by reading its `_DYNAMIC` segment (an array of `Dyn` entries at a known virtual address). This works when the ELF loader is the OS kernel, which understands program headers.

On Cortex-M with a non-PIC firmware, **there is no `_DYNAMIC` segment** (it is emitted only for `-pie`/`-shared` objects). To export the firmware's symbols to the runtime loader without `-Wl,-E`, the linker script manually emits a small fixed-format table called `_reloc`:

```ld
_reloc = .;                                    // in .inits section (FLASH)
LONG(LOADADDR(.rel.dyn));  LONG(SIZEOF(.rel.dyn));
LONG(ADDR(.dynsym));       LONG(SIZEOF(.dynsym));
LONG(ADDR(.dynstr));       LONG(SIZEOF(.dynstr));
LONG(ADDR(.hash));         LONG(SIZEOF(.hash));
LONG(ADDR(.inits));        LONG(SIZEOF(.inits));
```

Each row is (address, size) of a section the loader needs. The `link_context` reads this table at boot and exposes the spans to the `dynamic_linker`. This is the **`_reloc`-table model** — simpler and more explicit than parsing `_DYNAMIC`, at the cost of a non-standard linker script fragment.

**The patch journal: transactional revert:**

Every memory modification the loader makes — patching a GOT entry in the loaded library, patching a GOT entry in the firmware itself — is **recorded** in a `patch_journal` before the write. The journal entry is:

```cpp
struct { byte* address; uint32_t original_value; };
```

On `unload()`, the journal is **replayed in reverse**: each entry's `original_value` is `memcpy`'d back over the patched address. This restores every modified byte to its pre-load state, including:
- The loaded library's GOT slots
- The **firmware's own GOT** (which was patched with the library's exported symbols)
- Any `R_ARM_COPY` data copied into main's `.bss`

After the journal revert, the heap buffer holding the library image is freed (`unique_ptr`). The system is bit-for-bit identical to before `load()`.

**Why this matters on Cortex-M:** Unlike a desktop OS where `dlclose` can `munmap` the library (the OS handles cleanup transparently), on bare-metal the loader is responsible for every byte. Without a journal, unloading a library would leave stale function pointers in other libraries' GOTs and stale data in the firmware's GOT — guaranteed to HardFault on the next cross-module call.

**Bidirectional linking (patch_main_got):**

When a loaded library exports a symbol (say `my_algorithm`), and the **firmware itself** has an unresolved GOT entry for that symbol (because it was compiled with `-fpic -Wl,-E` or has a `_reloc` table entry pointing at a weak reference), the loader patches the firmware's own GOT entry to point into the library. This is done by `patch_main_got()`, which iterates the firmware's `.rel.dyn`, finds JUMP_SLOT/GLOB_DAT entries whose symbol the library defines, and writes the library's export address. If the importing library or firmware slot cannot be resolved yet, it goes on a **pending list** (`pending_`), re-resolved when a later library provides the symbol.

### 4.1 Public API

#### `struct firmware::library`
- **File:** `../include/dynamic-newlib/dynamic-linker.hpp:24`
- **Fields:**
  - `image_memory image` — loaded ELF image in RAM
  - `patch_journal journal` — transactional revert journal
  - `uint32_t base_va` — base virtual address of first LOAD segment
  - `uint32_t dynsym_va` — VA of `.dynsym` in the library
  - `uint32_t dynstr_va` — VA of `.dynstr` in the library
  - `uint32_t hash_va` — VA of `.hash` in the library
  - `uint32_t dynsym_sz, dynstr_sz, hash_sz` — section sizes
  - `void* init_array` — pointer to `.init_array` in loaded RAM
  - `size_t init_count` — number of constructors
  - `bool constructed` — whether constructors have been run

#### `class firmware::dynamic_linker`
- **File:** `../include/dynamic-newlib/dynamic-linker.hpp:40`, `../src/dynamic-newlib/dynamic-linker.cpp:1`

**Weak symbol hook:** The loader resolves fallback symbols (libc, C++ runtime, test globals) through a weak hook `scl_lookup_firmware_symbol(const char* name)` rather than a hardcoded `strcmp` table. Applications override this weak default to provide target-specific symbol mapping. See `REFACTORING_SUMMARY.md` for details.

| Method | File:Line | Signature | Description |
|--------|-----------|-----------|-------------|
| `dynamic_linker` | hpp:43 | `dynamic_linker(const link_context& ctx)` | Construct with firmware's link context |
| `load_from_source` | cpp:59 | `result<library*> load_from_source(byte_source& src)` | Parse, load, relocate, construct library. Limit 8 libs. Co-allocates journal. |
| `lookup` | cpp:175 | `result<void*> lookup(library* lib, const char* name)` | Resolve: main→symtab→lib→cross-search |
| `libs` | hpp:58 | `auto& libs()` | Access all loaded libraries |

#### `class firmware::patch_journal`
- **File:** `../include/dynamic-newlib/patch-journal.hpp:1`, `../src/dynamic-newlib/patch-journal.cpp:1`
- Transactional memory modification log
- **Methods:**
  - `result<void> record(byte* address, uint32_t original_value)` — record one word
  - `result<void> record_bytes(byte* address, size_t nbytes)` — record N bytes as word entries (for R_ARM_COPY)
  - `void revert()` — replay recorded patches in reverse (restore original values)

#### `class firmware::image_memory`
- **File:** `../include/dynamic-newlib/image-memory.hpp:1`, `../src/dynamic-newlib/image-memory.cpp:1`
- Heap-based segment loader (`std::unique_ptr<byte[]>`)

#### `class firmware::link_context` (newlib variant)
- **File:** `../include/dynamic-newlib/link-context.hpp:1`, `../src/dynamic-newlib/link-context.cpp:1`
- Reads `_reloc` table (address/size pairs emitted by linker script) instead of parsing `_DYNAMIC`

### 4.2 Internal Functions (`../src/dynamic-newlib/dynamic-linker.cpp`)

| Function | Line | Scope | Description |
|----------|------|-------|-------------|
| `apply_plan` | 311 | dynamic_linker | `result<void> apply_plan(relocation_plan& plan, byte* base, library* lib)` — apply RELATIVE/GLOB_DAT/JUMP_SLOT/ABS32/REL32/COPY, record journal, create pending relocs for unresolved |
| `patch_main_got` | 427 | dynamic_linker | `result<void> patch_main_got(library* lib)` — patch firmware's own GOT with symbols from loaded library |
| `resolve_cross` | 285 | dynamic_linker | `result<uint32_t> resolve_cross(const char* name)` — search all loaded libs' `.dynsym` |
| `run_constructors` | 490 | dynamic_linker | `result<void> run_constructors(library* lib)` — iterate `.init_array`/`.inits` |

### 4.3 Pending Reloc Table

- **File:** `../include/dynamic-newlib/dynamic-linker.hpp:90`
- Records GOT entries set to 0 because no providing library was loaded yet
- Re-resolved when a new library provides the symbol
- **Fields:** `byte* address`, `uint32_t orig_val`, `char name[32]`

---

## 5. scl::static — Full API & Internals

> **Purpose:** Standalone ELF32 static patcher. Loads PIC `.so`, GOT-patches for direct calling, handles DT_NEEDED dependencies.
> **Files:** `../include/static/`, `../src/static/`

### 5.0 Architectural Theory: Static Patching Without an OS

**The problem:** The `dynamic-nolib` and `dynamic-newlib` loaders both assume the firmware is compiled `-fpic` (position-independent) so that loaded libraries can resolve symbols against the firmware's `.dynsym`. But what if the firmware is **not** PIC? Or what if you want a loader that can be bolted onto an existing non-PIC firmware project without recompiling it with `-fpic -Wl,-E`?

**The `scl::static` approach:** Instead of the firmware exporting symbols through a dynamic symbol table, the **loader** is given explicit pointers to the sections it needs to patch (`_reloc` table). It opens the `.so` file as a byte stream (over semihosting), parses its ELF headers, copies its load segments to a RAM buffer, then walks the **library's** `.rel.dyn` and patches every GOT entry with the resolved address. It also walks the **firmware's** `.rel.dyn` and patches the firmware's own references to library symbols.

**Two-stage patching:**

1. **`patch_shlib`** iterates the library's `.rel.dyn`:
   - `R_ARM_RELATIVE`: adds the load bias (`base + A`, where A is the addend)
   - `R_ARM_GLOB_DAT` / `R_ARM_JUMP_SLOT`: looks up the symbol name via SysV hash in the library's own `.dynsym`/`.dynstr`, then resolves against the firmware's `.dynsym` (or a caller-supplied `symbol_table`). The resolved absolute address is written into the library's GOT.
   - `R_ARM_ABS32`: resolves the symbol and adds the addend, writes into the library's section.

 2. **`patch_main`** iterates the firmware's `.rel.dyn`:
    - `R_ARM_COPY`: the library exports a global variable that the firmware has a COPY relocation for. The loader `memcpy`s the library's initialised value into the firmware's `.bss` allocation for that variable.
    - `R_ARM_GLOB_DAT`: the firmware's GOT entry for a library-exported symbol is patched with the absolute address in the library's RAM buffer.
    - **Registration in `global_syms`:** every successfully resolved GOT slot is also registered in the caller-provided `symbol_table` (`global_syms->add_global`). This makes the resolved address visible to **subsequently loaded modules** — for example, `uart_puts` resolved from the first `.so` load becomes available when a second `.so` (e.g. a C++ fixture) needs it.
    - Weak symbols: resolved later via `resolve_weak_function()`, which lazily looks up the name when first called.

**DT_NEEDED dependency resolution:**

The `.so` file may list dependencies via `DT_NEEDED` entries. The loader's `resolve_dependencies()` method calls a caller-supplied callback (`load_dep(sOName, out_loader)`) which loads each dependency recursively. Each loaded dependency produces its own `shlib_loader` instance, stored in `loaded_deps_` so they outlive the resolution phase.

**COPY relocations on Cortex-M:**

A `R_ARM_COPY` relocation is special: the compiler determines that a global variable imported from a shared library should live in the **main executable's `.bss`** (because the main executable's link-time size for that symbol is authoritative). At load time, the dynamic linker copies the initialised value from the library's `.data` into the main's `.bss` slot. The library's GOT entry for that symbol points to main's copy. This is the standard ELF COPY relocation model, and it works the same on Cortex-M — except there is no OS to ensure the copy happens. The SCL `patch_main` handles it explicitly.

**Why no trampoline:** After `patch_shlib` and `patch_main`, every GOT entry in the loaded library contains a real absolute address. The library's code uses `ldr r3, [r10, #offset]` to read its GOT, so r10 must still be set. However, the calling firmware (which is non-PIC and does not use r10) calls the library through the trampoline `pic_call_u32` — exactly as in `scl::dynamic-nolib`. The difference is that the **resolution** happened eagerly at load time, not lazily at first call.

### 5.1 Public API

#### `class elf_loader::shlib_loader`
- **File:** `../include/static/shlib-loader.h:26`, `../src/static/shlib-loader.cpp:1`

| Method | File:Line | Signature | Description |
|--------|-----------|-----------|-------------|
| `load_bin` | cpp:53 | `loader_error load_bin(uint8_t* buffer, uint32_t bufsize)` | Copy PT_LOAD segments into RAM buffer. Scans DT_NEEDED. |
| `read_elf` | cpp:44 | `loader_error read_elf(string_view file)` | Open and parse ELF file |
| `populate_helper_sections` | cpp:69 | `loader_error populate_helper_sections()` | Locate `.inits`, `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` in both lib and main |
| `get_ram_shlib` | cpp:162 | `uint8_t* get_ram_shlib()` | Returns loaded library's RAM base |
| `get_ram_main` | cpp:166 | `uint8_t* get_ram_main()` | Returns main's RAM base |
| `find_shlib_symbols_to_patch` | cpp:183 | `loader_error find_shlib_symbols_to_patch(span<symbol_patch>)` | Build patch array for library symbols |
| `find_main_symbols_to_patch` | cpp:200 | `loader_error find_main_symbols_to_patch(span<symbol_patch>)` | Build patch array for main symbols |
| `patch_shlib` | cpp:270 | `uint32_t patch_shlib(symbol_table* global_syms)` | Fix up library GOT: ABS32, RELATIVE, GLOB_DAT, JUMP_SLOT |
| `patch_main` | cpp:381 | `uint32_t patch_main(symbol_table* global_syms)` | Fix up main's references to library symbols: COPY, GLOBAL, WEAK |
| `execute_constructors` | cpp:496 | `loader_error execute_constructors()` | Run `.init_array` constructor functions |
| `resolve_dependencies` | cpp:489 | `loader_error resolve_dependencies(symbol_table*, load_dep_callback)` | Auto-load DT_NEEDED deps |
| `resolve_weak_function` | h:62 | `unsigned int resolve_weak_function(funcm f)` | Lazy weak-function dispatch |

#### `class elf_loader::elf_reader`
- **File:** `../include/static/elf-reader.h:1`, `../src/static/elf-reader.cpp:1`
- Thin adapter over shared `loader::elf_image`
- **Methods:** `find_symbol_address(name)`, `pass_args()`, helper section maps

#### Cache-coherence barrier — `scl_memory_barrier()`
- **File:** `../include/scl/barrier.h:4`
- DSB 0xf + ISB 0xf on ARM; no-op otherwise
- The static loader calls this after patching shlib and main symbols (`shlib-loader.cpp:394`, `:500`). It replaced the former per-file `sync_after_patching()` helper, which the refactor removed in favour of the shared `scl/barrier.h` macro. *(Note: a stale `void sync_after_patching();` declaration still lingers in `../include/static/shlib-loader.h:91` — dead, unused.)*

### 5.2 Internal Data Structures

```cpp
// ../src/static/shlib-loader.cpp:41
struct init_lds {
    uint32_t reldyn, reldyn_size;     // .rel.dyn address + size
    uint32_t dynsym, dynsym_size;     // .dynsym address + size
    uint32_t dynstr, dynstr_size;     // .dynstr address + size
    uint32_t hash, hash_size;         // .hash address + size
    uint32_t inits, inits_size;       // .init_array address + size
};
```

- `section_map m_shlib{}`, `section_map m_main{}` — cached section locations (each is `helper_section[6]`, since `helper_section_idx::COUNT = 6`)
- `weak_sym_cache_` — vector of `weak_main_sym` for late weak-function resolution
- `loaded_deps_` — vector of unique_ptr to auto-loaded DT_NEEDED dependencies

### 5.11 Direct Calls via GOT/PLT (Build Options)

The firmware can call loaded `.so` functions **directly** — `spi_init()`,
`uart_puts("hello")` — without `dlsym` or `Module::sym()`. The linker
generates PLT stubs and GOT slots; the runtime loader (`patch_main()`)
patches the GOT with the resolved addresses.

#### Required Build Options

| Setting | Why |
|---------|-----|
| **`extern "C"` in headers** | C++ name mangling (`_Z8spi_initv`) won't match the `.so`'s C symbols (`spi_init`). All headers shared between firmware and `.so` must wrap declarations in `extern "C" { }` |
| **`target_link_libraries` with `.so` paths** | The linker creates `.dynsym`, `.rel.dyn`, `.plt`, `.got` sections. CMake bare-metal filters `-l` flags; use absolute `.so` paths in `target_link_libraries` instead |
| **`patch_main()` in loader pipeline** | `shlib_loader::patch_main()` writes each resolved symbol's address into the main firmware's GOT slot. `Module::build()` calls this automatically |
| **GOT copy at startup** | The `Reset_Handler` in `exception-handlers.c` copies `.got` / `.got.plt` from flash to RAM before `main()` runs. The linker script provides `_sgotdata`/`_sgot`/`_esgot` symbols |
| **Linker script with `.got`, `.got.plt`, `.plt`** | `sections-sh.ld` places `.got` / `.got.plt` in RAM (`> RAM AT> FLASH`) and `.plt` in FLASH |

#### What the Linker Produces

```
.dynsym    → 13 entries (spi_init, uart_puts, led_blink, …)
.rel.dyn   → 12 R_ARM_JUMP_SLOT entries (one per function)
.plt       → Thumb PLT stubs in FLASH (0x0800e274)
.got       → GOT slots in RAM (0x20000124), initialised with PLT addresses
.got.plt   → PLT GOT in RAM (0x20000160)
.dynamic   → DT_NEEDED entries for libspi.so, libuart.so, libled.so
```

`patch_main()` iterates `.rel.dyn`, looks up each symbol in the loaded
`.so`'s `.dynsym`, and writes the runtime address into the GOT slot.
After `Module::load_*()` returns, every PLT call goes straight to the
correct function in RAM.

#### CMake Snippet

```cmake
add_executable(scl-fw-drv …)
target_link_libraries(scl-fw-drv PRIVATE scl::static
    ${DRV_DIR}/fixtures/libspi.so
    ${DRV_DIR}/fixtures/libuart.so
    ${DRV_DIR}/fixtures/libled.so
    ${DRV_DIR}/fixtures/libcpp_features.so)   # C++ fixture
# No -fPIC, no --no-dynamic-linker, no -rdynamic needed.
# The .so files must be named lib*.so for the linker to find them.

# C++ fixture build recipe:
set(LIB_CXXFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
add_custom_target(build_cpp_features
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS}
            -c cpp_features.cpp -o cpp_features.o
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} cpp_features.o
            -o libcpp_features.so -shared -nostartfiles -nostdlib
            -Wl,--gc-sections,-S   # -S strips debug sections at link time
    COMMENT "Building libcpp_features.so")
```

**Note:** `-Wl,-S` strips debug sections (`.debug_*`) from the `.so`, reducing a C++ fixture from ~87 KB to ~14 KB — essential when loading from a small on-flash filesystem buffer.

#### main.cpp Usage

```cpp
#include "spi.h"    // extern "C" void spi_init(void);
#include "uart.h"   // extern "C" void uart_puts(const char*);
#include "led.h"    // extern "C" void led_blink(uint32_t);
#include "cpp_features.h"

// Local new/delete so the linker does NOT resolve _Znaj/_ZdlPv from
// libcpp_features.so's PLT — these operators must be callable before
// the .so is loaded (init_syms → symbol_table → new[]).
void* operator new(size_t size)                  { return malloc(size); }
void* operator new[](size_t size)                { return malloc(size); }
void  operator delete(void* p) noexcept           { if (p) free(p); }
void  operator delete[](void* p) noexcept         { if (p) free(p); }
void  operator delete(void* p, size_t) noexcept   { if (p) free(p); }
void  operator delete[](void* p, size_t) noexcept { if (p) free(p); }

static elf_loader::symbol_table g_syms;
static void init_syms() {
    g_syms.add_global("printf",     (uint32_t)printf);
    g_syms.add_global("puts",       (uint32_t)puts);
    g_syms.add_global("malloc",     (uint32_t)malloc);
    g_syms.add_global("free",       (uint32_t)free);
    // …
}

// --- C tests ---
scl::Module mod;
mod.load_file("libspi.so", buf, sz, &g_syms);
spi_init(); spi_format();

// Load uart.so from flash, use it, discard
spi_read(uart_off, g_img, uart_sz);
scl::Module mu;
mu.load_buf(g_img, uart_sz, g_exec, sizeof(g_exec), &g_syms);
uart_init();
uart_puts("UART from flash");

// --- C++ fixture test ---
// Load uart.so again (fresh copy, hardware re-inited)
spi_read(uart_off, g_img, uart_sz);
scl::Module mu2;
mu2.load_buf(g_img, uart_sz, g_exec, sizeof(g_exec), &g_syms);
uart_init();  // re-init after discard, fresh module has initialized=0

// Load C++ fixture at offset 16384 (after uart's code)
spi_read(cpp_off, g_img, cpp_sz);
scl::Module mc;
mc.load_buf(g_img, cpp_sz, g_exec + 16384, sizeof(g_exec) - 16384, &g_syms);

// Direct calls through GOT/PLT (patched by patch_main)
uint32_t r0 = cpp_features_run(0, 10);
uint32_t s  = cpp_features_summary();
cpp_features_print();   // outputs both via printf (semihost) and uart_puts (UART)
```

**Key pattern:** `patch_main()` registers every resolved symbol (e.g. `uart_puts`) in `g_syms`. When a later module (e.g. the C++ fixture) imports `uart_puts`, `patch_shlib()` finds it in `g_syms` and patches the GOT slot correctly — even though `uart_puts` was never in `init_syms()`.

#### Limitations

- PLT stubs are **Thumb code** (Cortex-M4). The linker may emit ARM-mode
  stubs for non-Thumb targets — verify with `objdump -d -j .plt`.
- **CMake for `arm-none-eabi` filters `-l` flags.** Use absolute `.so`
  paths in `target_link_libraries`, not `-lspi`. For non-CMake builds
  (Eclipse, Makefile), `-lsh1 -lsh2` works directly.
- **One GOT slot per symbol, per loaded module.** If the firmware calls
  `spi_init()` but `spi.so` hasn't been loaded yet, the GOT slot
  contains the PLT resolver address (undefined → crash). Always load
  the `.so` before calling its functions.
- **C++ only.** C firmware must use the `Module::sym()` API since C has
  no `extern "C"` mangling distinction.
- **C++ fixture needs local new/delete.** When a C++ `.so` defines
  `operator new[]` / `operator delete[]`, the linker resolves the
  firmware's own `new[]`/`delete[]` to the `.so`'s PLT. This creates a
  bootstrap problem: the symbol table itself uses `new[]` in
  `init_syms()` — before the `.so` is loaded. Define the operators
  locally in the firmware (wrapping `malloc`/`free`) to break the cycle.
- **`.init_array` not generated with `-nostdlib`.** The
  `__attribute__((constructor))` directive does not produce
  `.init_array` entries when the `.so` is compiled with `-nostartfiles
  -nostdlib`. Use explicit exports and call constructors by name, or
  provide a custom `.inits` section via the linker script.

---

## 6. scl::overlay — Full API & Internals

> **Purpose:** Linker-overlay window manager. Time-multiplexes one SRAM window between modules bound at link time.
> **Files:** `../include/overlay/overlay.h`, `../src/overlay/overlay.c`, `../src/overlay/ldscripts/overlay-fragment.ld`

### 6.0 Architectural Theory: Linker Overlays on Cortex-M

**The problem:** A feature set (say, a cryptographic algorithm, an audio decoder, and a sensor fusion pipeline) may each need 32 KB of RAM for code + data. If all three must be resident simultaneously, the RAM budget is 96 KB. But the three features are **never used at the same time** — they are mutually exclusive execution phases. Can they share RAM?

**Overlay theory (from 1960s batch computing):** An overlay is a section of code and data that occupies the same address range as other overlays, with only one resident at a time. The linker arranges this: it places all overlays at the **same VMA** (the overlay window) but at **different LMAs** (the flash addresses where their bytes are stored back-to-back). A small runtime manager copies the bytes from flash into the window on demand. This is purely a link-time and runtime-copy mechanism — **no ELF parsing, no symbol resolution, no GOT**.

**Why overlays on Cortex-M are different from overlays on a MMU system:**
- **No virtual memory:** On a desktop OS, overlays are implemented with `mmap` and page tables. On Cortex-M, the overlay manager does a physical `memcpy` from flash to RAM.
- **Instruction barrier required:** After copying new code into the window, the CPU pipeline may still hold stale instructions from the previous overlay. The manager must execute `DSB` (data synchronization barrier — drain the write buffer) followed by `ISB` (instruction synchronization barrier — flush the pipeline) before jumping into the freshly copied code. Without these barriers, the CPU may execute old instructions (HardFault or silent corruption).
- **No cross-module calls:** The linker enforces `NOCROSSREFS` (no cross references) between overlays — they are never co-resident. An overlay can call the **firmware** (which is always in flash) using `-mlong-calls` (since the window may be out of BL range), but it cannot call another overlay.
- **Shared window address:** All overlay entry points have the same absolute address (the window VMA). This is a debugging surprise: `print address_of_ov1_function` and `print address_of_ov2_function` produce the same value. The **only** thing that differs is which module's bytes are currently in the window.

**The Cortex-M specific implementation in SCL:**

```c
void scl_overlay_load(scl_overlay_region* r, int index) {
    const scl_overlay_module* m = &r->modules[index];
    memcpy(m->vma, m->lma, m->size);          // copy from flash to RAM window
    scl_memory_barrier();                      // DSB + ISB (from scl/barrier.h)
    r->resident = index;
}
```

The `scl_memory_barrier()` macro expands to `DSB 0xf` + `ISB 0xf` on ARM (full-system barriers); off-target it compiles to a no-op. On Cortex-M4 (STM32F411) there is no instruction cache, so no cache maintenance is needed beyond these barriers.

**Linker script side (`overlay-fragment.ld` template):**

The application's linker script must define an `OVERLAY` region:

```ld
MEMORY { OVL (rwx) : ORIGIN = 0x20018000, LENGTH = 16K }

OVERLAY : NOCROSSREFS {
    .ov1 { KEEP(*(.ov1.text)) KEEP(*(.ov1.rodata)) KEEP(*(.ov1.data)) }
    .ov2 { ... }
    .ovcpp { ... }
} >OVL AT>FLASH
_ov1_lma = LOADADDR(.ov1);  _ov1_size = SIZEOF(.ov1);
_ovl_vma = ADDR(.ov1);
```

Each `.ovN` section is compiled separately and placed by the linker into the `OVL` memory region at the same VMA but at different LMAs. The firmware reads `_ovN_lma` (the flash address) and `_ovN_size` (the byte count) to build the `scl_overlay_module` descriptor array.

**Trade-off summary:**
- **Pros:** Zero runtime linking, zero GOT/PLT overhead, direct function calls (with `-mlong-calls`), tiny manager (~50 lines of C), portable to any Cortex-M.
- **Cons:** Modules are frozen into the firmware image at compile time (cannot add new modules at runtime), all modules share one RAM window (only one active at a time), cross-module calls are forbidden by the linker.

### 6.1 Public API

#### `struct scl_overlay_module`
- **File:** `../include/overlay/overlay.h:29`
- **Fields:**
  - `void* vma` — shared window run address (VMA, same for every module)
  - `const void* lma` — flash load address of this module's bytes
  - `uint32_t size` — module size in bytes
  - `const char* name` — short name for tracing

#### `struct scl_overlay_region`
- **File:** `../include/overlay/overlay.h:40`
- **Fields:**
  - `const scl_overlay_module* modules` — descriptor table
  - `int count` — number of entries
  - `int resident` — index of loaded module, or `SCL_OVERLAY_NONE`

#### `scl_overlay_init`
- **File:** `../src/overlay/overlay.c:10`
- **Signature:** `void scl_overlay_init(scl_overlay_region* r, const scl_overlay_module* modules, int count)`
- Initialize region: set module table, mark no-resident

#### `scl_overlay_load`
- **File:** `../src/overlay/overlay.c:17`
- **Signature:** `int scl_overlay_load(scl_overlay_region* r, int index)`
- Load module at `index`: `memcpy(LMA→VMA)` + DSB/ISB barriers. No-op if already resident.
- Returns 0 on success, -1 if index out of range.

#### `scl_overlay_current`
- **File:** `../src/overlay/overlay.c:34`
- **Signature:** `int scl_overlay_current(const scl_overlay_region* r)`
- Returns index of resident module, or `SCL_OVERLAY_NONE`

### 6.2 Shared Helpers Used by the Overlay Manager

The overlay manager no longer defines its own barrier/hex-print helpers; the
refactor extracted them into shared components reused across loaders:

| Function | File | Description |
|----------|------|-------------|
| `scl_memory_barrier` | `../include/scl/barrier.h:4` | DSB 0xf / ISB 0xf — drain write buffer, flush pipeline. Compiles away to a no-op off-target. |
| `scl_log_put_hex` | `../src/scl-log-put-hex.c:3` | Print `"label0xXXXXXXXX"` through the scl_log hook |

### 6.3 Linker Script Template

- **File:** `../src/overlay/ldscripts/overlay-fragment.ld:1`
- `OVERLAY : NOCROSSREFS` block aliasing one SRAM region
- Exports per-module symbols: `_ovN_lma`, `_ovN_size`, window `_ovl_vma`
- NOCROSSREFS guarantees modules never reference each other at link time

---

## 7. scl::reloc — Full API & Internals

> **Purpose:** Loader for ELF32 relocatable (ET_REL) modules — kernel-module style, no GOT/PLT/r10.
> **Files:** `../include/reloc/reloc-loader.hpp`, `../src/reloc/reloc-loader.cpp`

### 7.0 Architectural Theory: Relocatable Objects on Cortex-M

**The problem:** ET_DYN loaders (`scl::dynamic-nolib`, `scl::dynamic-newlib`, `scl::static`) all consume `-fPIC -shared` objects. These require the firmware to export symbols (via `-Wl,-E` or `_reloc` table), maintain a GOT, and use r10-based trampolines. The GOT indirection costs RAM (one word per imported symbol) and runtime (one extra load per external access). Can we eliminate the indirection entirely?

**ET_REL theory (kernel-module model):** A relocatable object (`.o` file, ELF type `ET_REL`) is the compiler's output **before** the final link. It still has undefined symbol references and placeholder addresses. The Linux kernel loads kernel modules this way: it places the module's sections in memory, resolves symbols against the kernel's export table, and applies `R_X86_64_64` / `R_X86_64_PC32` relocations in place — no PLT, no GOT, no lazy binding.

**The same approach on Cortex-M:**

The loader (`scl::reloc`) operates on `-mlong-calls -fno-pic -r` objects:

- **`-fno-pic`:** The module does NOT use position-independent code. Every external reference is an absolute address (loaded from a literal pool: `ldr rN, =target`). The loader fills in the real address at load time.
- **`-mlong-calls`:** Every call is emitted as `ldr rN, =target ; blx rN`, which can reach any 32-bit address. This removes the ±16 MB range limitation of `BL` — essential because the module runs in RAM (0x20000000) but may need to call firmware functions in flash (0x08000000).
- **`-r`:** The object stays relocatable. Symbol references remain as `R_ARM_ABS32` relocations instead of being resolved by the linker.

**What the loader does (three phases):**

1. **Place (phase 1 — `place_module`):** Iterate all `SHF_ALLOC` sections. For each, copy the section bytes from the object file into the RAM arena (contiguously, respecting alignment). `.bss`-type sections (SHT_NOBITS) are zero-filled. Record each section's runtime address in `sec_base[i]`. This is the only phase that writes memory.

2. **Resolve + apply (phase 2 — `apply_relocs`):** For each `SHT_REL` section, walk the relocation entries and apply them:
   - `R_ARM_ABS32` / `R_ARM_TARGET1`: read the addend `A` from the section, compute `S = symbol_value` (section_base + st_value, or resolved by callback), write `*P = S + A`.
   - `R_ARM_REL32`: `*P = S + A - P` (relative to the relocated address).
   - `R_ARM_THM_CALL`: decode the Thumb BL instruction's in-place addend `A`, compute the target offset `(S&~1) + A - P`, re-encode the instruction.

3. **Construct (phase 3 — `run_ctors`):** Walk `.init_array` and call each constructor through its now-resolved absolute address.

**Symbol resolution model:**

Defined symbols (those with `shndx != SHN_UNDEF`) resolve to `sec_base[shndx] + st_value`. If the symbol is a function (`STT_FUNC`), the Thumb bit (1) is OR'd in.

Undefined symbols (`shndx == SHN_UNDEF`) are resolved through the caller-provided `loader::symbol_source` callback — the same interface used by `reloc_applier`. The firmware provides this callback with its own export table.

**Why `load_set` exists for cyclic dependencies:**

Single `load()` places one module and resolves its externals immediately. If module A calls module B and B calls A, neither can be loaded alone — each will find the other's symbols unresolved. `load_set()` solves this with a **two-phase** approach:

1. **Phase 1:** Place ALL modules in the set (contiguously in the arena). Now every symbol in every module has a known runtime address.
2. **Phase 2:** Resolve each module's relocations against the entire set first (the cross-module resolver `set_resolve` searches all placed modules), then against the external resolver for anything still undefined.

This yields **bit-identical behaviour** to the ET_DYN `dl_cross_call` approach but without any GOT/PLT indirection — the eager twin of the lazy cross-call model.

**What the firmware does NOT need (contrast with ET_DYN loaders):**

| Feature | ET_DYN (`scl::dynamic-*`) | ET_REL (`scl::reloc`) |
|---------|--------------------------|----------------------|
| Firmware PIC | Yes (`-fpic -pie`) | No (plain `-fno-pic`) |
| Symbol export | `-Wl,-E` or `_reloc` table | Caller-provided `symbol_source` |
| GOT in RAM | Yes (patched at load) | No |
| r10 trampoline | Yes (`pic_call_*`) | No (direct call) |
| `_DYNAMIC` segment | Yes (parsed at boot) | No |
| Linker script | Complex (`.dynsym`, `.got`, `.rel.dyn` preserved) | Minimal (standard Cortex-M layout) |
| Text size | ~150 KB (with HAL/newlib) | ~10 KB (bare minimal) |

### 7.1 Public API

#### `struct reloc::module`
- **File:** `../include/reloc/reloc-loader.hpp:43`
- **Fields:**
  - `span<byte> arena` — RAM the sections were placed in
  - `span<const byte> image` — original ET_REL file bytes
  - `uint32_t sec_base[MAX_SECTIONS]` — runtime base per section index
  - `uint32_t used` — bytes of arena consumed
  - `uint16_t shnum` — section-header count
  - `uint32_t shoff` — section-header table offset
  - `uint16_t shentsize` — section-header entry size
  - `uint16_t symtab_idx` — section index of `.symtab`
  - `bool valid`
  - `explicit operator bool() const noexcept`

#### `struct reloc::module_set`
- **File:** `../include/reloc/reloc-loader.hpp:75`
- **Fields:** `module mods[MAX_SET]`, `int n`

#### `reloc::load`
- **File:** `../src/reloc/reloc-loader.cpp:238`
- **Signature:**
  ```cpp
  loader::result<module> load(
      span<const byte> image,     // ET_REL file bytes
      span<byte> arena,           // caller-supplied RAM arena
      loader::symbol_source resolve  // resolver for externals
  ) noexcept;
  ```
- Pipeline: `place_module` → `apply_relocs` → `run_ctors`

#### `reloc::load_set`
- **File:** `../src/reloc/reloc-loader.cpp:248`
- **Signature:**
  ```cpp
  loader::result<module_set> load_set(
      span<const span<const byte>> images,  // set of ET_REL files
      span<byte> arena,
      loader::symbol_source resolve
  ) noexcept;
  ```
- Two-phase: place all → resolve all (cross-module + external) → run all ctors
- Handle mutual/cyclic dependencies (unlike single `load`)

#### `reloc::sym`
- **File:** `../src/reloc/reloc-loader.cpp:267`
- **Signatures:**
  - `uint32_t sym(const module& m, string_view name) noexcept` — search one module
  - `uint32_t sym(const module_set& s, string_view name) noexcept` — search across set

### 7.2 Internal Functions (`../src/reloc/reloc-loader.cpp`)

| Function | Line | Description |
|----------|------|-------------|
| `load16` | 50 | `uint16_t load16(span, uint32_t off)` — read 2-byte LE value |
| `load8` | 55 | `uint8_t load8(span, uint32_t off)` — read 1-byte value |
| `align_up` | 58 | `uint32_t align_up(uint32_t v, uint32_t a)` — align up |
| `struct sh_reader` | 63 | Helper: read Shdr fields by offset. Method `f(i, field)` |
| `struct sym_env` | 75 | Symbol resolution context for ET_REL: `addr_of(si)` resolves one symbol index |
| `sym_env::addr_of` | 84 | Resolve UND→resolver, SHN_ABS→value, defined→sec_base+value (STT_FUNC→+Thumb bit) |
| `apply_thm_call` | 104 | `void apply_thm_call(byte* p, uint32_t S, uint32_t P)` — encode R_ARM_THM_CALL with in-place addend |
| `place_module` | 131 | Phase 1: place SHF_ALLOC sections in arena. Reads Shdr, copies/memsets sections |
| `apply_relocs` | 170 | Phase 2: walk SHT_REL sections, apply ABS32/TARGET1/REL32/THM_CALL |
| `run_ctors` | 210 | Phase 3: iterate `.init_array` and call each constructor |
| `struct set_ctx` / `set_resolve` | 229 | Cross-module resolver for load_set: search all placed modules first, then external |

### 7.3 Relocation Types Handled

| Type | Value | Handler |
|------|-------|---------|
| `R_ARM_ABS32` | 2 | `*P = S + A` (A in-place) |
| `R_ARM_TARGET1` | 38 | Same as ABS32 (bare-metal) |
| `R_ARM_REL32` | 3 | `*P = S + A - P` |
| `R_ARM_THM_CALL` | 10 | Thumb BL: decode A, `off = (S&~1) + A - P`, re-encode |

### 7.4 Constants and Limits

- `MAX_SECTIONS = 48` — fixed cap on section-header count (no heap)
- `MAX_SET = 4` — max modules linked together by `load_set()`
- `ET_REL = 1`, `EM_ARM = 40`, `SHT_SYMTAB = 2`, `SHT_INIT_ARRAY = 14`, `SHF_ALLOC = 2`
- `SHN_UNDEF = 0`, `SHN_ABS = 0xfff1`, `STT_FUNC = 2`
- All defined at `../src/reloc/reloc-loader.cpp:22-38`

---

## 8. scl::xip — Full API & Internals

> **Purpose:** Execute-In-Place loaders. Keep R-X segments in flash, copy only RW to RAM.
> **Files:** `../include/xip/`, `../src/xip/`

### 8.0 Architectural Theory: Execute-In-Place on Cortex-M

**The problem with RAM-only loading:** Every dynamic loader above copies the **entire** library into RAM — both the code (R-X, read-execute) and the data (RW, read-write). On a Cortex-M with 128 KB of RAM this is expensive: a 48 KB library consumes 37% of RAM just for code that never changes. Can we execute code **directly from flash** and only copy the mutable data to RAM?

**XIP theory (Harvard vs von Neumann):** Desktop CPUs use a von Neumann architecture with a unified address space but separate caches for instructions and data. Cortex-M uses a **modified Harvard** architecture: flash is on a separate bus from RAM, but both are accessible from a single 32-bit address space (flash at `0x08000000`, RAM at `0x20000000`). The CPU can fetch instructions from either bus — it does not need code to be in RAM. This makes true XIP (execute from flash) feasible without an MMU.

**What prevents a loaded `.so` from executing directly from flash:**

1. **Segment fusion:** Many toolchains emit a single fused `PT_LOAD` segment containing both `.text` (R-X) and `.data`/`.got` (RW) with `RWX` permissions. A single fused segment cannot XIP because the writable part (GOT, data) is interleaved with read-only code — you would have to make the entire flash region writable, which violates the flash hardware protection.

   **Solution:** The `xip::classify_segments()` function requires exactly **two** `PT_LOAD` segments: one with `PF_X & !PF_W` (R-X, the flash candidate) and one with `PF_W` (RW, always copied to RAM). This is achieved by linking the `.so` with a **split linker script** (`xip-split.ld`) that forces code and data into separate segments.

2. **Relocations targeting the text segment:** Even with cleanly split segments, some relocations may write into the R-X range (e.g., the compiler emitted a relocation in `.text` that the loader must patch). Flash is read-only at runtime — any relocation targeting the R-X segment disqualifies XIP.

   **Solution:** `xip::text_is_immutable()` scans every `SHT_REL` section and verifies that every `r_offset` points to a writable section (`SHF_WRITE`). If even one relocation targets a non-writable section (the R-X range), the module cannot XIP and must be copied whole to RAM.

3. **PC-relative GOT addressing:** The PLT stubs in flash compute their GOT slot address PC-relative: `add r12, pc, #offset`. This works only if the GOT's virtual address equals its file offset — i.e., `data_vaddr == data_off`. The split linker script must honour this constraint.

   **Solution:** `xip::classify_segments()` additionally checks `VA == file_offset` for both segments. Without this, the PC-relative calculation in flash would compute a wrong RAM address for the GOT slot.

**The XIP memory model:**

```
FLASH (memory-mapped, read-only):
  ┌──────────────────────────────┐
  │ R-X segment (.text, .rodata, │
  │ .plt, .got)                  │  ← executed here, in place
  │ text_base = flash_addr       │
  │ text_vaddr == text_off       │
  └──────────────────────────────┘

RAM (allocated by loader):
  ┌──────────────────────────────┐
  │ RW segment (.data, .bss,     │  ← only mutable part copied
  │ .got.plt, .data.rel.ro)      │     to RAM
  │ data_base = ram_addr         │
  │ data_vaddr == data_off       │
  │                              │
  │ got_base = data_base +       │
  │   (.got_vaddr - data_vaddr)  │  ← r10 points here
  └──────────────────────────────┘
```

**Split-base relocation (`xip::apply_rel_xip`):**

With one load bias (standard PIC loading), `R_ARM_RELATIVE` adds a single base: `*P = A + base`. With XIP there are **two bases**: `text_base` (flash address of the R-X segment) and `data_base` (RAM address of the RW segment). The relocation applier must decide per-addend:

```
if in_text(A) => *P = A + (text_base - text_vaddr)
if in_data(A) => *P = A + (data_base - data_vaddr)
```

The `SegmentMap::in_text()` and `SegmentMap::in_data()` methods provide this classification. Externally defined symbols (imported from the firmware) are routed to the appropriate base based on their `st_value` falling inside the text or data VA range.

**XIP cross-library calls:**

Because the GOT in flash is **read-only** (the PLT reads it PC-relative), the loader cannot pre-patch JUMP_SLOT entries with resolved addresses — the flash GOT is immutable at runtime. Instead, every external JUMP_SLOT is pointed at `xip_cross_call` (a trampoline). At call time, the trampoline:

1. Maps the GOT slot address back to a symbol name (using the `.rel.dyn`/`.rel.plt` tables in flash)
2. Resolves the symbol (other loaded libs first, then main)
3. Returns `{target|Thumb-bit, callee_got}`
4. Swaps r10 to the callee's `got_base` and calls the target

This is identical to the `dl_cross_call` mechanism in `scl::dynamic-nolib`, but the GOT lookup reads from the **flash-resident** GOT copy (at `flash_data_base`) rather than the RAM copy. The `xip_cross_lookup()` function in `xip/nolib/adapter.cpp` handles this distinction.

**Impact on RAM and flash:**

| Metric | Copy-whole (no XIP) | XIP (split) | Saving |
|--------|--------------------|-------------|--------|
| RAM per library | 48 KB (code + data) | 4 KB (data only) | **44 KB (92%)** |
| Flash per library | 48 KB (one copy in flash archive) | 48 KB (already in flash, used in-place) | 0 (flash is the same) |
| Load time | 48 KB memcpy | 4 KB memcpy | **92% faster** |
| Cross-call overhead | `dl_cross_call` r10 swap | `xip_cross_call` r10 swap (same) | none |

### 8.1 Core XIP API

#### `struct xip::SegmentMap`
- **File:** `../include/xip/segment-map.hpp:16`
- **Fields:**
  - `uintptr_t text_vaddr, text_off, text_size, text_align` — R-X segment
  - `uintptr_t data_vaddr, data_off, data_filesz, data_memsz` — RW segment
  - `bool split` — false = fused RWX blob, cannot XIP
- **Methods:**
  - `bool in_data(uintptr_t vaddr) const noexcept` — true if vaddr in RW range
  - `bool in_text(uintptr_t vaddr) const noexcept` — true if vaddr in R-X range (and not in data)

#### `xip::classify_segments`
- **File:** `../src/xip/segment-map.cpp:16`
- **Signature:** `bool classify_segments(span<const byte> image, SegmentMap& out) noexcept`
- Parse program headers: find exactly one R-X (PF_X & !PF_W) and one RW (PF_W) PT_LOAD
- Requires `VA == file_offset` for both (PLT PC-relative GOT computation)
- Returns false for fused RWX or multi-segment images

#### `xip::text_is_immutable`
- **File:** `../include/xip/relocate.hpp:23`, `../src/xip/relocate.cpp:1`
- **Signature:** `bool text_is_immutable(span<const byte> image, const SegmentMap& seg) noexcept`
- Scans all SHT_REL sections: verifies every relocation targets a writable section (SHF_WRITE)
- If any reloc targets the R-X range → false (module disqualifies XIP)

#### `xip::apply_rel_xip`
- **File:** `../include/xip/relocate.hpp:30`, `../src/xip/relocate.cpp:1`
- **Signature:**
  ```cpp
  loader::result<void> apply_rel_xip(
      const SegmentMap& seg,     // segment classification
      uintptr_t text_base,       // flash address of text_vaddr
      uintptr_t data_base,       // RAM address of data_vaddr
      span<byte> ram_data,       // RW data in RAM
      span<const byte> reldyn,   // .rel table
      span<const byte> dynsym,   // .dynsym
      span<const byte> dynstr,   // .dynstr
      loader::symbol_source resolve  // external resolver
  ) noexcept;
  ```
- Split-base relocation: R_ARM_RELATIVE addend routed to `text_base` or `data_base` per `in_text()/in_data()`
- Only RW words written (r_offset must be in data segment)

#### `xip::compact_for_xip`
- **File:** `../include/xip/compact.hpp:30`, `../src/xip/compact.cpp:11`
- **Signature:** `bool compact_for_xip(span<const byte> so_image, Module& out) noexcept`
- Checks `classify_segments` (clean split) AND `text_is_immutable` (no writable-target relocs)
- If both pass: `out.text = R-X subspan` (flash), `out.xip = true`
- Otherwise: `out.xip = false` (caller must copy whole to RAM)

### 8.2 XIP Adapters

The XIP adapters share a common base extracted into `src/xip/adapter-base.hpp`. This header provides template/inline implementations of `dlsym`, `get_got_base`, `is_xip`, `get_xip_map`, and `patch_main_imports` — all of which are identical between the dynamic-nolib and dynamic-newlib adapters. Each per-loader adapter instantiates these for its own handle type.

**Shared components:**
- `../src/xip/adapter-base.hpp` — template implementations of `dlsym`, `get_got_base`, `is_xip`, `get_xip_map`, `patch_main_imports`
- `../src/xip/xip-cross-call.c` — shared cross-library call trampoline (parameterized by lookup function name via `XIP_CROSS_LOOKUP` / `XIP_CROSS_FN` macros)
- `../include/xip/xip-handle.hpp` — shared `xip::xip_map` struct and `xip::basic_xip_handle<FallbackT>` template
- Both dynamic adapters (`nolib`, `newlib`) include this base rather than duplicating the code

#### `xip::nolib::dlopen` (dynamic-nolib adapter)
- **File:** `../src/xip/dynamic-nolib/adapter.cpp`
- **Signature:** `void* dlopen(span<const byte> so_image, const char* name) noexcept`
- XIP path: copy RW to RAM, `apply_rel_xip`, register exidx, run ctors, set r10 = data_got
- Fallback: copy whole to RAM, apply relocs manually, register exidx, run ctors

#### `xip::nolib::dlsym`
- Delegate to `xip::detail::dlsym_impl<xip_handle>()` in `adapter-base.hpp`
- Resolve via handle's symbol_resolver, apply split-base offset

#### `xip::nolib::dlclose`
- Inline in `adapter.cpp` (unregister exidx, free RAM arena, reset handle)

#### `xip::nolib::xip_cross_call` / `xip_cross_lookup`
- **File:** `../src/xip/xip-cross-call.c` (shared, compiled with `XIP_CROSS_LOOKUP=xip_cross_lookup`, `XIP_CROSS_FN=xip_cross_call`)
- The nolib dynamic loader compiles a thin wrapper `src/port/dl-cross-call.c` which `#define`s `XIP_CROSS_LOOKUP=dl_cross_lookup` and includes the shared trampoline
- XIP cross-library trampoline. Resolves GOT slot → symbol → callee address → swaps r10 → calls

#### `xip::nolib::patch_main_imports`
- Delegate to `xip::detail::patch_main_imports_impl()` in `adapter-base.hpp`
- Point each external JUMP_SLOT at `xip_cross_call` trampoline

#### XIP for dynamic-newlib and static
- `../src/xip/dynamic-newlib/adapter.cpp` — XIP adapter for dynamic-newlib (complete 436-line implementation)
- `../src/xip/static/adapter.cpp` — XIP adapter for static loader (complete 407-line implementation with `bind_host_imports()`, `load()`, `resolve()`, `unload()`, `xip_cross_lookup()`)
- Dynamic adapters share the same pattern: classify, apply_rel_xip (or fallback), run ctors with split bases

### 8.3 XIP Linker Script Architecture & Offset Cancellation (`xip-split.ld`)

To achieve true execute-in-place without copying code to RAM, the shared library must be linked with a specialized linker script (`xip-split.ld`). Standard toolchains merge code and data into a single `PT_LOAD` segment or emit arbitrary file offsets, which breaks XIP.

#### The `PHDRS` Block

The linker script explicitly defines two distinct program headers:

```ld
PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X: PF_R | PF_X (4 + 1 = 5) — stays in flash */
  data PT_LOAD FLAGS(6);   /* RW:  PF_R | PF_W (4 + 2 = 6) — copied to RAM */
}
```

- **`text PT_LOAD FLAGS(5)`**: `FLAGS(5)` specifies `PF_R (4) | PF_X (1) = 5` (Read-Execute). This segment contains `.text`, `.rodata`, `.plt`, `.ARM.extab`, `.ARM.exidx`, `.hash`, `.dynsym`, `.dynstr`, and `.rel.dyn`. It remains in flash memory (`0x08xxxxxx` range) and is executed in place (`(*F*)`).
- **`data PT_LOAD FLAGS(6)`**: `FLAGS(6)` specifies `PF_R (4) | PF_W (2) = 6` (Read-Write). This segment contains `.data`, `.bss`, `.got`, `.data.rel.ro`, `.init_array`, and `.dynamic`. The loader allocates RAM (`0x20xxxxxx` range) and copies **only** this segment (`(*R*)`).

#### `SIZEOF_HEADERS` and Virtual Address Offset Cancellation

A critical constraint for XIP on ARM Cortex-M is that PLT stubs in flash compute their GOT slot addresses using PC-relative arithmetic: `add r12, pc, #offset`. This requires that the virtual address relative to file offset is identical for both code and data (`text_vaddr - text_off == data_vaddr - data_off`).

In `xip-split.ld`, this is guaranteed by setting the section origin to `SIZEOF_HEADERS`:

```ld
SECTIONS
{
  . = SIZEOF_HEADERS;

  /* ===== R-X Segment (stays in Flash) ===== */
  .hash           : { *(.hash) } :text
  .dynsym         : { *(.dynsym) } :text
  .dynstr         : { *(.dynstr) } :text
  .rel.dyn        : { *(.rel*) } :text
  .text           : { *(.text .text.*) } :text
  .rodata         : { *(.rodata .rodata.*) } :text
  .plt            : { *(.plt) } :text
}
```

Setting `. = SIZEOF_HEADERS` forces `text_vaddr == text_off` (cancelling the offset: `text_vaddr - text_off = 0`). Consequently:

$$\text{data\_vaddr} - \text{data\_off} = \text{text\_vaddr} - \text{text\_off} = 0$$

Because the offset difference cancels to zero, PC-relative calculations executed from flash evaluate directly to the exact physical GOT slot location in RAM without requiring runtime offset translation!

#### Quantitative RAM Footprint Reduction

| Load Mode | Flash Usage | RAM Usage | RAM Reduction |
|-----------|-------------|-----------|---------------|
| **Copy-Whole (standard PIC)** | 48 KB (flash archive) | 48 KB (full code + data copy) | 0% (baseline) |
| **XIP Split Model (`xip-split.ld`)** | 48 KB (executed in flash) | **1.2 KB – 4 KB** (RW data & GOT only) | **>92% RAM Savings** |

### 8.4 Didactic XIP Complex Walkthrough Results & Symbol Resolution Evolution

The complex XIP walkthrough suite (`walkthrough_complex.cpp`) executes a multi-module load of `c2.so` and `cpp3.so`, verifying symbol resolution and tracking slot addresses step-by-step from GOT/PLT initialization to execution.

#### Memory Placement & Address Region Markers

To visually trace memory regions, diagnostic logs use explicit region markers:
- **`(*F*)`**: Addresses residing in **Flash** memory (`0x08xxxxxx` range, read-only / in-place code).
- **`(*R*)`**: Addresses residing in **SRAM / RAM** (`0x20xxxxxx` range, mutable data & relocated GOT entries).

When `c2.so` and `cpp3.so` are loaded:
```
[1] Segment Mapping:
    c2.so:
      .text/.rodata/PLT  0x0800a200 (*F*)  4120 B
      .got/.data/.bss    0x20001000 (*R*)  480 B
    cpp3.so:
      .text/.rodata/PLT  0x0800c000 (*F*)  8450 B
      .got/.data/.bss    0x20001200 (*R*)  610 B
```

#### Step-by-Step Symbol Resolution & Patching Evolution

##### Stage 0 — Initial GOT State (BEFORE Relocation in Flash)
In the raw compiled `.so` file in flash, GOT entries contain initial un-rebased file offsets or placeholder pointers:
```
  Initial GOT values BEFORE relocation:
    [ 0] Slot VMA=0x00001000: 0x00000f00 (*F*)  (reserved)
    [ 1] Slot VMA=0x00001004: 0x00000000        (reserved)
    [ 2] Slot VMA=0x00001008: 0x00000000        (reserved)
    [ 3] Slot VMA=0x0000100c: 0x00000214 (*F*)  (RELATIVE -> rebase)
    [ 4] Slot VMA=0x00001010: 0x00000000        (to be patched: cpp3_method)
```

##### Stage 1 — Relocated GOT State in RAM (AFTER Loader Relocation)
The loader allocates RAM for the RW segment (`0x20001000 (*R*)`), copies `.got`/`.data`, and applies split-base relocations (`xip::apply_rel_xip`):
- `R_ARM_RELATIVE` slots are rebased by adding `data_base` (`0x20001000 (*R*)`).
- `R_ARM_GLOB_DAT` / `R_ARM_ABS32` slots are resolved to host or peer symbol addresses:
```
  GOT values AFTER relocation (current state in RAM):
    [ 0] RAM=0x20001000 (*R*): 0x00000f00 (*F*) -> 0x0800a200 (*F*)  (reserved by dynamic linker)
    [ 1] RAM=0x20001004 (*R*): 0x00000000        -> 0x20001000 (*R*)  (callee got_base)
    [ 2] RAM=0x20001008 (*R*): 0x00000000        -> 0x08001421 (*F*)  (resolver trampoline)
    [ 3] RAM=0x2000100c (*R*): 0x00000214 (*F*) -> 0x20001214 (*R*)  <- rebased (c2_var)
    [ 4] RAM=0x20001010 (*R*): 0x00000000        -> 0x0800c145 (*F*)  <- resolved (cpp3_method)
```

##### Stage 2 — PLT / Cross-Library JUMP_SLOT Routing
For function calls crossing library boundaries (e.g. `c2` calling `cpp3_method` or math/libc functions), `R_ARM_JUMP_SLOT` relocations in the RAM GOT (`(*R*)`) point to the `xip_cross_call` resolver or directly resolved code entry points (`(*F*)`):
```
  [PLT] function slots reached via the PLT (GOT JUMP_SLOTs):
    .rel.plt GOT[0x0010] RAM=0x20001010 (*R*): 0x00000000 -> 0x0800c145 (*F*)  cpp3_method
    .rel.plt GOT[0x0014] RAM=0x20001014 (*R*): 0x00000000 -> 0x080021a9 (*F*)  sinf
```

##### Stage 3 — Execution & Parameter Passing
During execution (`cpp3_method(seed, x)`):
1. **`r10` Register Set**: `r10` is loaded with `cpp3.so`'s `got_base` (`0x20001200 (*R*)`).
2. **Instruction Fetch from Flash**: The Cortex-M CPU fetches instructions directly from Flash (`0x0800c145 (*F*)`).
3. **Data Access in RAM**: Global/static variables (`cpp3_magic`) are read/written in RAM (`0x20001210 (*R*)`).
4. **Result Verification**: `cpp3_method(seed, x)` returns the exact expected parameter calculation, confirming that cross-library calls, PLT routing, and split-base relocation operate seamlessly directly out of Flash.


---

## 9. common-elf-reader — Full ELF Parsing API

> **Location:** `../common-elf-reader/` (git submodule, standalone repo)
> **Files:** `include/loader/{byte-source.hpp,elf-image.hpp,elf-types.hpp,error.hpp}`, `include/loader/detail/bytes.hpp`, `include/loader/sources/memory-byte-source.hpp`, `src/elf-image.cpp`
> **CMake target:** `common-elf-reader::elf-reader` (INTERFACE source library)

### 9.0 Overview: What common-elf-reader Provides

The `common-elf-reader` is a standalone ELF32 reader/parser extracted so both the SCL and the former `shared-static-rework` project share one parser API. It provides four layers:

| Layer | File | What it does |
|-------|------|-------------|
| **I/O abstraction** | `byte-source.hpp` | Abstract random-access byte reader — the parser never assumes where bytes live |
| **Error handling** | `error.hpp` | `error` enum (10 codes) + `result<T>` (C++20 expected-like) |
| **ELF structures** | `elf-types.hpp` | ARM ELF32-LE on-disk structs (`Ehdr`, `Phdr`, `Shdr`, `Sym`, `Rel`, `Dyn`) + constants (`DT_*`, `PT_LOAD`, `R_ARM_*`, `SHF_*`) + accessors (`r_sym`, `r_type`, `st_bind`, `st_type`) |
| **Bounded byte access** | `detail/bytes.hpp` | Header-only LE load/store with bounds checks — used by ALL loaders |
| **ELF parser** | `elf-image.hpp` + `elf-image.cpp` | Validated ELF view: parse, program/section headers, find section by name, image size/base |

The reader has **no dependencies** beyond C++20. It is compiled from source into each consumer (INTERFACE target, no `.a` archive). Only `collect_relocations()` needs `SCL_HOSTED` (for `std::vector`); freestanding firmware uses `reloc_applier` instead.

### 9.1 API Reference

#### `loader::byte_source` — I/O Abstraction
- **File:** `../common-elf-reader/include/loader/byte-source.hpp:17`
- Abstract base class for random-access byte reading. The entire parser stack goes through this interface — swap the backend to read from semihosting, flash, RAM, SD card, etc.
- **Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `size` | `virtual size_t size() const noexcept = 0` | Total bytes in the object |
| `read_at` | `virtual result<void> read_at(size_t offset, span<byte> out) noexcept = 0` | Fill `out` from `offset`. Returns `error::SHORT_READ` if range exceeds object |

#### `loader::memory_byte_source` — In-Memory Backend
- **File:** `../common-elf-reader/include/loader/sources/memory-byte-source.hpp:16`
- Concrete `byte_source` over a `span<const byte>`. Non-owning — the caller keeps the buffer alive.
- **Constructor:** `explicit memory_byte_source(span<const byte> data) noexcept`
- Implements `size()` and `read_at()` via `memcpy`

#### `loader::error` — Error Codes
- **File:** `../common-elf-reader/include/loader/error.hpp:12`
- **Enum values:** `OK`, `FILE_NOT_FOUND`, `SHORT_READ`, `NOT_ELF`, `NOT_ARM32`, `NOT_SHARED_OBJECT`, `BAD_SECTION`, `SYMBOL_NOT_FOUND`, `UNSUPPORTED_RELOC`, `OUT_OF_MEMORY`
- Used throughout the entire SCL as the failure type

#### `loader::result<T>` — Value-or-Error
- **File:** `../common-elf-reader/include/loader/error.hpp:40`
- C++20 replacement for `std::expected<T, error>`. Exposes:
  - `has_value()` / `operator bool` — check for success
  - `operator*` / `operator->` — access the value
  - `err()` — get the error (when `!has_value()`)
  - `value_or(fallback)` — value or default
- **Partial specialisation** `result<void>` (line 74-87): carries success/error with no value
- **Builder:** `loader::unexpected(error e)` constructs the failure alternative

#### `loader::elf::` Types — ARM ELF32 Structures
- **File:** `../common-elf-reader/include/loader/elf-types.hpp:13`
- All structs are `memcpy`-compatible with on-disk layout (requires little-endian host, enforced by `static_assert` at line 17).

| Struct | Bytes | Fields | Purpose |
|--------|-------|--------|---------|
| `Ehdr` | 52 | `e_ident[16], e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx` | ELF file header |
| `Phdr` | 32 | `p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align` | Program header (segment) |
| `Shdr` | 40 | `sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize` | Section header |
| `Sym` | 16 | `st_name, st_value, st_size, st_info, st_other, st_shndx` | Symbol table entry |
| `Rel` | 8 | `r_offset, r_info` | REL relocation entry (ARM uses REL, not RELA) |
| `Dyn` | 8 | `d_tag, d_val` | Dynamic section entry |

- **Constants:** `DT_NULL` (0), `DT_HASH` (4), `DT_STRTAB` (5), `DT_SYMTAB` (6), `DT_STRSZ` (10), `DT_INIT_ARRAY` (25), `DT_FINI_ARRAY` (26), `PT_LOAD` (1), `SHT_REL` (9), `SHF_ALLOC` (2), `ET_DYN` (3), `EM_ARM` (40)
- **Relocation types** (`rel_type` enum, line 122): `ABS32` (2), `REL32` (3), `COPY` (20), `GLOB_DAT` (21), `JUMP_SLOT` (22), `RELATIVE` (23)
- **Decoders:**
  - `elf::r_sym(word info)` — extract symbol index from `r_info` (high 24 bits)
  - `elf::r_type(word info)` — extract relocation type from `r_info` (low 8 bits)
  - `elf::st_bind(uint8_t info)` — extract binding from `st_info` (high nibble)
  - `elf::st_type(uint8_t info)` — extract symbol type from `st_info` (low nibble)

#### `loader::detail::bytes` — Bounded LE Accessors
- **File:** `../common-elf-reader/include/loader/detail/bytes.hpp:22`
- Header-only, bounds-checked. Used by `reloc_applier`, `symbol_resolver`, `lazy_resolve`, and all loaders.
- **Functions:**

| Function | Signature | Description |
|----------|-----------|-------------|
| `load_word` | `uint32_t load_word(span<const byte> s, size_t off) noexcept` | LE 32-bit read; out-of-range → 0 |
| `load_byte` | `uint8_t load_byte(span<const byte> s, size_t off) noexcept` | Single byte read; out-of-range → 0 |
| `store_word` | `void store_word(span<byte> s, size_t off, uint32_t v) noexcept` | LE 32-bit write; out-of-range → no-op |
| `name_at` | `string_view name_at(span<const byte> tab, uint32_t off) noexcept` | NUL-terminated string bounded by span; hand-rolled scan (no `strnlen` for freestanding) |

#### `loader::elf_image` — ELF Parser
- **File:** `../common-elf-reader/include/loader/elf-image.hpp:18`, `../common-elf-reader/src/elf-image.cpp:26`
- Validated view over an ARM ELF32 shared object. The `byte_source` must outlive the `elf_image`.
- **Static factory:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `parse` | `static result<elf_image> parse(byte_source& src) noexcept` | Validate magic/class/data/machine/type, read Ehdr. Errors: `NOT_ELF`, `NOT_ARM32`, `NOT_SHARED_OBJECT` |

- **Accessors:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `header` | `const elf::Ehdr& header() const noexcept` | Cached ELF file header |
| `program_header` | `result<void> program_header(half index, elf::Phdr& out) const noexcept` | Read program header `i` into `out`. Error: `BAD_SECTION` if index ≥ e_phnum |
| `section_header` | `result<void> section_header(half index, elf::Shdr& out) const noexcept` | Read section header `i` into `out`. Error: `BAD_SECTION` if index ≥ e_shnum |
| `find_section` | `result<elf::Shdr> find_section(string_view name) const noexcept` | Find section by name. Reads `.shstrtab`, iterates all section headers comparing names. Error: `BAD_SECTION` if not found |
| `image_size` | `result<uint32_t> image_size() const noexcept` | Highest `p_vaddr + p_memsz` across all PT_LOAD segments (the total VA range). 32-bit safe |
| `image_base` | `result<uint32_t> image_base() const noexcept` | Lowest `p_vaddr` across all PT_LOAD segments |
| `source` | `byte_source& source() const noexcept` | Access the underlying byte source |

- **Hosted-only:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `collect_relocations` | `result<vector<byte>> collect_relocations() const noexcept` | Concatenate all allocated `SHT_REL` sections (`.rel.dyn`, `.rel.plt`, ...) into one buffer. Requires `SCL_HOSTED` for `std::vector` |

### 9.2 Internal Functions (`../common-elf-reader/src/elf-image.cpp`)

| Function | Line | Scope | Description |
|----------|------|-------|-------------|
| `read_pod<T>` | 14 | anonymous ns | `result<T> read_pod(byte_source& src, size_t offset)` — read a trivially-copyable POD at offset. Used to read Ehdr, Phdr, Shdr from the byte source. Template ensures trivially-copyable via `static_assert`. |

### 9.3 Architecture: How the Parser Works on Cortex-M

**Parsing flow for every `dlopen`:**

```
byte_source (semihosting file / flash blob / RAM)
  │
  └─► elf_image::parse(bytes)
       │
       ├─► read_pod<Ehdr>(src, 0)   ──► 52 bytes at offset 0
       │    └─► validate: e_ident[0:3] == "\x7fELF"
       │    └─► validate: EI_CLASS == 32-bit
       │    └─► validate: EI_DATA == little-endian
       │    └─► validate: e_machine == EM_ARM (40)
       │    └─► validate: e_type == ET_DYN (3)
       │    └─► cache Ehdr in elf_image
       │
       ├─► for i in 0..e_phnum:        (used by PT_LOAD copy loop)
       │    └─► read_pod<Phdr>(src, e_phoff + i * sizeof(Phdr))
       │    └─► returns Phdr.p_type, p_offset, p_vaddr, p_filesz, p_memsz
       │
       ├─► for i in 0..e_shnum:        (used by find_section)
       │    └─► read_pod<Shdr>(src, e_shoff + i * sizeof(Shdr))
       │    └─► returns Shdr.sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size
       │
       ├─► image_size():
       │    └─► Iterate PT_LOAD segments, compute max(p_vaddr + p_memsz)
       │    └─► This is the total RAM footprint for arena allocation
       │
       └─► image_base():
            └─► Iterate PT_LOAD segments, find min(p_vaddr)
```

**Why bounds-checked byte access matters on Cortex-M:**

The `detail/bytes.hpp` helpers (`load_word`, `store_word`, `name_at`) are used by every loader component when walking in-memory ELF tables (`.dynsym`, `.dynstr`, `.hash`, `.rel.*`). On Cortex-M there is no MMU to trap out-of-bounds access — a malformed section offset would silently read arbitrary memory and potentially jump to it (HardFault). The bounds checks ensure that:
- `load_word` returns 0 for out-of-range offsets (instead of reading garbage)
- `store_word` is a no-op for out-of-range offsets (instead of corrupting adjacent memory)
- `name_at` never scans past the string table span (no unbounded `strlen` into unmapped memory)

**The `detail/` namespace convention:**

Access to raw ELF bytes goes through `loader::detail::load_word` / `store_word` / `name_at` rather than direct pointer dereferencing or `memcpy`. This single chokepoint makes it possible to add memory access instrumentation, endian conversion (for big-endian targets), or fault injection testing without touching every loader.

### 9.4 Dependencies and Integration

**How the SCL consumes common-elf-reader (`../CMakeLists.txt:47`):**

```cmake
add_subdirectory(${COMMON_ELF_READER_DIR} common-elf-reader)
# → defines common-elf-reader::elf-reader (INTERFACE)

target_link_libraries(scl-common INTERFACE common-elf-reader::elf-reader)
# → every loader transitively gets the parser
```

All six loader targets transitively link `common-elf-reader::elf-reader` — five through `scl-common`, and `scl-static` directly (via `target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)`). The parser's single `.cpp` file (`src/elf-image.cpp`) is compiled exactly once per executable (CMake de-duplicates INTERFACE sources), with the consuming application's compiler flags — ensuring the correct `-mcpu`, `-mfloat-abi`, and optimisation level regardless of which loader flavour is in use.

---

## 10. Template Application

A unified firmware `main()` that exercises **every** SCL feature across all six libraries.

> **Note:** This template combines APIs that normally live in separate firmware builds (freestanding vs hosted, ET_DYN vs ET_REL). It demonstrates every call pattern — adapt the platform init to your actual board.

```cpp
// ============================================================
// TEMPLATE: all SCL APIs in one file
// ============================================================
#include <cstdint>
#include <cstring>
#include <span>

// ---- scl::common core ----
#include "loader/elf-image.hpp"
#include "loader/byte-source.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/error.hpp"
#include "loader/dynamic-info.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/lazy-resolve.hpp"
#include "loader/region-allocator.hpp"
#include "loader/relocator.hpp"             // SCL_HOSTED only
#include "loader/detail/bytes.hpp"
#include "loader/elf-types.hpp"

// ---- scl::firmware helpers ----
#include "scl/log.h"
#include "scl/image-memory.hpp"
#include "scl/link-context.hpp"
#include "scl/pic-trampoline.h"
#include "scl/unwind-exidx.h"

// ---- scl::dynamic-nolib (freestanding) ----
#include "dynamic-nolib/dl.hpp"
#include "dynamic-nolib/lazy-plt.hpp"

// ---- scl::dynamic-newlib (hosted) ----
#include "dynamic-newlib/dynamic-linker.hpp"
#include "dynamic-newlib/patch-journal.hpp"
#include "dynamic-newlib/image-memory.hpp"

// ---- scl::static (legacy patcher) ----
#include "static/elf-reader.h"
#include "static/shlib-loader.h"

// ---- scl::overlay ----
#include "overlay/overlay.h"

// ---- scl::reloc ----
#include "reloc/reloc-loader.hpp"

// ---- scl::xip ----
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include "xip/compact.hpp"
#include "xip/dynamic-nolib/adapter.hpp"    // XIP adapter for nolib
#include "xip/module.hpp"

// ============================================================
// SECTION 1: scl::common — low-level toolbox usage
// ============================================================
void demo_common_toolbox() {
    // ---- byte_source: storage abstraction ----
    std::byte raw_elf[4096];                     // pretend .so bytes
    loader::memory_byte_source src(raw_elf);     // wrap RAM buffer as a byte_source

    // ---- error/result ----
    loader::result<int> ok_result = 42;
    loader::result<int> err_result = loader::unexpected(loader::error::NOT_ELF);
    if (!err_result) {
        loader::error e = err_result.err();      // e == loader::error::NOT_ELF
    }

    // ---- elf_image: parse ELF ----
    auto parsed = loader::elf_image::parse(src);
    if (parsed) {
        auto& hdr = parsed->header();           // Ehdr: e_phnum, e_shnum, e_entry...
        auto sz   = parsed->image_size();       // total PT_LOAD VA range
        auto base = parsed->image_base();       // lowest PT_LOAD vaddr

        // Program headers
        loader::elf::Phdr ph;
        parsed->program_header(0, ph);           // ph.p_vaddr, p_offset, p_filesz, p_memsz

        // Section headers
        auto sh = parsed->find_section(".dynsym"); // Shdr: sh_addr, sh_offset, sh_size
        auto sh_by_idx = loader::elf::Shdr{};
        parsed->section_header(3, sh_by_idx);
    }

    // ---- detail/bytes: bounded LE accessors ----
    std::span<const std::byte> buf(raw_elf, 100);
    uint32_t w = loader::detail::load_word(buf, 16);   // LE read at offset
    uint8_t  b = loader::detail::load_byte(buf, 0);     // single byte
    loader::detail::store_word({raw_elf, 100}, 20, 0x12345678);  // LE write
    auto str = loader::detail::name_at(buf, 4);          // NUL-terminated string read

    // ---- dynamic_info: parse _DYNAMIC ----
    auto di = loader::dynamic_info::parse(buf);  // extracts DT_HASH, DT_SYMTAB, DT_STRTAB

    // ---- symbol_resolver: SysV hash lookup ----
    auto resolver = loader::symbol_resolver(buf, buf, buf);
    auto addr = resolver.address_of("printf");   // image-relative .dynsym value

    // ---- reloc_applier: allocation-free relocation streaming ----
    loader::reloc_applier::apply_rel(
        std::span<std::byte>(raw_elf, 4096),     // writable image
        0x20000000,                              // base address
        buf, buf, buf,                           // .rel.dyn, .dynsym, .dynstr
        loader::symbol_source{                   // external resolver callback
            +[](void*, std::string_view n) -> loader::result<uint32_t> {
                if (n == "printf") return 0x08001234u;
                return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
            },
            nullptr
        }
    );

    // ---- relocator: hosted-only plan builder ----
    // (SCL_HOSTED only, requires std::vector)
    // auto plan = loader::relocator::compute(img, base);

    // ---- LazyResolve: map GOT slot to JUMP_SLOT symbol ----
    loader::lazy_tables lt{buf, buf, buf};       // JMPREL + dynsym + dynstr
    auto target = loader::lazy_resolve_slot(      // resolve GOT[5]->symbol
        0x80, lt,                                // got_off = 0x80
        loader::symbol_source{nullptr, nullptr}
    );

    // ---- region_allocator: fixed-arena allocator ----
    std::byte arena_buf[16384];
    loader::region_allocator arena(arena_buf, sizeof(arena_buf));
    auto* block = arena.alloc(4096);             // allocate (8-byte aligned)
    arena.free(block);                           // free with coalesce

    // ---- elf_hash ----
    uint32_t hash = loader::elf_hash("mylib_function");  // SysV hash value
}

// ============================================================
// SECTION 2: scl::dynamic-nolib — the real dlopen
// ============================================================
void demo_dynamic_nolib() {
    // ---- configuration ----
    fw::dl_enable_cross_call(true);    // lib->lib r10 routing
    fw::dl_cross_trace(2);             // verbose GOT/PLT tracing

    // ---- load a library from any byte_source ----
    // Front-end: app provides byte_source (semihosting, flash archive, ...)
    std::byte lib_bytes[1024];         // pretend .so bytes from somewhere
    loader::memory_byte_source src(lib_bytes);
    auto load_res = fw::dlopen_source(src, "mylib1");
    if (!load_res) {
        // error: load_res.err() — NOT_ELF, OUT_OF_MEMORY, SHORT_READ, etc.
        return;
    }
    fw::handle* h = *load_res;         // get the handle

    // ---- handle fields ----
    uint32_t base     = h->base;       // runtime base address
    uint32_t size     = h->size;       // allocated size
    uint32_t got_base = h->got_base;   // r10 value for entering this lib
    uint32_t fb       = base + 0x1000; // some field

    // ---- resolve a function symbol ----
    auto fn_res = fw::dlsym(*h, "library_function");
    if (!fn_res) return;               // SYMBOL_NOT_FOUND
    auto fn_ptr = reinterpret_cast<uint32_t (*)(uint32_t)>(*fn_res);

    // ---- call library with r10 swap ----
    uint32_t result = fw::pic_call_u32(fn_ptr, (void*)h->got_base, 42);

    // ---- call library with 2 args ----
    uint32_t r2 = fw::pic_call_u32_2(fn_ptr, (void*)h->got_base, 1, 2);

    // ---- call library void function ----
    fw::pic_call_void(fn_ptr, (void*)h->got_base);

    // ---- resolve a data symbol ----
    auto var_res = fw::dlsym(*h, "library_global");
    if (var_res) {
        auto* var_ptr = static_cast<uint32_t*>(*var_res);
        uint32_t val = *var_ptr;       // read exported global
    }

    // ---- lazy binding diagnostics ----
    uint32_t fixups = fw::dl_fixup_count();   // how many lazy fixups fired

    // ---- GOT/PLT dump (didactic) ----
    fw::dl_dump_image(*h);                   // dump relocation tables
    fw::dl_dump_got_walkthrough(*h);         // BEFORE/AFTER GOT walkthrough
    const char* names[] = {"printf", "puts"};
    fw::dl_dump_main_provides(names, 2);     // main's exported symbols

    // ---- dlclose (refcount-guarded) ----
    fw::dlclose(*h);
    // h is now invalid

    // ---- POSIX-style facade ----
    void* posix_h = dl::dlopen("mylib1.so", RTLD_NOW);
    if (posix_h) {
        void* sym = dl::dlsym(posix_h, "library_function");
        dl::dlclose(posix_h);
    }
    const char* err = dl::dlerror();    // last error string
}

// ============================================================
// SECTION 3: scl::dynamic-newlib — hosted dynamic loader
// ============================================================
void demo_dynamic_newlib() {
    // ---- create link context (reads _reloc table or _DYNAMIC) ----
    auto ctx = firmware::link_context::from_self();
    firmware::dynamic_linker linker(ctx);

    // ---- load library from byte_source ----
    std::byte lib_bytes[1024];
    loader::memory_byte_source src(lib_bytes);
    auto lib_res = linker.load_from_source(src);
    if (!lib_res) return;
    firmware::library* lib = *lib_res;

    // ---- library fields ----
    auto base_va  = lib->base_va;
    auto init_arr = lib->init_array;
    auto init_cnt = lib->init_count;

    // ---- resolve symbol ----
    auto sym_addr = linker.lookup(lib, "library_function");
    if (sym_addr) {
        auto fn = reinterpret_cast<uint32_t (*)(uint32_t)>(*sym_addr);
        uint32_t r = fn(42);           // direct call (eager binding, no trampoline)
    }

    // ---- unload (journal revert + heap free) ----
    linker.unload(lib);

    // ---- lookup without a library (firmware symbols) ----
    auto fw_sym = linker.lookup(nullptr, "printf");
}

// ============================================================
// SECTION 4: scl::static — legacy ELF32 patcher
// ============================================================
void demo_static_loader() {
    elf_loader::shlib_loader loader;

    // ---- read ELF file ----
    loader.read_elf("libsh1.so");

    // ---- load binary into RAM buffer ----
    uint8_t buffer[8192];
    loader.load_bin(buffer, sizeof(buffer));

    // ---- populate helper sections ----
    loader.populate_helper_sections();

    // ---- patch counts ----
    uint32_t sh_cnt = loader.get_shlib_patch_count();
    uint32_t m_cnt  = loader.get_main_patch_count();

    // ---- allocate patch arrays ----
    elf_loader::symbol_patch sh_patches[64];
    elf_loader::symbol_patch m_patches[64];
    loader.allocate_patch_arrays(sh_patches, m_patches);

    // ---- find symbols ----
    loader.find_shlib_symbols_to_patch(sh_patches);
    loader.find_main_symbols_to_patch(m_patches);

    // ---- patch GOT (library and main) ----
    elf_loader::symbol_table syms;
    uint32_t n_sh = loader.patch_shlib(&syms);
    uint32_t n_m  = loader.patch_main(&syms);

    // ---- resolve DT_NEEDED dependencies ----
    loader.resolve_dependencies(&syms, nullptr);

    // ---- run constructors ----
    loader.execute_constructors();

    // ---- call library functions directly (no trampoline) ----
    auto fn = reinterpret_cast<uint32_t (*)(uint32_t)>(
        reinterpret_cast<uintptr_t>(loader.get_ram_shlib()) + 0x100);
    // (addresses are known after patching)

    // ---- sync barriers ----
    elf_loader::sync_after_patching();
}

// ============================================================
// SECTION 5: scl::overlay — linker-overlay window manager
// ============================================================
// Linker provides these symbols (from OVERLAY block):
extern uint32_t _ovl_vma;
extern uint32_t _ov1_size;
extern const void _ov1_lma;           // __load_start_ov1
extern const void _ov2_lma;
extern const void _ovcpp_lma;

// Overlay entry points (long_call):
extern "C" uint32_t ov1_use_fm(void);
extern "C" uint32_t ovcpp_method(int a, int b);

void demo_overlay() {
    // ---- build descriptor table from linker symbols ----
    scl_overlay_module mods[3] = {
        { (void*)&_ovl_vma, &_ov1_lma,  (uint32_t)(uintptr_t)&_ov1_size,  "ov1" },
        { (void*)&_ovl_vma, &_ov2_lma,  (uint32_t)(uintptr_t)&_ov2_size,  "ov2" },
        { (void*)&_ovl_vma, &_ovcpp_lma,(uint32_t)(uintptr_t)&_ovcpp_size,"ovcpp" },
    };

    // ---- init overlay region ----
    scl_overlay_region ovl;
    scl_overlay_init(&ovl, mods, 3);

    // ---- load module 0 (ov1) ----
    // memcpy(LMA->VMA) + DSB/ISB; no-op if already resident
    if (scl_overlay_load(&ovl, 0) != 0) return;

    // ---- check current resident ----
    int cur = scl_overlay_current(&ovl);  // == 0

    // ---- call overlay function (direct long_call, already linked) ----
    uint32_t r = ov1_use_fm();

    // ---- switch to another overlay (replaces window) ----
    scl_overlay_load(&ovl, 2);            // ovcpp now resident, ov1 gone
    ovcpp_method(2, 100);
}

// ============================================================
// SECTION 6: scl::reloc — ET_REL module loader
// ============================================================
void demo_reloc_loader() {
    std::byte arena[16384];    // Caller-supplied RAM arena

    // ---- resolve callback for external symbols ----
    loader::symbol_source resolve{
        +[](void*, std::string_view n) -> loader::result<uint32_t> {
            if (n == "printf") return 0x08001234u;
            return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
        },
        nullptr
    };

    // ---- load a single ET_REL module ----
    std::byte mod_bytes[2048];  // pretend .o bytes
    auto m = reloc::load(mod_bytes, arena, resolve);
    if (m) {
        // module fields
        uint32_t used = m->used;
        auto sec0_base = m->sec_base[0];  // runtime address of section 0

        // look up exported symbol
        uint32_t a = reloc::sym(*m, "my_function");
        auto fn = reinterpret_cast<uint32_t (*)()>(a);
        fn();
    }

    // ---- load a SET of modules (handles cyclic deps) ----
    std::byte c1_bytes[1024], c2_bytes[1024], cpp3_bytes[1024];
    std::span<const std::byte> images[] = { c1_bytes, c2_bytes, cpp3_bytes };
    auto set = reloc::load_set(images, arena, resolve);
    if (set) {
        // cross-module symbol lookup
        uint32_t a1 = reloc::sym(*set, "c1_function");
        uint32_t a2 = reloc::sym(*set, "c2_function");

        // access individual modules
        auto& mod0 = set->mods[0];  // c1
        auto& mod1 = set->mods[1];  // c2
    }
}

// ============================================================
// SECTION 7: scl::xip — Execute-In-Place
// ============================================================
void demo_xip() {
    // ---- core XIP analysis (pure computation) ----
    std::byte so_bytes[4096];       // .so file bytes (in flash)
    xip::SegmentMap seg;
    bool can_xip = xip::classify_segments(so_bytes, seg);
    if (can_xip) {
        bool immutable = xip::text_is_immutable(so_bytes, seg);
        if (immutable) {
            // Text can stay in flash, only RW goes to RAM
            xip::Module xmod;
            bool ok = xip::compact_for_xip(so_bytes, xmod);
            if (ok) {
                // xmod.text -> flash span, xmod.data -> empty (loader fills)
                // xmod.text_base = flash address of .text
                // xmod.xip = true
            }
        }
    }

    // ---- XIP split-base relocation (low-level) ----
    std::byte ram_buf[2048];        // RAM for RW data
    loader::symbol_resolver resolver({}, {}, {});
    xip::apply_rel_xip(
        seg,                        // segment map
        0x08000000,                 // text_base (flash address)
        0x20000000,                 // data_base (RAM address)
        ram_buf,                    // RW data in RAM
        {}, {}, {},                 // .rel.dyn, .dynsym, .dynstr
        loader::symbol_source{nullptr, nullptr}
    );

    // ---- XIP adapter for dynamic-nolib ----
    void* xh = xip::nolib::dlopen(so_bytes, "mylib");
    if (xh) {
        bool is_xip = xip::nolib::is_xip(xh);
        auto got    = xip::nolib::get_got_base(xh);

        xip::xip_map xmap;
        bool got_map = xip::nolib::get_xip_map(xh, xmap);
        // xmap.text_base, xmap.data_base, xmap.got_base, xmap.is_xip

        void* fn = xip::nolib::dlsym(xh, "library_function");
        if (fn) {
            // Call with r10 = got_base
            uint32_t r = pic_call_u32(fn, (void*)got, 42);
        }
        xip::nolib::dlclose(xh);
    }

    // ---- XIP: patch main's imports ----
    xip::nolib::patch_main_imports(so_bytes);
}

// ============================================================
// SECTION 8: Cross-feature integration
// ============================================================
void demo_cross_feature() {
    // ---- Use pic_call with different got_base patterns ----
    // Each library has its own got_base (r10 value)

    // ---- Lazy PLT via dl_runtime_resolve ----
    // set up by dlopen_source automatically:
    //   JUMP_SLOT GOT slots point at dl_runtime_resolve
    //   dl_fixup() patches each slot on first call

    // ---- Cross-library calls via dl_cross_call ----
    // When dl_enable_cross_call(true):
    //   external JUMP_SLOTs -> dl_cross_call
    //   dl_cross_lookup() resolves, swaps r10, calls callee

    // ---- EHABI: exception across module boundary ----
    // dl_exidx_register() at load, dl_exidx_unregister() at close
    // __gnu_Unwind_Find_exidx consults the registry

    // ---- Section cache (dynamic-nolib internal) ----
    // section_cache loads .shstrtab once (2 file reads)
    // vs. elf_image::find_section() which reads per-section (8+ reads)

    // ---- combined_resolver dependency tracking ----
    // Record dep edges so dlclose refcount-guards properly
}

// ============================================================
// SECTION 9: Variable and data cross-reference
// ============================================================
// Global variables that loaders expect from firmware:
extern uint32_t _simage_arena_start;    // Arena base (linker script)
extern uint32_t _simage_arena_end;      // Arena end
// GOT boundaries (for scl::static):
extern uint32_t _sgot, _esgot, _sgot_plt, _esgot_plt;
// Overlay region symbols:
extern uint32_t _ovl_vma, _ovl_window_size;
// _reloc table entries (for scl::dynamic-newlib):
//   LINK(LOADADDR(.rel.dyn)) LONG(SIZEOF(.rel.dyn))
//   LINK(ADDR(.dynsym))      LONG(SIZEOF(.dynsym))
//   ... etc

// ============================================================
// SECTION 10: Logging hook
// ============================================================
// Override weak no-ops in scl/log.h:
extern "C" void scl_log_puts(const char* s) {
    // send to semihosting trace, UART, etc.
}
extern "C" void scl_log_putc(char c) {
    // send single char to trace
}
```

---

## 11. Execution Path / Call Stack Analysis

### 10.1 Path A: Dynamic Load (scl::dynamic-nolib)

```
SEMIHOSTING / FLASH ARCHIVE
  │
  ▼
fw::dlopen_source(src, rawname)                  [../src/dynamic-nolib/dl.cpp:190]
  │
  ├─► loader::elf_image::parse(src)              [../common-elf-reader/src/elf-image.cpp]
  │    └─► validate ELF magic, class, endian, machine, version, phoff, shoff
  │    └─► return elf_image view
  │
  ├─► img.image_size()                           [../common-elf-reader/src/elf-image.cpp]
  │    └─► iterate PT_LOAD, compute VA range
  │
  ├─► fw::image_arena().alloc(size)              [../src/image-memory.cpp]
  │    └─► region_allocator::alloc()              [../src/region-allocator.cpp]
  │         └─► first-fit search in free list
  │         └─► coalesce if needed
  │
  ├─► memset(0) arena slot
  │
  ├─► Copy PT_LOAD segments (LOOP)
  │    └─► img.program_header(i, ph)              [../common-elf-reader/src/elf-image.cpp]
  │    └─► src.read_at(ph.p_offset → mem + ph.p_vaddr)
  │
  ├─► section_cache::load(img, src)              [../src/dynamic-nolib/dl.cpp:84]
  │    └─► src.read_at(shoff → shdr table)
  │    └─► src.read_at(.shstrtab_offset → names)
  │
  ├─► sec_in_image(cache, mem, ".dynsym", ...)   [../src/dynamic-nolib/dl.cpp:144]
  ├─► sec_in_image(cache, mem, ".dynstr", ...)
  ├─► sec_in_image(cache, mem, ".hash", ...)
  │
  ├─► fw::link_context::from_self()              [../src/link-context.cpp]
  │    └─► read _DYNAMIC at link-time fixed address
  │    └─► dynamic_info::parse()                  [../src/dynamic-info.cpp]
  │         └─► iterate DT_NULL-terminated Dyn array
  │         └─► extract DT_HASH/SYMTAB/STRTAB/STRSZ
  │    └─► build symbol_resolver over main's .dynsym/.dynstr/.hash
  │
  ├─► alloc_handle() → handle slot               [../src/dynamic-nolib/dl.cpp:60]
  │
  ├─► Snapshot .got → h->got_raw[]              [../src/dynamic-nolib/dl.cpp:246]
  │
  ├─► combined_resolver::thunk                   [../src/dynamic-nolib/dl.cpp:164]
  │    └─► search other loaded libs (g_handles[])
  │    └─► if found: record dependency edge (deps[])
  │    └─► fallback: main->resolve(name)
  │
  ├─► loader::reloc_applier::apply_rel(          [../src/reloc-applier.cpp]
  │    │   .rel.dyn and .rel.plt)
  │    └─► for each elf::Rel entry:
  │         ├─ R_ARM_RELATIVE:  *P = base + A
  │         ├─ R_ARM_GLOB_DAT:  *P = resolve(sym)
  │         ├─ R_ARM_JUMP_SLOT: *P = resolve(sym)
  │         ├─ R_ARM_ABS32:     *P = S + A
  │         ├─ R_ARM_REL32:     *P = S + A - P
  │         └─ R_ARM_COPY:      memcpy P → P + size
  │
  ├─► Bump dependency refcounts
  │
  ├─► Lazy setup: JUMP_SLOT → dl_runtime_resolve [../src/dynamic-nolib/dl.cpp:286]
  │    └─► write &dl_runtime_resolve|1 to each JUMP_SLOT GOT slot
  │    └─► stash .rel.dyn + .rel.plt JMPREL in h->lazy / h->lazy_plt
  │
  ├─► Cross-call routing (if enabled)            [../src/dynamic-nolib/dl.cpp:312]
  │    └─► external JUMP_SLOTs → dl_cross_call trampoline
  │
  ├─► Compute got_base = base + .got_offset      [../src/dynamic-nolib/dl.cpp:333]
  │
  ├─► Exidx fixup                                [../src/dynamic-nolib/dl.cpp:335]
  │    └─► Walk .ARM.exidx, force Thumb bit on extab personalities
  │    └─► dl_exidx_register()                   [../src/port/unwind-find-exidx.c]
  │
  ├─► Run constructors (LOOP)                    [../src/dynamic-nolib/dl.cpp:378]
  │    └─► find_by_type(SHT_INIT_ARRAY)
  │    └─► for each entry: pic_call_void(fn, got_base)
  │
  └─► Record .fini_array for dlclose
       └─► find_by_type(SHT_FINI_ARRAY)

  │
  ▼
fw::dlsym(h, "name")                              [../src/dynamic-nolib/dl.cpp:392]
  └─► h->resolver.address_of(name)               [../src/symbol-resolver.cpp]
       └─► elf_hash(name) → bucket index
       └─► walk chain[], compare names
       └─► return h->base + st_value

  │
  ▼
fw::pic_call_u32(fn_ptr, got_base, arg)           [../src/port/pic-trampoline.c]
  └─► push {r10, lr}
  └─► mov r10, got_base
  └─► blx  fn_ptr         ← ENTER LIBRARY CODE
  └─► pop  {r10, pc}

  │
  ▼
fw::dlclose(h)                                    [../src/dynamic-nolib/dl.cpp:398]
  ├─► check refcount (refuse if != 0)
  ├─► run .fini_array destructors via pic_call_void
  ├─► decrement dependency refcounts
  ├─► dl_exidx_unregister(base)
  └─► image_arena().free(base)
```

### 10.2 Path B: Lazy Resolution (PLT)

```
First call to imported function:

  LIBRARY CODE
  │
  └─► BL to PLTn (plt section entry)
       │
       └─► PLTn asm:
            add r12, pc, #offset    → r12 = &GOT[n] (GOT slot address)
            ldr pc, [r12]           → pc = *GOT[n] = dl_runtime_resolve address

       │
       ▼
  dl_runtime_resolve                              [../src/port/lazy-resolve.c]
       └─► push {r0-r3, r4, lr}    ← save original args
       └─► vpush {d0-d7}           ← save VFP args
       └─► mov r0, r12             ← r0 = &GOT[n] (the slot address)
       └─► bl  dl_fixup            ← resolve + patch

       │
       ▼
  dl_fixup(slot)                                   [../src/dynamic-nolib/dl.cpp:456]
       ├─► find owning handle (which loaded lib owns this GOT slot?)
       ├─► link_context::from_self()   ← main's exports
       ├─► lazy_resolve_slot(got_off, h->lazy, resolve)
       │    └─► for each .rel(.dyn/.plt) JMPREL entry:
       │    │    └─► if r_offset == got_off:
       │    │         └─► read sym name via name_at(dynstr, Sym.st_name)
       │    │         └─► resolve(name) → absolute address
       │    │         └─► return target
       │    └─► SYMBOL_NOT_FOUND if no match
       │
       ├─► *slot = target    ← PATCH GOT SLOT (subsequent calls skip resolver)
       ├─► g_fixup_count++
       └─► return target

       │
       ▼
  dl_runtime_resolve (after bl dl_fixup returns)
       └─► mov r12, r0           ← target address in r12
       └─► vpop {d0-d7}          ← restore VFP args
       └─► pop  {r0-r3, r4, lr} ← restore original args
       └─► bx  r12               ← TAIL-CALL the REAL function

       │
       ▼
  REAL FUNCTION (e.g. printf in main flash)
  └─► SECOND CALL: GOT[n] already patched → PLTn → ldr pc,[GOT[n]] → REAL FUNCTION (direct, no resolver)
```

### 10.3 Path C: Cross-Library Call (r10 swap)

```
  LIBRARY A calls function from LIBRARY B:

  LIBRARY A CODE
  │
  └─► BL to PLTn (A's PLT)
       │
       └─► PLTn: add r12, pc, #offset → r12 = &A_GOT[n]
       │       ldr  pc, [r12]        → pc = dl_cross_call (if routing enabled)
       │
       ▼
  dl_cross_call                                    [../src/port/dl-cross-call.c]
       └─► push {r4-r7, r10, lr}   ← save caller state (including r10 = A's got_base)
       └─► push {r0-r3}            ← save original args
       └─► vpush {d0-d7}           ← save VFP
       └─► mov r0, r12             ← r0 = &GOT[n] in A's base
       └─► bl  dl_cross_lookup     ← resolve: symbol → {target, callee_got}

       │
       ▼
  dl_cross_lookup(got_slot_addr)                   [../src/dynamic-nolib/dl.cpp:501]
       ├─► find which handle owns this GOT slot (caller = A's handle)
       ├─► map slot offset back to symbol name:
       │    └─► scan .rel.dyn and .rel.plt for r_offset == slot_offset
       │    └─► read JUMP_SLOT sym → name
       ├─► search other handles for this name:
       │    └─► if B defines it:
       │    │    target = B.base + st_value
       │    │    got    = B.got_base
       │    └─► fallback main: target = main->resolve(name), got = 0
       └─► return (got << 32) | (target | 1)

       │
       ▼
  dl_cross_call (after bl dl_cross_lookup returns)
       └─► mov r4, r0             ← r4 = target | Thumb bit
       └─► mov r5, r1             ← r5 = callee_got (B's got_base)
       └─► vpop {d0-d7}           ← restore args
       └─► ldr r0-r3, [sp]        ← restore original function args
       └─► mov r10, r5            ← SWAP r10 = B's got_base (B's GOT now reachable)
       └─► blx r4                 ← CALL B's function
       │                             ← (B's function returns here)
       └─► add sp, sp, #16        ← clean up saved args
       └─► pop {r4-r7, r10, pc}   ← RESTORE r10 to A's got_base, return to A's caller
```

### 10.4 Path D: Static Patcher (scl::static)

```
shlib_loader::read_elf("lib.so")                  [../src/static/elf-reader.cpp]
  └─► elf_reader::open → elf_image::parse

shlib_loader::load_bin(buf, size)                 [../src/static/shlib-loader.cpp:55]
  └─► copy PT_LOAD segments into buf at p_vaddr offsets
  └─► scan DT_NEEDED → record dependency list

shlib_loader::populate_helper_sections()           [../src/static/shlib-loader.cpp:105]
  └─► locate .inits, .rel.dyn, .dynsym, .dynstr, .hash in both lib and main
  └─► main sections come from _reloc init block (linker-exported table)

shlib_loader::patch_shlib(&syms)                  [../src/static/shlib-loader.cpp:266]
  └─► for each .rel.dyn entry:
       └─► R_ARM_ABS32:     resolve sym → patch lib GOT
       └─► R_ARM_RELATIVE:  add load bias
       └─► R_ARM_GLOB_DAT:  resolve sym → patch lib GOT
       └─► R_ARM_JUMP_SLOT: resolve sym → patch lib GOT

shlib_loader::patch_main(&syms)                   [../src/static/shlib-loader.cpp:403]
  └─► for each main's .rel.dyn:
       └─► R_ARM_COPY:      memcpy lib export → main's .bss
       └─► R_ARM_GLOBAL:    patch main GOT with lib's exports
       └─► R_ARM_WEAK:      weak symbol resolution

shlib_loader::execute_constructors()               [../src/static/shlib-loader.cpp:509]
  └─► iterate .inits / .init_array → call each ctor

  │
  ▼
DIRECT CALL (no trampoline — addresses already patched)
```

### 10.5 Path E: Overlay Load

```
scl_overlay_init(&ovl, modules, 3)               [../src/overlay/overlay.c:10]
  └─► set modules pointer, count, resident = SCL_OVERLAY_NONE

scl_overlay_load(&ovl, index)                     [../src/overlay/overlay.c:17]
  ├─► if already resident → no-op (return 0)
  ├─► memcpy(m->vma, m->lma, m->size)    ← copy from flash to SRAM window
  ├─► __asm dsb 0xf                      ← drain write buffer
  ├─► __asm isb 0xf                      ← flush prefetch pipeline
  ├─► r->resident = index                ← mark resident
  └─► return 0

  │
  ▼
ov1_use_fm()                                      ← direct long_call to window address
  └─► (all addresses fixed at link time — no runtime linking)
```

### 10.6 Path F: ET_REL Module Load (scl::reloc)

```
reloc::load(image, arena, resolve)                [../src/reloc/reloc-loader.cpp:238]

PHASE 1 — place_module(image, arena, used, m)     [../src/reloc/reloc-loader.cpp:131]
  ├─► validate ELF magic (0x7f 'ELF'), type=ET_REL, machine=EM_ARM
  ├─► read section headers (sh_reader)
  ├─► for each SHF_ALLOC section:
  │    └─► align used to section alignment
  │    └─► copy from image to arena (or memset 0 for NOBITS)
  │    └─► set m.sec_base[i] = arena_base + used
  │    └─► used += section size
  ├─► record .symtab index
  └─► m.valid = true

PHASE 2 — apply_relocs(m, und)                    [../src/reloc/reloc-loader.cpp:170]
  ├─► for each SHT_REL section:
  │    └─► for each elf::Rel entry:
  │         ├─ R_ARM_ABS32 / TARGET1: *P = S + A
  │         ├─ R_ARM_REL32:           *P = S + A - P
  │         └─ R_ARM_THM_CALL:        BL re-encode
  │    └─► sym_env.addr_of(sym_idx):
  │         ├─ UND → resolve(name) via callback
  │         ├─ SHN_ABS → value
  │         └─ defined → sec_base[shndx] + st_value (|1 for STT_FUNC)

PHASE 3 — run_ctors(m)                            [../src/reloc/reloc-loader.cpp:210]
  └─► iterate .init_array → call each constructor

  │
  ▼
reloc::sym(m, "name")                              [../src/reloc/reloc-loader.cpp:267]
  └─► scan .symtab → match name → sec_base + st_value (|1 for STT_FUNC)

  │
  ▼
DIRECT CALL (no indirection — all addresses ABS32-relocated)
```

### 10.7 Path G: load_set (cyclic dependencies)

```
reloc::load_set(images, arena, resolve)            [../src/reloc/reloc-loader.cpp:248]

PHASE 1 — place ALL modules
  └─► for each image:
       └─► place_module() into arena (contiguous, packing)
       └─► all symbols now have addresses — cross-references resolvable

PHASE 2 — resolve ALL
  └─► set_ctx = { placed modules, n, external resolver }
  └─► set_resolve: search placed modules first, then external
  └─► for each module:
       └─► apply_relocs() with set_resolve as resolver
       └─► mutual/cyclic references resolved because all placed in phase 1

PHASE 3 — run ALL ctors
  └─► for each module: run_ctors()
```

### 10.8 Path H: XIP Load

```
xip::compact_for_xip(so_image, out)                [../src/xip/compact.cpp:11]
  │
  ├─► xip::classify_segments(so_image, seg)       [../src/xip/segment-map.cpp:16]
  │    └─► find one R-X (exec, !write) PT_LOAD + one RW(!W) PT_LOAD
  │    └─► require VA == file_offset for both (PLT GOT computation)
  │    └─► set seg.split = false if fused RWX or mismatch
  │
  ├─► if seg.split:
  │    └─► xip::text_is_immutable(so_image, seg)   [../src/xip/relocate.cpp]
  │         └─► scan all SHT_REL sections
  │         └─► verify every r_offset → SHF_WRITE section
  │         └─► false if any reloc targets R-X range
  │
  └─► if both pass:
       out.text = so_image.subspan(text_off, text_size)  // in-place flash view
       out.text_base = flash_addr of text
       out.xip = true

  │
  ▼
XIP LOADER (e.g. xip::nolib::dlopen)              [../src/xip/dynamic-nolib/adapter.cpp:40]
  │
  ├─► if can_xip:
  │    ├─► alloc RAM for RW segment (data_memsz)
  │    ├─► copy RW initializer bytes from flash to RAM
  │    ├─► memset 0 for .bss
  │    ├─► compute got_base = data_base + (.got_vaddr - data_vaddr)
  │    ├─► xip::apply_rel_xip(seg, text_base, data_base, ...)
  │    │    └─► RELATIVE: *P = A + (in_text(A) ? text_base - text_vaddr : data_base - data_vaddr)
  │    │    └─► GLOB_DAT/JUMP_SLOT: *P = sym_value(routed to text or data base)
  │    │    └─► Only RW words written (r_offset must be in data segment)
  │    ├─► Register exidx (text_base + .ARM.exidx offset — in flash)
  │    ├─► Route external JUMP_SLOTs → xip_cross_call
  │    └─► Run ctors via pic_call_void with got_base
  │
  └─► else (fallback to copy-whole):
       ├─► alloc RAM for entire image
       ├─► copy all LOAD segments to RAM
       ├─► apply all relocations manually (with split-base awareness)
       ├─► Route JUMP_SLOTs → xip_cross_call
       ├─► Register exidx
       └─► Run ctors
```

### 10.9 Path I: Dynamic-Newlib Unload (Journal Revert)

```
firmware::dynamic_linker::unload(lib)              [../src/dynamic-newlib/dynamic-linker.cpp:215]
  │
  ├─► lib->journal.revert()                        [../src/dynamic-newlib/patch-journal.cpp:35]
  │    └─► while count_ > 0:
  │         └─► count_--
  │         └─► memcpy(address, &original_value, 4)   ← RESTORE every patched word
  │
  ├─► prune pending_relocs pointing to this lib's image
  │
  └─► remove from libs_ vector → unique_ptr frees heap memory
       └─► System is now bit-identical to before load
```

---

## 12. Text Schemas & Diagrams

### 11.1 Memory Layout: Dynamic Load (scl::dynamic-nolib)

```
FLASH (firmware image, compiled -fpic -pie -Wl,-E)
┌──────────────────────────────────────┐
│ .text  .rodata  .data  .bss          │
│ (firmware code + data)               │
│                                      │
│ _DYNAMIC segment:                    │
│   DT_HASH, DT_SYMTAB, DT_STRTAB     │
│   (exported symbols for resolution)  │
│                                      │
│ __exidx_start → __exidx_end         │
│   (main's EHABI unwind tables)       │
└──────────────────────────────────────┘

RAM ARENA (_simage_arena_start .. _simage_arena_end)
┌──────────────────────────────────────┐
│ SLOT 0: loaded lib (e.g. "mylib1")  │
│  ┌────────┬────────┬────────┬──────┐ │
│  │ .text  │ .data  │ .got   │ .bss │ │
│  │ (PIC)  │        │        │      │ │
│  └────────┴────────┴────────┴──────┘ │
│  base = slot_addr                    │
│  got_base = base + .got_offset       │
│  (r10 = got_base to enter lib)       │
│                                      │
│ SLOT 1: another loaded lib           │
│ SLOT 2: ...                          │
│ SLOT 3: ...                          │
└──────────────────────────────────────┘

.got layout per library:
  [0] = (reserved) _DYNAMIC address
  [1] = (reserved) link_map
  [2] = (reserved) resolver
  [3] = RELATIVE (rebased to base + .data)
  [4] = GLOB_DAT  (resolved external data)
  [5] = JUMP_SLOT → dl_runtime_resolve | lazy tramp
  [6] = JUMP_SLOT → dl_cross_call (if routing enabled)
  ...
```

### 11.2 Memory Layout: Overlay Window

```
FLASH
┌──────────────────────┐
│ Firmware .text/.rodata│
├──────────────────────┤
│ Overlay bytes:       │
│ ov1 bytes (LMA_ov1)  │
│ ov2 bytes (LMA_ov2)  │
│ ovcpp bytes (LMA_ovcp)│
│ (back-to-back in     │
│  flash, only one     │
│  loaded at a time)   │
└──────────────────────┘

RAM SRAM WINDOW (VMA_ovl, e.g. 0x20018000)
┌──────────────────────┐
│ Current overlay code │ ← memcpy'd from LMA
│ .text .data .bss     │
│ size = SIZEOF(.ovN)  │
│                      │
│ Address aliasing:    │
│ ov1, ov2, ovcpp all  │
│ share this SAME VMA  │
└──────────────────────┘
```

### 11.3 Memory Layout: XIP Split

```
FLASH (memory-mapped)
┌──────────────────────────────┐
│ R-X segment (stays in flash):│
│ .text  .rodata  .plt  .got   │
│ (PC-relative, position-indep)│
│ text_vaddr == text_off       │
│ text_base = flash_addr       │
│                              │
│ (NO relocations target here) │
├──────────────────────────────┤
│ RW segment copy (initializer │
│ bytes; memcpy'd to RAM at    │
│ load time)                   │
└──────────────────────────────┘

RAM (copied from flash RW bytes)
┌──────────────────────────────┐
│ RW segment:                  │
│ .data  .got  .bss            │
│ data_vaddr == data_off       │
│ data_base = ram_addr         │
│                              │
│ GOT is at:                   │
│ got_base = data_base +       │
│   (.got_vaddr - data_vaddr)  │
│ (r10 = got_base for pic_call)│
└──────────────────────────────┘
```

### 11.4 ELF Parsing Flow

```
Byte Source (semihost file / flash blob / RAM buffer)
│
└─► elf_image::parse()
     │
     ├─► validate ELF header (magic, class=32, endian=LE, machine=ARM,
     │                        version=1, phoff!=0, shoff!=0)
     │
     ├─► Ehdr fields: e_phnum (program header count)
     │                e_shnum (section header count)
     │                e_entry (entry point)
     │                e_shstrndx (.shstrtab section index)
     │
     ├─► [optional] read program headers:
     │    └─► Phdr: p_type=PT_LOAD, p_vaddr, p_offset, p_filesz, p_memsz, p_flags
     │    └─► Phdr: p_type=PT_DYNAMIC, p_vaddr (→ _DYNAMIC array)
     │    └─► Phdr: p_type=PT_ARM_EXIDX, p_vaddr, p_filesz
     │
     ├─► [optional] read section headers:
     │    └─► Shdr: sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size
     │    └─► Section names read from .shstrtab
     │
     ├─► find_section(name):
     │    └─► read Shdr at index matching name
     │
     └─► image_size(): max(PT_LOAD vaddr+memsz) - min(PT_LOAD vaddr)
         image_base(): min(PT_LOAD vaddr)

_DYNAMIC parsing (via dynamic_info):
  └─► iterate Dyn array until DT_NULL:
       └─► DT_HASH:    address of .hash
       └─► DT_SYMTAB:  address of .dynsym
       └─► DT_STRTAB:  address of .dynstr
       └─► DT_STRSZ:   size of .dynstr
       └─► DT_JMPREL:  address of .rel.plt
       └─► DT_INIT:    address of .init
       └─► DT_INIT_ARRAY: address of .init_array
       └─► DT_FINI:       address of .fini
       └─► DT_NEEDED:     dependency name (string table offset)

Symbol resolution (via symbol_resolver):
  └─► .hash: [nbucket(4), nchain(4), bucket[nbucket](4), chain[nchain](4)]
  └─► elf_hash(name) % nbucket → bucket[i] → walk chain[]:
       └─► compare name with .dynstr + .dynsym[sym_index].st_name
       └─► match → return .dynsym[sym_index].st_value (image-relative)

Relocation apply (via reloc_applier):
  └─► .rel.dyn / .rel.plt: array of elf::Rel { r_offset, r_info }
       └─► r_type = r_info & 0xff
       └─► r_sym  = r_info >> 8
       └─► for each:
            RELATIVE:  *base[r_offset] = base + A (load bias)
            GLOB_DAT:  *base[r_offset] = resolve(sym_name)
            JUMP_SLOT: *base[r_offset] = resolve(sym_name)
            ABS32:     *base[r_offset] = S + A
            REL32:     *base[r_offset] = S + A - P
            COPY:      memcpy(size)
```

### 11.5 Firmware Export Table: _DYNAMIC vs _reloc

```
Dynamic loader (scl::dynamic-nolib):
  └─► firmware's _DYNAMIC segment is emitted by the linker (-pie -Wl,-E)
  └─► link_context::from_self() reads it from its known link-time address
  └─► Contains: DT_HASH, DT_SYMTAB, DT_STRTAB of main's exported symbols

Static loader (scl::dynamic-newlib):
  └─► firmware's linker script emits a _reloc table:
       _reloc = .
       LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
       LONG(ADDR(.dynsym));      LONG(SIZEOF(.dynsym));
       LONG(ADDR(.dynstr));      LONG(SIZEOF(.dynstr));
       LONG(ADDR(.hash));        LONG(SIZEOF(.hash));
       LONG(ADDR(.inits));       LONG(SIZEOF(.inits));
  └─► link_context reads this fixed-layout table at boot

ET_REL loader (scl::reloc):
  └─► No _DYNAMIC at all — uses .symtab + .strtab from the .o file
  └─► externals resolved through loader::symbol_source callback
```

### 11.6 GOT/PLT Mechanics

```
Standard ARM GOT/PLT on Cortex-M:

PLTn (at PLT section):
  ldr  r12, [pc, #offset]    ; r12 = &GOT[n] (pc-relative load)
  ldr  pc, [r12]              ; pc = *GOT[n] (jump through GOT slot)

GOT layout:
  GOT[0] = _DYNAMIC address
  GOT[1] = link_map (unused on bare-metal)
  GOT[2] = resolver entry (dl_runtime_resolve on Cortex-M)
  GOT[3] = first real symbol...

Lazy binding initial state:
  GOT[3..N] are initially set to point back into PLT
  Specifically: GOT[n] points to the PLTn instruction AFTER the
  first ldr — so the second ldr loads itself → loops until resolver patches

On Cortex-M (our approach):
  We bypass PLT0 (GOT[2]-relative offset is ambiguous on M-profile).
  We rely on PLTn's: add r12, pc, #offset → r12 = &GOT[n]
  Then we write dl_runtime_resolve into every external JUMP_SLOT GOT slot.
  dl_fixup(slot) is called with r12 = &GOT[n], resolves, patches GOT[n].

After resolution:
  GOT[n] = real target address | 1 (Thumb bit)
  PLTn's second ldr now loads the real target directly.
```

### 11.7 Source File Dependency Graph

```
common-elf-reader/ (submodule)
  byte-source.hpp ←── elf-image.hpp ←── dynamic-info.hpp
  error.hpp        ←── elf-types.hpp ←── symbol-resolver.hpp
  detail/bytes.hpp ←── reloc-applier.hpp ←── lazy-resolve.hpp
                      relocator.hpp (hosted only)

scl::common (include/scl/ + src/)
  log.h ────────────────────→ ALL loader implementations
  image-memory.hpp ─────────→ dynamic-nolib/dl.cpp
  link-context.hpp ─────────→ dynamic-nolib/dl.cpp, xip/*
  pic-trampoline.h ─────────→ dynamic-nolib/dl.cpp, xip/*
  unwind-exidx.h ───────────→ dynamic-nolib/dl.cpp, xip/*

scl::dynamic-nolib
  include/dynamic-nolib/dl.hpp ←── src/dynamic-nolib/dl.cpp
  include/dynamic-nolib/lazy-plt.hpp
  src/port/pic-trampoline.c ──→ (naked asm, called from dl.cpp)
  src/port/dl-cross-call.c ───→ (references dl_cross_lookup in dl.cpp)
  src/port/lazy-resolve.c ────→ (references dl_fixup in dl.cpp)

scl::dynamic-newlib
  include/dynamic-newlib/dynamic-linker.hpp ←── src/dynamic-newlib/dynamic-linker.cpp
  include/dynamic-newlib/patch-journal.hpp  ←── src/dynamic-newlib/patch-journal.cpp
  include/dynamic-newlib/image-memory.hpp   ←── src/dynamic-newlib/image-memory.cpp
  include/dynamic-newlib/link-context.hpp   ←── src/dynamic-newlib/link-context.cpp

scl::static
  include/static/shlib-loader.h ←── src/static/shlib-loader.cpp
  include/static/elf-reader.h   ←── src/static/elf-reader.cpp
  include/static/logger.h       ←── src/static/logger.cpp

scl::overlay
  include/overlay/overlay.h ←── src/overlay/overlay.c

scl::reloc
  include/reloc/reloc-loader.hpp ←── src/reloc/reloc-loader.cpp

scl::xip
  include/xip/segment-map.hpp ←── src/xip/segment-map.cpp
  include/xip/relocate.hpp    ←── src/xip/relocate.cpp
  include/xip/compact.hpp     ←── src/xip/compact.cpp
  include/xip/module.hpp
  xip-adapters: src/xip/{dynamic-nolib,dynamic-newlib,static}/adapter.cpp
```

### 11.8 Call Stack Registers Flow

```
pic_call_u32(fn, got, a0):
  Entry:  r0 = fn addr
          r1 = got_base (callee's GOT)
          r2 = a0 (first argument)
          r10 = caller's got_base
          lr = caller return address
  After trampoline:
          r10 = r1 = got_base (CALLER's r10 saved on stack)
          blx r0 → enters library code
          Library uses r10 to reach its GOT:
            ldr rX, [r10, #offset]  → load global
            ldr rX, [r10, #offset]  → load external fn pointer
  Return:
          pop {r10, pc}  ← restores caller's r10 and return

dl_cross_call (entered from PLTn):
  Entry:  ip(r12) = &GOT[n] in CALLER's image
          lr = caller's caller return
          r0-r3 = original args to library function
          r10 = caller's got_base
          d0-d7 = VFP args
  After lookup:
          r4 = target address
          r5 = callee's got_base
          r10 = r5 (swapped to callee)
          blx r4 → enters callee code
  Return:
          pop {r4-r7, r10, pc} → restores CALLER's r10

dl_runtime_resolve (entered from PLTn):
  Entry:  ip(r12) = &GOT[n] (the slot that needs patching)
          r0-r3 = original args
          d0-d7 = VFP args
          r10 = current lib's got_base
  After fixup:
          GOT[n] = real target | 1
          r12 = target
          restore r0-r3, d0-d7
          bx r12 → enters real function (GOT now patched forever)
```

---

---

## 13. Build System — Firmware, Tests, and Fixtures

This chapter documents the complete build pipeline: CMake targets, toolchain files, platform library, test app helper, fixture build, flash/run scripts, and the integration recipe for adding a new app.

### 13.1 CMake Architecture Overview

The SCL uses a **two-level CMake** structure:

```
Level 1: repo root (CMakeLists.txt)
  └─► scl-common           (INTERFACE, portable C++20)
  └─► scl-dynamic-nolib    (INTERFACE, ARM only, hard-float)
  └─► scl-dynamic-newlib   (INTERFACE, ARM + SCL_HOSTED only)
  └─► scl-static           (INTERFACE, ARM only)
  └─► scl-overlay          (INTERFACE, portable C)
  └─► scl-reloc            (INTERFACE, portable C++)
  └─► scl-xip-*            (INTERFACE, depends on base loader)

Level 2: test/ (CMakeLists.txt)
  └─► includes ../ (SCL_ROOT) → gets all scl::* targets
  └─► includes cmake/scl-test-app.cmake
  └─► includes platforms/weact-f411ce (platform library)
  └─► test-dynamic-nolib/test{1,2,3,4}/
  └─► test-overlay/test1/
  └─► test-dynamic-newlib/test{1,2}/

Standalone:
  test-relocatable/                     (self-contained, scl::reloc only)
  test-static/test-static/               (self-contained, scl::static only)
```

**Key pattern:** All SCL targets are `INTERFACE EXCLUDE_FROM_ALL` (never produce `.a` archives). Sources propagate to the consuming app, which compiles them with its own flags. Only what an app links gets compiled, exactly once per executable (transitively shared sources de-duplicated).

### 13.2 Toolchain Files (`../cmake/`)

Two toolchain files control the cross-build:

#### `../cmake/arm-none-eabi.cmake:1` — Freestanding (hard-float)

```
Target:   Cortex-M4F, hard-float (fpv4-sp-d16)
Flags:    -fpic -O0 -g -ffunction-sections -fno-common
          -ffreestanding -fno-tree-loop-distribute-patterns
C++ add:  -fno-threadsafe-statics
Exceptions/RTTI: ON (main provides EH runtime to loaded libs)
Link:     -pie -nostdlib -nostartfiles
          -Wl,-Bsymbolic -Wl,--unresolved-symbols=ignore-all
          -Wl,-E -Wl,--gc-sections
```

**Used by:** freestanding apps (dyn1, dyn2, flatfs, ovl1), ET_REL app (reloc), and XIP dynamic-newlib adapter.

#### `../cmake/arm-none-eabi-hosted.cmake:1` — Hosted (newlib, soft/hard float)

```
Target:   Cortex-M4, soft-float by default (-DSCL_HF=ON for hard)
Flags:    -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3
C++ add:  -fno-exceptions -fno-rtti -fno-use-cxa-atexit
          -fno-threadsafe-statics -fno-move-loop-invariants
Link:     --specs=nano.specs -u _printf_float
```

**Used by:** hosted test apps (dynamic-newlib1, dynamic-newlib2, XIP dynamic-newlib test).
Note: `scl-dynamic-newlib` and `scl-xip-dynamic-newlib` targets themselves no longer require
`SCL_HOSTED` — they build with the regular `arm-none-eabi.cmake` toolchain. The hosted
toolchain is only needed for the **test apps** that link against newlib startup code.

### 13.3 Platform Library (`../test/platforms/weact-f411ce/`)

The **`platform::weact-f411ce`** INTERFACE target (`../test/platforms/weact-f411ce/cmake/platform-library.cmake:1`) provides:

- **Sources:** `startup.c` (reset vector, .data copy, .bss zero, FPU enable, `main` call), `board.c` (clock init, SysTick), `semihost.c`/`semihost-file.cpp` (ARM semihosting calls), `libc-shim.c` (memcpy/memset/strcmp for `-nostdlib`), `crt.cpp` (operator new/delete), `printf-shim.c`, `dl-semihost.cpp`, `scl-log-semihost.cpp`
- **Link recipe:** `-pie -nostdlib -nostartfiles -Wl,-Bsymbolic --unresolved-symbols=ignore-all -Wl,-E --gc-sections -L<ldscripts>`
- **Linker scripts:** `sections.ld` (main), `sections-overlay.ld` (with `OVERLAY` block), `sections-flatfs.ld` (extended flash sectors), `mem.ld` (FLASH@0x08000000/512K, RAM@0x20000000/128K, ARENA@0x20010000/28K), `mem-overlay.ld` (adds OVL window @0x20018000/16K)

The hosted platform lives at `../test/platforms/weact-f411ce-hosted/vendor/` (vendored CMSIS, HAL, newlib, plus its own `mem-sh.ld`/`sections-dyn.ld`/`libs-sh.ld`).

### 13.4 Test App Helper (`../test/cmake/scl-test-app.cmake:1`)

The `scl_test_app()` function (`../test/cmake/scl-test-app.cmake:33`) creates one firmware executable. Signature:

```cmake
scl_test_app(<target-name>
    LIBS scl::dynamic-nolib         # SCL libraries to link
    LDSCRIPT sections.ld            # linker script from platform ldscripts/
    ANCHORS sym1 sym2 ...           # -Wl,-u keep-alive anchors
    LINK_LIBM                       # append libm.a for hard-float math
    SOURCES extra1.cpp extra2.cpp   # extra app sources
    INCLUDE_DIRS path1 path2        # extra include dirs
)
```

**What it does automatically:**
1. Globs `src/*.c` and `src/*.cpp` from the app directory
2. Adds the platform library (`platform::weact-f411ce`)
3. Links the specified SCL libraries
4. Defines `DL_LIB_DIR=<test/fixtures>` (absolute path to committed `.so` files)
5. Adds EH anchors (`__cxa_throw`, `__cxa_begin_catch`, `__gxx_personality_v0`, `__dynamic_cast`, ...)
6. Resolves `libsupc++.a`, `libgcc.a`, `libm.a` via the cross-compiler's `-print-file-name`
7. Links with the chosen linker script
8. POST_BUILD: `objcopy -O ihex` → `.hex` and `-O binary` → `.bin`
9. Registers a CTest test that runs `flash-run.sh <app>.hex`

### 13.5 Building Each Firmware


#### Freestanding Apps (dyn1, dyn2, flatfs, ovl1)

```sh
# From repo root:
cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-dynamic/build -j
```

**App CMakeLists examples:**

- `../test-dynamic/test-dynamic-nolib/test1/CMakeLists.txt:11` — `scl_test_app(scl-fw-dyn1 LIBS scl::dynamic-nolib ANCHORS printf puts)`
- `../test-dynamic/test-dynamic-nolib/test2/CMakeLists.txt:11` — `scl_test_app(scl-fw-dyn2 LIBS scl::dynamic-nolib ANCHORS sinf cosf logf sqrtf trace_event ... LINK_LIBM)`
- `../test-dynamic/test-dynamic-nolib/test3/CMakeLists.txt:14` — `scl_test_app(scl-fw-flatfs LIBS scl::dynamic-nolib LDSCRIPT sections-flatfs.ld ...)`
- `../test-dynamic/test-dynamic-nolib/test4/CMakeLists.txt:10` — `scl_test_app(scl-fw-gotplt-demo LIBS scl::dynamic-nolib ...)`
- `../test-overlay/test1/CMakeLists.txt:11` — `scl_test_app(scl-fw-ovl1 LIBS scl::dynamic-nolib scl::overlay ...)`

**Outputs:** Each produces `<target>.elf` + `.hex` + `.bin` in the build tree.

#### Hosted Apps (dynamic-newlib1, dynamic-newlib2)

```sh
# From repo root — SOFT float:
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake
cmake --build test/build-dynamic-newlib -j

# HARD float (separate tree):
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib-hf \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake -DSCL_HF=ON
cmake --build test/build-dynamic-newlib-hf -j
```

The hosted platform (`weact-f411ce-hosted`) is vendored under `../test/platforms/weact-f411ce-hosted/vendor/` and provides its own `mem-sh.ld`, `libs-sh.ld`, `sections-dyn.ld` linker scripts (with the `_reloc` table), plus HAL/CMSIS/newlib startup.

#### ET_REL App (scl::reloc)

```sh
# From repo root:
cmake -S test-relocatable -B test-relocatable/build \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-relocatable/build -j
```

The ET_REL app (`../test-relocatable/CMakeLists.txt:26`) is a plain non-PIC firmware — no `-pie`, no `-Wl,-E`, no `.dynsym`/`GOT`/`_DYNAMIC`. It reuses only the **sources** from `weact-f411ce` platform (startup, semihost, board, crt, libc-shim) but its **own** linker script (`../test-relocatable/platform/reloc.ld`). Links `scl::reloc` and builds ET_REL fixtures automatically.

#### Static Patcher App (scl::static)

```sh
# From test-static/test-static/:
cd test-static/test-static
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j                          # → build/f411ce-sh.elf
```

Self-contained project with its own `system/`, `ldscripts/`, `libsh1/2/` fixtures. Links `scl::static`.

### 13.6 Fixture Build System

Fixtures are the prebuilt ARM `.so` files loaded at runtime by the test apps. Two distinct systems:

#### Freestanding Fixtures (`../test/fixtures/`, `../test/fixtures-src/`)

- **Committed prebuilt `.so` files** live in `test/fixtures/` (mylib1/2, libcppsh, libcppeh, c1/c2/cpp3, libprov/cons, ehx_catch, libsh1/2)
- **Sources** in `test/fixtures-src/` per library (C: `mylib1.c`, `c1.c`; C++: `libcppsh.cpp`, `cpp3.cpp`)
- Each has its own Makefile or CMake target. Build recipe (from `build-linking-guide.md`):

```makefile
# C fixture:
CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
          -fpic -msingle-pic-base -mno-pic-data-is-text-relative -O0 -g -fno-common
LDFLAGS = -shared -nostartfiles -Wl,-nmagic -Wl,-E \
          -Xlinker --unresolved-symbols=ignore-all -T ldscripts/gcc.ld
```

```makefile
# C++ fixture:
CXXFLAGS = -std=c++23 -fPIC -shared -fno-threadsafe-statics \
           -fno-use-cxa-atexit -nostdlib -Os
# libcppeh/ehx additionally: exceptions + RTTI ON
```

- **Key linker script** (`../test/fixtures-src/ldscripts/mem.ld`): RAM origin 0, length 35K (all offsets are pure image-relative, loader adds base). `gcc.ld` wraps `mem.ld` + `sections.ld`.
- **`-msingle-pic-base -mno-pic-data-is-text-relative`:** library uses r10 as single PIC base; does not assume data sits at fixed text offset (it's relocated at load time).

#### test-drv Fixtures (C and C++, CMake `add_shlib` / `add_shlib_cpp`)

The `test-static/test-static/test-drv` project uses CMake custom targets to build both C and C++ fixtures:

```cmake
# C fixture (existing):
function(add_shlib name)
    add_custom_target(build_${name}
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS}
                -c ${name}.c -o ${name}.o
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} ${name}.o
                -o lib${name}.so -shared -nostartfiles
                -T ldscripts/mem.ld -Wl,--gc-sections
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib(spi) add_shlib(uart) add_shlib(led)

# C++ fixture (new):
set(LIB_CXXFLAGS ${LIB_CFLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics)
function(add_shlib_cpp name)
    add_custom_target(build_cpp_${name}
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS}
                -c ${name}.cpp -o ${name}.o
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} ${name}.o
                -o lib${name}.so -shared -nostartfiles -nostdlib
                -T ldscripts/mem.ld -Wl,--gc-sections,-S
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib_cpp(cpp_features)
```

Key differences from the C version:
- Uses `-nostdlib` (no libc — the C++ fixture provides its own `new`/`delete` wrapping `malloc`/`free`)
- Linker flag `-Wl,-S` strips debug sections at link time (reduces `.so` from ~87 KB to ~14 KB)
- Compiled with `-fno-exceptions -fno-rtti` (no unwind tables, no typeinfo)
- The fixture is passed as a **link-time input** to the firmware (`target_link_libraries(... libcpp_features.so)`) so the linker creates PLT/GOT entries for direct calls like `cpp_features_run()`

#### Hosted Fixtures (`../test/platforms/weact-f411ce-hosted/vendor/libs/`)

- Built **automatically** by CMake fixture targets during `cmake --build test/build-dynamic-newlib[-hf]`
- Sources in `vendor/libs/{c,cpp}/`, `vendor/debug/test_libs/`, `vendor/debug/test_hardfp/`
- Output in `vendor/libs/build/` (soft-float, 11 `.so`) or `vendor/libs/build_hf/` (hard-float, 13 `.so`, includes hf_test1/2)
- Built via vendored Makefiles called from CMake `add_custom_target`

#### ET_REL Fixtures (`../test-relocatable/fixtures-reloc/`)

- Built via Makefile (`../test-relocatable/fixtures-reloc/Makefile`) called from CMake `add_custom_target(reloc-fixtures ...)`
- Compile flags: `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mlong-calls -fno-pic -Os -ffreestanding -fno-common`
- Link: `-r` (partial link, stays ET_REL)
- Recipe from `build-linking-guide.md §3.5`:
  - `-fno-pic`: not position-independent, runs at fixed absolute address
  - `-mlong-calls`: emit `ldr rN,=target; blx rN` (reaches flash from RAM without GOT/PLT)
  - `-r`: keep relocatable, symbols stay UND for loader resolution

### 13.7 Running on Target

#### Flash Scripts (`../test/tools/flash-run.sh`)

The main script `../test/tools/flash-run.sh:1`:

1. Accepts an **app name** (e.g. `scl-fw-dyn2`) or an explicit `.hex` path
2. For app names: searches build trees under the SCL root for the newest matching `.hex`
3. For newlib static apps: `SCL_DYNAMIC_NEWLIB_VARIANT=sf|hf` selects the build tree and fixture set
4. For flat-fs apps: passes `arm semihosting_cmdline` with fixture names (provisioning list)
5. Stages fixtures to `/tmp` for the newlib2 suite (which hardcodes `/tmp/*.so` paths)
6. **ABI guard:** verifies the staged fixtures' float ABI matches the firmware (via `readelf -A` → `Tag_ABI_VFP_args`). Mismatch aborts with a clear message.
7. Launches OpenOCD: `init` → `reset halt` → `program <hex>` → `arm semihosting enable` → `reset run`
8. Semihosting console streams back to the terminal; Ctrl-C stops

**Wrappers:**
- `../test/tools/flash-run-sf.sh:1` — sets `SCL_DYNAMIC_NEWLIB_VARIANT=sf`, calls `flash-run.sh`
- `../test/tools/flash-run-hf.sh` — sets `SCL_DYNAMIC_NEWLIB_VARIANT=hf`, calls `flash-run.sh`

**Via CTest:**

```sh
ctest --test-dir test-dynamic/build --output-on-failure               # 5 tests
ctest --test-dir test/build-dynamic-newlib --output-on-failure    # 2 tests (SF)
ctest --test-dir test/build-dynamic-newlib-hf --output-on-failure # 2 tests (HF)
ctest --test-dir test-relocatable/build --output-on-failure             # 2 tests
```

#### ET_REL Flash (`../test-relocatable/tools/flash_reloc_run.sh`)

Single-shot script: builds app + fixtures → flashes → runs, from the repo root. Equivalent to the CMake build + `ctest` sequence above.

### 13.8 Adding a New Test App

Minimal template — create `test/test-<flavour>/testN/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)       # allow standalone configure
    project(scl-fw-<name> LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/scl-test-app.cmake)
endif()

scl_test_app(scl-fw-<name>
    LIBS scl::dynamic-nolib             # SCL target(s)
    LDSCRIPT sections.ld                # from platform ldscripts/
    ANCHORS printf puts                 # extra -Wl,-u anchors
    LINK_LIBM)                          # if hard-float math needed
```

Place `src/main.cpp` and optional `include/` next to the CMakeLists. The app sources are globbed automatically. Fixtures go in `test/fixtures/` (prebuilt `.so`) or the hosted vendor tree.

### 13.9 Full Build Matrix

| Build Dir | Configure Command | Apps | Toolchain |
|-----------|-----------------|------|-----------|
| `test-dynamic/build/` | `cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | dyn1, dyn2, flatfs, gotplt-demo, ovl1 | arm-none-eabi (freestanding/hard) |
| `test/build-dynamic-newlib/` | `cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake` | newlib1, newlib2 + SF fixtures | arm-none-eabi (hosted/soft) |
| `test/build-dynamic-newlib-hf/` | `... -DSCL_HF=ON` | newlib1, newlib2 + HF fixtures | arm-none-eabi (hosted/hard) |
| `test/build-static/` | `cmake -S test-static/test-static -B test/build-static -DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake` | f411ce-sh | arm-none-eabi (hosted/soft) |
| `test-relocatable/build/` | `cmake -S test-relocatable -B test-relocatable/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | scl-fw-reloc, scl-fw-classic-reloc | arm-none-eabi (freestanding/hard, no-PIC) |

### 13.10 Compile-Time Constants Reference

| Constant | Defined by | Consumed by | Meaning |
|----------|-----------|-------------|---------|
| `DL_LIB_DIR` | `scl_test_app()` → `target_compile_definitions` | app `main.cpp` | Absolute path to `.so` fixtures for semihosting |
| `SCL_HOSTED` | SCL CMake when option ON | `elf-image.hpp`, SCL CMake | Enables `std::vector` parts, `scl::dynamic-newlib` |
| `SCL_SO_DIR` | newlib1 CMake | its `main.cpp` | Dir for hosted fixture `.so` |
| `HARD_FLOAT` | hosted platform when `-DSCL_HF=ON` | test selection in `tests-dyn.cpp` | Enables hard-float test set |
| `RELOC_LIB_DIR` | `test-relocatable/CMakeLists.txt` | reloc test `main.cpp` | Dir for ET_REL fixture `.rel.o` files |
| `SCL_OVERLAY_NONE` (= -1) | `../include/overlay/overlay.h:37` | overlay users | "no module resident" constant |

### 13.11 Requirements Reference: Tools and Default Paths

| Tool | Default Location | Used by | Override |
|------|-----------------|---------|----------|
| `arm-none-eabi-gcc/g++/objcopy/size/gdb/nm` | `~/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin` | All cross builds | `-DSCL_ARM_TOOLCHAIN_BIN=<dir>` (CMake), or `PATH` |
| `openocd` | `~/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin` | `test/tools/flash-run*.sh` | `OPENOCD=` env var |
| OpenOCD scripts | `~/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts` | `test/tools/flash-run*.sh` | `OPENOCD_SCRIPTS=` env var |
| CMake ≥ 3.20 | system | All builds | system |
| Python 3 | system | `make-pdfs.py`, `embed-blob.py`, `pack-flatfs.py` | system |
| `make` | system | Vendored fixture Makefiles (called by CMake) | system |

### 13.12 Complete CMakeLists.txt Reference

All 29 CMakeLists.txt files, organised hierarchically from repository root to leaf executable targets.

---

#### Level 0 — Repository Root

##### `CMakeLists.txt` (root)
**Path:** `../CMakeLists.txt`
**Project:** `cortex-m-scl`
**Role:** SCL library package. Defines **6 INTERFACE library targets** (no `.a` archives): `scl-common`, `scl-dynamic-nolib`, `scl-dynamic-newlib`, `scl-static`, `scl-overlay`, `scl-reloc`, plus `scl-xip-common` and three XIP adapters (`scl-xip-dynamic-nolib`, `scl-xip-dynamic-newlib`, `scl-xip-static`).
**Key points:**
- Pulls in `common-elf-reader/` submodule via `add_subdirectory`
- All targets are `INTERFACE EXCLUDE_FROM_ALL` — sources propagate to consumers
- Creates namespaced aliases: `scl::common`, `scl::overlay`, `scl::reloc`, etc.
**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `-DCOMMON_ELF_READER_DIR=<path>` | `./common-elf-reader` | Path to the git submodule |
| `-DSCL_BUILD_TESTS=ON\|OFF` | `ON` (top-level) | Enable CTest registration |
| `-DSCL_HOSTED=ON\|OFF` | `OFF` (ARM), `ON` (host) | Build `std::vector`-dependent parts |
| `-DCMAKE_TOOLCHAIN_FILE=<path>` | — | Not needed for host build |

---

#### Level 1 — Git Submodule

##### `common-elf-reader/CMakeLists.txt`
**Path:** `../common-elf-reader/CMakeLists.txt`
**Target:** `common-elf-reader::elf-reader` (INTERFACE)
**Role:** Shared ELF32 parser: `elf_image`, `byte_source`, `error`/`result<T>`, ELF types, bounded byte accessors.
**Sources:** `src/elf-image.cpp`
**Options:**

| Option | Inherited | Description |
|--------|-----------|-------------|
| `-DSCL_HOSTED=ON\|OFF` | from root CMake | Gates `elf_image::collect_relocations()` |

---

#### Level 2 — Test Root

##### `test/CMakeLists.txt`
**Path:** `../test/CMakeLists.txt`
**Project:** `scl-tests`
**Role:** Top-level firmware test aggregator. Guards against non-cross-compile (must use ARM toolchain). Adds `SCL_ROOT/` as `scl` subdirectory, includes `cmake/scl-test-app.cmake` helper, adds `test-dynamic-newlib/`, `test-dynamic-nolib/`, `test-overlay/`.
**Options:**
- `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` — **required** (freestanding ARM)
- `-DSCL_ARM_TOOLCHAIN_BIN=<dir>` — override ARM toolchain path

---

#### Level 3 — Test Subprojects

##### `test-dynamic/test-dynamic-nolib/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-nolib/CMakeLists.txt`
**Role:** Aggregates 4 test apps: `test1` (demo walkthrough), `test2` (16-test battery), `test3` (flat-fs backend), `test4` (GOT/PLT lazy-binding demo). No targets of its own. No options.

##### `test-dynamic/test-dynamic-newlib/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-newlib/CMakeLists.txt`
**Role:** Aggregates `test1` (SCL battery, journal-revert proof) and `test2` (full DYN suite). Self-skips if not configured with hosted toolchain.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake` | **required** (hosted ARM) |
| `-DSCL_HF=ON\|OFF` | Hard-float variant (separate build tree) |

##### `test-overlay/CMakeLists.txt`
**Path:** `../test-overlay/CMakeLists.txt`
**Role:** Adds `test1/` subdirectory only. No options.


---

#### Level 4 — Test Executables (Firmware Apps)

##### `test-dynamic/test-dynamic-nolib/test1/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-nolib/test1/CMakeLists.txt`
**Target:** `scl-fw-dyn1`
**Role:** Demo firmware — dlopen/dlsym/call/dlclose of mylib1/2, libcppsh (C++), libcppeh (exceptions).
**Config:** `scl_test_app(scl-fw-dyn1 LIBS scl::dynamic-nolib ANCHORS printf puts)`
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |
| `-DDL_LIB_DIR=<path>` | set automatically by `scl_test_app` to `test/fixtures` |

##### `test-dynamic/test-dynamic-nolib/test2/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-nolib/test2/CMakeLists.txt`
**Target:** `scl-fw-dyn2`
**Role:** 16-test cross-module battery (c1/c2/cpp3, r10 routing, lazy vs eager, EH, GOT walkthroughs).
**Config:** `scl_test_app(scl-fw-dyn2 LIBS scl::dynamic-nolib ANCHORS sinf cosf logf sqrtf trace_event trace_u32 fm_callback fm_var fm_info printf puts LINK_LIBM)`
**Options:** Same as test1.

##### `test-dynamic/test-dynamic-nolib/test3/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-nolib/test3/CMakeLists.txt`
**Target:** `scl-fw-flatfs`
**Role:** Flat-fs battery — provisions on-chip flash archive, then runs 16-test battery from flash via `dlopen_fs()`.
**Config:** `scl_test_app(scl-fw-flatfs LIBS scl::dynamic-nolib LDSCRIPT sections-flatfs.ld ANCHORS ... LINK_LIBM)`
**Options:** Same as test1, plus `LDSCRIPT sections-flatfs.ld` (selected by `scl_test_app`).

##### `test-dynamic/test-dynamic-nolib/test4/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-nolib/test4/CMakeLists.txt`
**Target:** `scl-fw-gotplt-demo`
**Role:** GOT[2]/PLT0 standard lazy-binding demonstration and GOT walkthrough.
**Config:** `scl_test_app(scl-fw-gotplt-demo LIBS scl::dynamic-nolib ANCHORS ... LINK_LIBM)`
**Options:** Same as test1.

##### `test-dynamic/test-dynamic-newlib/test1/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-newlib/test1/CMakeLists.txt`
**Target:** `scl-fw-dynamic-newlib1`
**Role:** Static-loader battery — load mylib1.so over semihosting, dlsym/call, negative lookup, dlclose, reload proves journal revert, missing-file error.
**Config:** Links `platform::weact-f411ce-hosted` + `scl::dynamic-newlib`. Builds fixture `.so` via custom `make` target. Uses hosted toolchain.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake` | **required** |
| `-DSCL_HF=ON\|OFF` | hard-float variant (separate build dir) |

##### `test-dynamic/test-dynamic-newlib/test2/CMakeLists.txt`
**Path:** `../test-dynamic/test-dynamic-newlib/test2/CMakeLists.txt`
**Target:** `scl-fw-dynamic-newlib2`
**Role:** Full static-loader DYN suite — GOT/PLT patching, concurrent libs, coverage libs, cross-library calls, C++ tests, hard-float tests.
**Config:** Same as test1 but builds larger fixture set (libs/c, libs/cpp, debug/test_libs, hf_test1/2). Stages fixtures to `/tmp`.
**Options:** Same as newlib test1.

##### `test-overlay/test1/CMakeLists.txt`
**Path:** `../test-overlay/test1/CMakeLists.txt`
**Target:** `scl-fw-ovl1`
**Role:** Overlays-vs-dynamic comparison — same 3 scenarios via overlay (link-time) vs dynamic (runtime relocation), side-by-side addresses.
**Config:** `scl_test_app(scl-fw-ovl1 LIBS scl::dynamic-nolib scl::overlay LDSCRIPT sections-overlay.ld ANCHORS ... LINK_LIBM)`. Overlay sources compiled `-fno-pic -mlong-calls`.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |
| `-DDL_LIB_DIR=<path>` | set automatically |

##### `test-static/test-static/CMakeLists.txt`
**Path:** `../test-static/test-static/CMakeLists.txt`
**Target:** `f411ce-sh`
**Role:** Static patcher demo firmware. Self-contained: own linker scripts (`mem-sh.ld` + `libs-sh.ld` + `sections-sh.ld`), own `system/`, own `libsh1/2` fixtures. Defines `add_shlib()` macro for custom `-fPIC -shared -nostartfiles` targets.
**Config:** Links `scl::static`, three custom ldscripts, `-lsh1 -lsh2`, `-nostartfiles`, `--gc-sections`, `--specs=nano.specs`. Generates `.bin`, `.lst`, runs `size` post-build.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake` | self-contained ARM soft-float |
| `-DCMAKE_BUILD_TYPE=Debug\|Release` | build configuration |

---


---

#### Standalone — ET_REL Test Project

##### `test-relocatable/CMakeLists.txt`
**Path:** `../test-relocatable/CMakeLists.txt`
**Targets:** `scl-fw-reloc`, `scl-fw-classic-reloc`
**Role:** Self-contained non-PIC firmware for ET_REL module loader. Reuses platform *sources* from `../test/platforms/weact-f411ce/` but its **own** linker script `platform/reloc.ld`. Links `scl::reloc`. Compiles `-fno-pic -fno-pie` (no GOT/PLT). Builds ET_REL fixtures via `make -C fixtures-reloc/`. Embeds flat-fs blobs via `embed-blob.py` and `pack-flatfs.py`.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |
| `-DRELOC_LIB_DIR=<path>` | fixture dir (set automatically to `fixtures-reloc/build`) |

---

#### Standalone — XIP Test Project

##### `test-xip/CMakeLists.txt`
**Path:** `../test-xip/CMakeLists.txt`
**Project:** `scl-xip-tests`
**Role:** Top-level XIP firmware test aggregator. Adds `test-dynamic-nolib/`, `test-dynamic-newlib/`, `test-static/` subdirectories. Requires ARM cross-compiler.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |


##### `test-xip/test-dynamic-nolib/CMakeLists.txt`
**Path:** `../test-xip/test-dynamic-nolib/CMakeLists.txt`
**Role:** Single-line: adds `test1/` subdirectory. No options.

##### `test-xip/test-dynamic-nolib/test1/CMakeLists.txt`
**Path:** `../test-xip/test-dynamic-nolib/test1/CMakeLists.txt`
**Target:** `scl-fw-xip-nolib1`
**Role:** XIP dynamic-nolib battery — loads all 10 fixtures from flash (R-X in flash, RW in RAM), full surface exercise. Links `scl::xip-dynamic-nolib`. `-pie`, `-nostartfiles`, `-Bsymbolic`, `-E`. Copies PIC `.so` fixtures for semihosting.

##### `test-xip/test-dynamic-newlib/CMakeLists.txt`
**Path:** `../test-xip/test-dynamic-newlib/CMakeLists.txt`
**Role:** Single-line: adds `test1/` subdirectory. No options.

##### `test-xip/test-dynamic-newlib/test1/CMakeLists.txt`
**Path:** `../test-xip/test-dynamic-newlib/test1/CMakeLists.txt`
**Target:** `scl-fw-xip-newlib1`
**Role:** XIP dynamic-newlib battery. Links `scl::xip-dynamic-newlib`. Same PIC/XIP flags as nolib variant.

##### `test-xip/test-static/CMakeLists.txt`
**Path:** `../test-xip/test-static/CMakeLists.txt`
**Target:** `scl-fw-xip-static`
**Role:** XIP static loader test — basic load and call from flash. Links `scl::xip-common`. Compiles `pic-trampoline.c`, `image-memory.cpp`, `link-context.cpp`, `unwind-find-exidx.c` directly.

**XIP options (all three XIP firmware targets):**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |

---

#### Summary Table

| # | Depth | File | Options | What It Does |
|---|-------|------|---------|-------------|
| 1 | root | `../CMakeLists.txt` | `COMMON_ELF_READER_DIR`, `SCL_BUILD_TESTS`, `SCL_HOSTED` | SCL library package: 6 INTERFACE targets + aliases |
| 2 | submodule | `../common-elf-reader/CMakeLists.txt` | (inherits `SCL_HOSTED`) | elf_image/byte_source parser INTERFACE target |
| 3 | test/ | `../test/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `SCL_ARM_TOOLCHAIN_BIN` | Firmware test aggregator (cross only) |
| 11 | test/ | `../test-dynamic/test-dynamic-nolib/CMakeLists.txt` | (none) | Aggregates dyn1/2/flatfs/gotplt-demo |
| 12 | test/ | `../test-dynamic/test-dynamic-nolib/test1/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `DL_LIB_DIR` | dlopen demo firmware |
| 13 | test/ | `../test-dynamic/test-dynamic-nolib/test2/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `DL_LIB_DIR` | 16-test cross-module battery |
| 14 | test/ | `../test-dynamic/test-dynamic-nolib/test3/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `DL_LIB_DIR` | Flat-fs backend firmware |
| 15 | test/ | `../test-dynamic/test-dynamic-nolib/test4/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `DL_LIB_DIR` | GOT[2]/PLT0 lazy-binding demo |
| 16 | test/ | `../test-dynamic/test-dynamic-newlib/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `SCL_HF` | Aggregates newlib1/2 (hosted only) |
| 17 | test/ | `../test-dynamic/test-dynamic-newlib/test1/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `SCL_HF` | Static-loader battery |
| 18 | test/ | `../test-dynamic/test-dynamic-newlib/test2/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `SCL_HF` | Full DYN suite |
| 19 | test/ | `../test-overlay/CMakeLists.txt` | (none) | Adds overlay test1 subdir |
| 20 | test/ | `../test-overlay/test1/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `DL_LIB_DIR` | Overlays-vs-dynamic comparison |
| 21 | test/ | `../test-static/test-static/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | Static patcher demo (own ldscripts) |
| 22 | test-relocatable/ | `../test-relocatable/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE`, `RELOC_LIB_DIR` | ET_REL firmware (non-PIC, scl::reloc) |
| 23 | test-xip/ | `../test-xip/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | XIP firmware aggregator |
| 25 | test-xip/ | `../test-xip/test-dynamic-nolib/CMakeLists.txt` | (none) | Adds XIP nolib test1 subdir |
| 26 | test-xip/ | `../test-xip/test-dynamic-nolib/test1/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | XIP dynamic-nolib battery |
| 27 | test-xip/ | `../test-xip/test-dynamic-newlib/CMakeLists.txt` | (none) | Adds XIP newlib test1 subdir |
| 29 | test-xip/ | `../test-xip/test-static/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | XIP static loader test |


## 14. Advanced Interrupt Handling and Context Switching

When developing bare-metal or real-time embedded systems that support dynamic code loading (such as ELF shared libraries or relocatable objects), handling hardware interrupts (IRQs) within loaded modules introduces significant architectural challenges.

This section details the fundamental registers of Cortex-M, the design patterns to handle interrupts, preemptive Hard-FPU context switching, and handling timer interrupts during semihosting-based debugging.

---

### 14.1 The Fundamental Architecture Obstacle: The `r10` GOT Base

In Cortex-M microcontrollers, dynamically loaded shared libraries (`.so` / `ET_DYN`) are compiled with Position-Independent Code flags:
* `-fPIC` (or `-fpic`)
* `-msingle-pic-base` (which enforces a single Global Offset Table per module)

In this compile model, access to all global variables, static variables, and external functions is resolved relative to a designated **base register** (specifically `r10` / `sl` in the GNU ARM toolchain). 

#### The Interrupt Entry Conflict
When a peripheral triggers an interrupt, the Cortex-M core performs the following hardware sequence:
1. **Context Stacking**: The CPU automatically pushes caller-saved registers (`r0`-`r3`, `r12`, `lr`, `pc`, `xPSR`) to the active stack pointer (`MSP` or `PSP`).
2. **Execution Switch**: The core switches to **Handler Mode** using the Main Stack Pointer (`MSP`).
3. **ISR Vector Branch**: The CPU loads the target address from the vector table (pointed to by the `VTOR` register) and branches directly to it.

**Crucially, the hardware does not modify or load `r10`.** The `r10` register retains whatever value was present at the exact clock cycle the interrupt occurred. This could be:
* The main firmware's GOT base address.
* The GOT base of a completely different dynamic module.
* Uninitialized garbage.

If the hardware branches directly to an ISR inside the loaded library, the C/C++ function will attempt to read or write its global variables using `r10`. Because `r10` points to the wrong GOT (or invalid memory), this instantly triggers a **UsageFault**, **BusFault**, or **HardFault**.

---

### 14.2 Design Patterns for Interrupt Handling

Depending on your loader flavor (freestanding dynamic, hosted dynamic, or relocatable), choose one of the following implementation patterns.

#### Pattern A: Main Firmware Trampoline (Recommended for `scl::dynamic-*`)
The hardware vector table resides permanently in the main firmware. The main firmware exposes a static ISR that acts as an execution trampoline, manually configuring the dynamic library's register context before executing the handler.

```c
/* Main Firmware Source */

// Pointers resolved during dynamic module loading
extern void* g_fixture_got_base;         // The library's got_base (r10 target)
extern void (*g_fixture_isr_handler)(void); // The target library function

/**
 * @brief Hardware ISR wrapper defined in the main vector table
 */
void USART1_IRQHandler(void) {
    if (g_fixture_isr_handler && g_fixture_got_base) {
        // Save current r10 in r4 (callee-saved), set new r10, branch, restore r10
        __asm__ volatile (
            "mov r4, r10\n"           // Save current r10 to r4
            "mov r10, %0\n"           // Load fixture GOT base into r10
            "blx %1\n"                // Call the fixture ISR handler
            "mov r10, r4\n"           // Restore original r10
            :
            : "r"(g_fixture_got_base), "r"(g_fixture_isr_handler)
            : "r4", "lr", "memory"
        );
    }
}
```

#### Pattern B: Direct Vector Table Patching with Relocatable Objects (`scl::reloc`)
If your architecture requires dynamically registering interrupts directly with the hardware (e.g., zero-latency interrupt execution without trampoline overhead), you should compile modules as **Relocatable Objects** (`.rel.o` / `ET_REL`) rather than shared libraries.

##### How `scl::reloc` Solves the Register Dependency
* **No PIC Requirements**: Relocatable modules are compiled with `-fno-pic -mlong-calls -r`. 
* **Absolute Linking at Load Time**: The relocatable loader (`scl::reloc`) allocates raw memory segments for the module's sections (`.text`, `.data`, `.bss`) and applies relocations directly in place. All relative offsets are converted into absolute address instructions.
* **No `r10` Dependency**: Since no GOT is used, functions inside relocatable modules do not rely on a base register. They are normal, absolute function pointers.

```c
/* Main Firmware Source Vector Table Patching */
void register_dynamic_irq(IRQn_Type irq_type, void (*resolved_isr)(void)) {
    uint32_t* ram_vectors = (uint32_t*)SCB->VTOR;
    uint32_t vector_offset = (int32_t)irq_type + 16;
    
    // Write absolute address directly (safe because relocatable code has no GOT/r10 dependencies)
    ram_vectors[vector_offset] = (uint32_t)resolved_isr;
    NVIC_EnableIRQ(irq_type);
}
```

#### Pattern C: Self-Contained Assembly Wrapper (Module-Side Trampoline)
If the module is a shared library (`.so`) but needs to register itself directly into a RAM-based vector table, the registered pointer **cannot** point to the C-coded ISR. It must point to a naked assembly wrapper inside the module.

This wrapper is responsible for determining the module's GOT base address, configuring `r10`, and executing the handler.

```assembly
/* Module Assembly Wrapper (handler.S) */
.syntax unified
.cpu cortex-m4
.thumb

.section .text.isr_wrapper, "ax", %progbits
.global my_module_isr_wrapper
.type my_module_isr_wrapper, %function

my_module_isr_wrapper:
    push {r4, lr}
    
    // Load the GOT base address relative to the PC
    ldr r4, =_GLOBAL_OFFSET_TABLE_
    mov r10, r4
    
    // Call the actual C handler
    bl my_module_isr_c_handler
    
    pop {r4, pc}
    .size my_module_isr_wrapper, .-my_module_isr_wrapper
```

#### Pattern D: Dynamic RAM Vector Table Patching with Trampoline (Recommended for `scl::xip-static`)
If the dynamic module is compiled as a position-independent shared library (`-fPIC` which requires `r10` for local static/global access), but the system design requires dynamically registering the interrupt vector at runtime rather than hardcoding it in the ROM/Flash vector table, we can combine RAM vector table patching with a firmware-side trampoline.

##### How it works
1. **RAM Vector Table Initialization**: At boot, the main firmware copies the hardware vector table from Flash to RAM, aligning it to a 512-byte boundary (Cortex-M4 alignment requirement for a 128-word table), and points `SCB->VTOR` to the RAM copy.
2. **Dynamic Registration on Load**: When the static loader loads the module, we resolve the target handler and the library's `got_base`. We then dynamically patch the timer's vector slot in the RAM table with the address of a firmware-side trampoline function (`my_dynamic_tim2_handler`).
3. **Context-Saving Trampoline**: The trampoline preserves the caller's registers, loads the dynamic module's `got_base` into `r10`, calls the C handler, and restores `r10`.
4. **Dynamic Restoration on Unload**: When the module is unloaded, the vector slot in the RAM table is restored to `nullptr`.

##### Implementation Example
```c
/* Main Firmware Source */

// Align to 512 bytes for Cortex-M4 vector table requirements (16 exceptions + 86 interrupts)
__attribute__((aligned(512))) static uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
    uint32_t* flash_vectors = (uint32_t*)SCB->VTOR;
    for (int i = 0; i < 128; i++) {
        g_ram_vector_table[i] = flash_vectors[i];
    }
    __disable_irq();
    SCB->VTOR = (uint32_t)g_ram_vector_table;
    __DSB(); __ISB();
    __enable_irq();
}

void register_tim2_isr(void (*isr)(void)) {
    __disable_irq();
    g_ram_vector_table[TIM2_IRQn + 16] = (uint32_t)isr;
    __DSB(); __ISB();
    __enable_irq();
}

// Trampoline wrapper in main firmware
extern "C" void my_dynamic_tim2_handler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;

        blink_leds[0].turn_on();
        delay_ms(100);
        blink_leds[0].turn_off();

        // Swap r10 with got_base and execute resolved handler
        if (g_fixture_systick_handler && g_fixture_got_base) {
            unsigned int toggle = 0;
            __asm__ volatile (
                "mov r4, r10\n"
                "mov r10, %1\n"
                "mov r0, #1\n"
                "blx %2\n"
                "mov %0, r0\n"
                "mov r10, r4\n"
                : "=r"(toggle)
                : "r"(g_fixture_got_base), "r"(g_fixture_systick_handler)
                : "r0", "r4", "lr", "memory"
            );
        }
    }
}
```

---

### 14.3 Preemptive Context Switching with Hard-FPU

When integrating a multitasking scheduler (such as an RTOS) in a Cortex-M4 environment with a hardware Floating-Point Unit (FPU), the context switcher must handle FPU register states and maintain strict alignment constraints.

#### 14.3.1 FPU Register Stacking and `EXC_RETURN`
Upon exception entry, if the FPU was active (i.e. `CONTROL.FPCA == 1`), the Cortex-M4 hardware automatically stacks registers `s0-s15` and `FPSCR` in addition to the standard core registers. 
The software context switcher (`PendSV_Handler`) is responsible for saving and restoring the remaining callee-saved FPU registers (`s16-s31`) conditionally based on the `EXC_RETURN` value.

* **FPU Frame Detection**: Bit 4 of the Link Register (`lr` / `EXC_RETURN`) determines whether an FPU frame was stacked by the hardware:
  * `0`: FPU context active; FPU frame present on stack.
  * `1`: Standard context; FPU frame not present.
* **Conditional Stacking Assembly**:
  ```assembly
  // Check if FPU context was active (bit 4 of lr is 0)
  tst lr, #0x10
  it eq
  vstmdbeq r0!, {s16-s31}   @ Save FPU registers s16-s31
  ```

#### 14.3.2 Enforcing Strict 8-byte Stack Alignment
Under the ARM Architecture Procedure Call Standard (AAPCS), the stack pointer must be aligned to an **8-byte boundary** at all external function calls. 
* **The Problem**: Pushing the core registers `r4-r11, lr` (9 registers = 36 bytes) shifts the stack pointer by a value that is not a multiple of 8, breaking alignment.
* **The Solution**: We include a padding register (e.g. `r3`) in the software stacking list, expanding the list to 10 registers (`r3-r11, lr` = 40 bytes), which preserves 8-byte alignment.
  ```assembly
  stmdb r0!, {r3-r11, lr}   @ 10 registers (40 bytes), maintains 8-byte alignment
  ```

#### 14.3.3 Dynamic Stack-Selection Context Switcher (MSP / PSP Isolation)
In a hybrid multitasking system, we can configure Task 0 (the main firmware thread) to execute on the Main Stack Pointer (`MSP`) to support debuggers and standard libraries, while Task 1 (e.g., a background LED task) executes on an isolated Process Stack Pointer (`PSP`).

To do this, the context switcher must dynamically load/store from the active stack based on bit 2 of `EXC_RETURN` (which indicates whether `MSP` or `PSP` was active):
* `0`: Interrupted context was using `MSP`.
* `1`: Interrupted context was using `PSP`.

```assembly
/* Dynamic Stack-Selection Context Switcher */
extern "C" __attribute__((naked)) void PendSV_Handler(void) {
    __asm__ volatile (
        "cpsid i\n"

        // 1. Check active stack pointer (bit 2 of EXC_RETURN in lr)
        "tst lr, #4\n"
        "ite ne\n"
        "mrsne r0, psp\n"               // If PSP was active, load into r0
        "mrseq r0, msp\n"               // If MSP was active, load into r0

        // 2. Conditionally stack FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vstmdbeq r0!, {s16-s31}\n"

        // 3. Stack core registers + EXC_RETURN (including r3 for 8-byte alignment)
        "stmdb r0!, {r3-r11, lr}\n"

        // 4. Save SP back to current TCB
        "ldr r1, =g_current_task\n"
        "ldr r1, [r1]\n"
        "ldr r2, =g_tasks\n"
        "lsl r1, r1, #3\n"
        "str r0, [r2, r1]\n"

        // 5. Select next task
        "bl select_next_task\n"

        // Set g_current_task = r0
        "ldr r1, =g_current_task\n"
        "str r0, [r1]\n"

        // 6. Load new task SP
        "ldr r2, =g_tasks\n"
        "lsl r0, r0, #3\n"
        "ldr r0, [r2, r0]\n"

        // 7. Pop core registers
        "ldmia r0!, {r3-r11, lr}\n"

        // 8. Conditionally pop FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vldmiaeq r0!, {s16-s31}\n"

        // 9. Restore stack pointer to the correct register (MSP or PSP) based on bit 2 of new lr
        "tst lr, #4\n"
        "ite ne\n"
        "msrne psp, r0\n"
        "msreq msp, r0\n"

        "cpsie i\n"
        "bx lr\n"
        : : : "memory"
    );
}
```

---

### 14.4 Debugging & Timer Interrupts under Semihosting

If the application uses **semihosting** (such as printing log output via `trace_printf` over SWD/JTAG), debugging interrupts becomes a challenge because of how the CPU core behaves during debugger halts.

#### 14.4.1 The SysTick Debug Freeze
The SysTick timer is physically integrated directly inside the ARM Cortex-M processor core. Because of this integration, **whenever the CPU core is halted by a debugger, the SysTick timer freezes instantly and stops counting.**

This behavior causes a severe conflict when the application utilizes **semihosting** (such as printing test logs to the host PC via `trace_printf()`):

1. **How Semihosting Halts the Core**:
   Every time the firmware calls `trace_printf()`, it executes a `BKPT` (breakpoint) instruction. The processor immediately halts. The hardware debugger (e.g., OpenOCD/GDB via JTAG/SWD) detects the halt, reads the log text out of the microcontroller's RAM, prints it to your PC's command line, and then instructs the CPU to resume execution.
   
2. **The Time Inflation Discrepancy**:
   Because sending characters to a PC terminal over a debug probe is slow, each print statement pauses the CPU for **100 to 200 milliseconds** of real-world desk time. However, the CPU active execution time *between* these print statements (the actual test math) is extremely fast, taking only **a few microseconds**.

3. **Why the Interrupt Never Fires**:
   * **Real-world time elapsed on your desk**: 10 seconds (due to 50 print statements pausing the core for 200ms each).
   * **Active CPU time counted by SysTick**: 0.0005 seconds (50 active test blocks of 10 microseconds each, as SysTick was frozen during all print pauses).
   
   Since SysTick only registers 0.0005 seconds of elapsed time during the entire test suite run, the timer is nowhere near its 1.0-second limit. Therefore, the SysTick interrupt never fires, and the LED remains completely dark during the tests.
   
   Only when all prints finish and the CPU enters the final continuous loop (`for (;;) { wfi; }`) does SysTick finally accumulate enough active cycles to reach 1.0 second and begin blinking the LED.

#### 14.4.2 The APB Peripheral Timer Advantage (`TIM2`/`TIM3`)
To make the LED blink during semihosting execution, we must use a **Hardware Timer** (like `TIM2`) instead of SysTick.
* **Continuous Bus Clocking**: Hardware timers are peripheral modules running on the APB bus. By default, they continue to count even when the CPU core is halted by a debugger.
* **Instant Interrupt Execution**: While the core is halted, `TIM2` continues counting. Once the debugger resumes the CPU core, the timer immediately asserts its interrupt request, triggering `TIM2_IRQHandler` to execute and blink the LED during active test windows.

```cpp
/* Configure TIM2 to overflow every 1.0 second */
void timer_start(void) {
    // 1. Enable TIM2 clock on APB1
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    __asm__ volatile ("dsb\n isb\n");

    // 2. Fetch APB1 clock frequency and calculate timer clock frequency
    std::uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    std::uint32_t tim_clk = pclk1;
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0) {
        tim_clk = 2 * pclk1;
    }

    // 3. Configure Prescaler to tick at 10 kHz, and Auto-reload for 1Hz overflow
    TIM2->PSC = (tim_clk / 10000) - 1;
    TIM2->ARR = 10000 - 1;

    // 4. Clear flag, enable interrupt, enable NVIC, and start timer
    TIM2->CNT = 0;
    TIM2->SR = ~TIM_SR_UIF;
    TIM2->DIER |= TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 |= TIM_CR1_CEN;
}
```

#### 14.4.3 The 24-bit SysTick Reload Register Limit
When using SysTick, the reload register is only **24 bits wide**, allowing a maximum value of `16,777,215`.
* **The Failure**: Attempting to load `SystemCoreClock` (e.g., `100,000,000` or `168,000,000` for a 1-second interval) overflows the register, causing `SysTick_Config` to immediately fail and abort configuration. The timer then falls back to the default high-frequency 1000 Hz tick rate configured by the HAL.
* **The Solution**: If SysTick is required for low-frequency intervals, configure it to run at a high frequency (like **1000 Hz**), and use volatile tick counters inside the ISR to implement low-frequency actions:
  ```cpp
  extern "C" void SysTick_Handler(void) {
      g_systick_counter = g_systick_counter + 1;
      if (g_systick_counter >= 1000) { // 1000ms = 1.0 second
          g_systick_counter = 0;
          blink_leds[0].turn_on();
          g_led_off_counter = 100;    // LED on for 100ms
      }
      
      if (g_led_off_counter > 0) {
          g_led_off_counter = g_led_off_counter - 1;
          if (g_led_off_counter == 0) {
              blink_leds[0].turn_off();
          }
      }
  }
  ```

#### 14.4.4 Early Boot Starvation Guard
At boot, the HAL initializes SysTick to fire at 1000 Hz. If the custom `SysTick_Handler` uses a busy-wait delay (like `delay_ms(100)`) inside the ISR *before* the SysTick timer is reconfigured to a lower frequency, the handler will take 100ms to complete and will immediately re-trigger, completely starving the main thread.
* **Resolution**: Keep a `volatile bool g_blink_enabled = false` status flag. Guard the ISR with this flag, and only set it to `true` after reconfiguring the timer to a safe frequency.

#### 14.4.5 SysTick-Dependent Delay Lockup inside High-Priority ISRs
A common pitfall in embedded systems is calling a platform delay helper (like `delay_ms(100)`) from inside a hardware timer interrupt handler (such as a `TIM2` ISR).
* **The Failure**: Typical platform delay loops (e.g., in `board.c`) spin on a global millisecond counter (`s_millis`) that is updated by the **SysTick exception handler**:
  ```c
  void delay_ms(uint32_t ms) {
      uint32_t start = s_millis;
      while ((s_millis - start) < ms) { /* spin */ }
  }
  ```
  Since the processor is in Handler Mode executing the high-priority `TIM2_IRQHandler`, the `SysTick` exception (which usually runs at equal or lower priority) is blocked from executing. As a result, `s_millis` never increments, and the loop spins forever, locking up the CPU with the LED permanently on.
* **The Solution**: Avoid calling any timer-tick-dependent delay functions inside an ISR context. Instead, implement a freestanding instruction-based busy loop (`local_delay_ms`) that burns CPU cycles directly without relying on peripheral interrupts:
  ```cpp
  inline void local_delay_ms(std::uint32_t ms) {
      // 16 MHz HSI clock: 5333 loop cycles per millisecond
      std::uint32_t count = 5333 * ms;
      __asm__ volatile (
          "1:\n"
          "subs %0, %0, #1\n"
          "bne 1b\n"
          : "+r"(count)
          :
          : "cc"
      );
  }
  ```

---

### 14.5 Summary of Architectural Design Choices

| Metric | Main Firmware Trampoline | Relocatable Objects Vector Patch | Assembly Module Wrapper | TIM2 Hardware Timer (Host-Debug) |
| :--- | :--- | :--- | :--- | :--- |
| **Interrupt Latency** | Low (Minimal trampoline overhead) | Zero (Direct hardware branch) | Low (Wrapper call overhead) | Low (ISR entrance overhead) |
| **GOT Base (`r10`)** | Handled by Firmware Trampoline | Not Used (Absolute symbols only) | Handled by Module Assembly | Handled by ISR wrapper if PIC |
| **Debugger Stability** | Frozen during BKPT halts | Frozen during BKPT halts | Frozen during BKPT halts | **Active during BKPT halts** |
| **C/C++ Code Eligibility** | Yes (Standard PIC function) | Yes (Standard absolute function) | Yes (Wrapped via ASM entry) | Yes (Direct handler routing) |
| **Implementation Risk** | Low (Centralized in main firmware) | Medium (Loader maps symbols) | High (Custom assembly needed) | Low (Standard timer module) |

*End of Full-Manual-SCL.md — covers all 6 SCL libraries, ~45 source files, 200+ functions/variables/structs, 9 execution paths, complete build/test/run pipeline, all 30 CMakeLists.txt files with options, and advanced interrupt/context-switching patterns.*


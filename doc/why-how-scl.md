# Why & How SCL Resolves External Symbols

A detailed walkthrough of the SCL's symbol resolution pipeline: from ELF parsing
to GOT patching, in the order each step happens at runtime.

---

## 1. The Problem

A shared library (`.so`) is compiled with `-fPIC` — all code/data addresses are
PC-relative or go through a **GOT** (Global Offset Table). When the library is
loaded at a runtime-chosen address, every external symbol reference (functions
in main, variables in other libraries) is still zero. The loader must:

1. Copy the library to RAM
2. Find where each external symbol lives (in main, in other libraries)
3. Patch the GOT entries so the library's code can reach those symbols

---

## 2. Step 1 — Parse the ELF (`common-elf-reader`)

```
elf_image::parse(byte_source& src) → result<elf_image>
```

**What:** Validates magic/class/data/machine, reads the ELF header.

**Why:** Every subsequent step needs the program headers (to find PT_LOAD
segments) and section headers (to find `.dynsym`, `.dynstr`, `.hash`,
`.rel.dyn`). The parser is the foundation.

**Key API:**
```cpp
img.program_header(i, ph)    // read PT_LOAD, PT_DYNAMIC
img.section_header(i, sh)    // read .dynsym, .dynstr, .hash, .rel.dyn
img.find_section(".dynsym")  // lookup by name
img.image_size()             // highest PT_LOAD end (buffer size)
img.image_base()             // lowest PT_LOAD vaddr (load bias)
```

**Why `byte_source`:** The parser never assumes the whole file is resident.
On target it reads via semihosting; in host tests via an in-memory span.
This is the I/O seam.

---

## 3. Step 2 — Load PT_LOAD Segments

```
for each Phdr where p_type == PT_LOAD:
    copy p_filesz bytes from file offset p_offset → RAM + p_vaddr
    zero p_memsz - p_filesz bytes (BSS)
```

**What:** The library's code and data are copied to RAM at the chosen base
address. The GOT, `.data`, `.bss` are now writable.

**Why:** The library's code expects to run from address `p_vaddr`. After
copying, `image[0]` corresponds to vaddr 0 (image-relative), and the actual
RAM address is the base.

**How each loader does it:**

| Loader | Allocation | Copy |
|--------|-----------|------|
| static (shlib-loader) | Caller-provided buffer | `load_segments()` loop |
| dynamic-nolib (dl.cpp) | `image_arena().alloc()` | memcpy per PT_LOAD |
| dynamic-newlib | `std::make_unique` | `image_memory::load_segments()` |
| XIP | Code stays in flash | Only RW segment copied to RAM |

---

## 4. Step 3 — Find the Symbol Tables

The loader needs three tables to resolve symbols:

| Table | ELF Section | Contents |
|-------|------------|----------|
| `.dynsym` | SHT_DYNSYM | Symbol entries: name index, value, size, info |
| `.dynstr` | SHT_STRTAB | NUL-terminated symbol name strings |
| `.hash` | SHT_HASH | SysV hash table for O(1) lookup |

### 4.1 How to find them

**Dynamic-nolib** (`dl.cpp`): `link_context::from_self()` parses the firmware's
own `_DYNAMIC` segment at boot (the `-pie -Wl,-E` model). The loaded library's
sections (`.dynsym`, `.dynstr`, `.hash`) are found via the section cache after
PT_LOAD copy, as in-image spans.

**Dynamic-newlib** (`link-context.cpp`): Reads the firmware's `_reloc` table
(a linker-emitted table of address/size pairs in the `.inits` section) at boot.
This is the non-PIC model — no `_DYNAMIC` segment needed. Symbol lookups use
the same SysV-hash `symbol_resolver` as everything else. Uses a weak
`scl_lookup_firmware_symbol` hook for fallback symbol resolution.

**Static loader** (`elf-reader.cpp`): Parses the library's ELF headers directly
via the shared `loader::elf_image` parser. The library's `.dynsym`, `.dynstr`,
`.hash` sections are found by name from the section header table.

```cpp
extern Elf::Dyn _DYNAMIC[];  // linker provides this

auto info = dynamic_info::parse({(byte*)_DYNAMIC, 64*sizeof(Dyn)});
// info.hash   = address of .hash
// info.symtab = address of .dynsym
// info.strtab = address of .dynstr
// info.strsz  = size of .dynstr
```

**Why `_DYNAMIC`:** The linker puts `DT_HASH`, `DT_SYMTAB`, `DT_STRTAB`,
`DT_STRSZ` entries into the `.dynamic` section. The loader reads these at
runtime to find the tables without parsing the ELF file.

---

## 5. Step 4 — Resolve a Symbol Name → Address

```
symbol_resolver::address_of("printf") → result<uint32_t>
```

**What:** Given a symbol name, returns its image-relative address (st_value
from `.dynsym`).

**How — SysV Hash Table Lookup:**

```
nbucket = hash[0]           // number of buckets
nchain  = hash[1]           // number of chain entries
bucket  = hash[2..2+nbucket-1]
chain   = hash[2+nbucket..2+nbucket+nchain-1]

index = elf_hash(name) % nbucket
y = bucket[index]

while y != 0:
    if name == dynstr + dynsym[y].st_name:
        return dynsym[y].st_value    // found!
    y = chain[y]                      // follow chain

return SYMBOL_NOT_FOUND
```

**Why a hash table:** Without it, every symbol lookup would be O(n) — scan
all symbols. The hash table makes it O(1) amortized (O(chain_length) worst
case, but chains are short in practice).

**`elf_hash()` — the SysV hash function:**
```cpp
uint32_t elf_hash(string_view name) {
    uint32_t h = 0;
    for (char c : name) {
        h = (h << 4) + c;
        uint32_t g = h & 0xF0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
```

---

## 6. Step 5 — Apply Relocations (`.rel.dyn`)

```
reloc_applier::apply_rel(image, base, reldyn, dynsym, dynstr, resolve)
```

**What:** Streams the `.rel.dyn` section (an array of `Elf32_Rel` entries) and
patches the loaded image in-place.

**For each relocation entry:**

```
r_offset = where in the image to patch
r_info   = (sym_index << 8) | reloc_type
```

### 6.1 The Six Relocation Types

| Type | Name | What it does | When |
|------|------|-------------|------|
| 2 | `R_ARM_ABS32` | `*slot += sym_target()` | Absolute pointer (e.g. `.init_array` entry) |
| 3 | `R_ARM_REL32` | `*slot += sym_target - (base + slot)` | PC-relative 32-bit data reference |
| 20 | `R_ARM_COPY` | `memcpy(slot, sym_target, size)` | Extern variable defined in main |
| 21 | `R_ARM_GLOB_DAT` | `*slot = sym_target()` | Global variable GOT slot |
| 22 | `R_ARM_JUMP_SLOT` | `*slot = sym_target()` | Function GOT/PLT slot |
| 23 | `R_ARM_RELATIVE` | `*slot += base` | Base-relative local data (no symbol) |

### 6.2 How `sym_target()` is computed

```cpp
auto sym_target = [&]() -> uint32_t {
    if (st_value != 0) return base + st_value;  // self-defined symbol
    auto r = resolve(name_at(dynstr, st_name));  // external: look up
    return r.has_value() ? *r : 0u;
};
```

**Key insight:** If `st_value != 0`, the symbol is defined in this library
(self-referencing). The runtime address is `base + st_value` (load bias +
link-time value). If `st_value == 0`, the symbol is external — the loader
must look it up via the `symbol_source` callback.

### 6.3 The `symbol_source` callback

```cpp
struct symbol_source {
    result<uint32_t> (*fn)(void* ctx, string_view name);
    void* ctx;
};
```

This is the **resolution chain** — each loader provides a different callback:

**Static loader:** Looks up in the global symbol table, then in main's `.dynsym`.

**Dynamic-nolib:** A `combined_resolver` that searches:
1. The current library's own `.dynsym`
2. All previously loaded libraries (g_handles[])
3. The main firmware's `.dynsym` (via `link_context`)

**Dynamic-newlib:** The `dynamic_linker`'s `resolve()` method searches its
internal map of loaded libraries.

**XIP:** Same as dynamic-nolib but with the RW segment address adjustment.

---

## 7. Step 6 — GOT/PLT Mechanism

### 7.1 What is the GOT?

The **Global Offset Table** is an array of 32-bit slots in the library's
`.got` / `.got.plt` sections. Each slot holds the runtime address of an
external symbol.

```
.got:      [global_var_1] [global_var_2] ...     ← GLOB_DAT relocations
.got.plt:  [reserved] [func_1] [func_2] ...      ← JUMP_SLOT relocations
```

### 7.2 How code uses the GOT

Without GOT (absolute address — breaks at runtime):
```asm
ldr r0, =printf     ; absolute address, wrong if loaded at different base
blx r0
```

With GOT (PC-relative → GOT slot → actual address):
```asm
ldr r0, [pc, #offset_to_got_slot]  ; load address from GOT
blx r0                              ; call through GOT
```

The GOT slot is patched by the loader at load time with the actual address.

### 7.3 PLT (Procedure Linkage Table)

For lazy binding, the `.got.plt` slots initially point to PLT stubs:

```asm
; PLT[0] — resolver trampoline
plt0: ldr r12, [pc, #got_plt_0]    ; r12 = &_GLOBAL_OFFSET_TABLE_
      ldr pc,  [r12, #8]!          ; jump to resolver

; PLT[n] — function stub
pltN: ldr r12, [pc, #got_slot_n]   ; r12 = GOT[n] (initially → plt0)
      bx  r12                      ; first call → resolver, later → target
```

After lazy resolution, GOT[n] is patched to point directly to the target
function, so subsequent calls skip the PLT stub.

---

## 8. Step 7 — Lazy Binding (JUMP_SLOT)

```cpp
lazy_resolve_slot(got_offset, lazy_tables, resolve) → result<uint32_t>
```

**What:** When a function is called for the first time, the PLT stub jumps
to the resolver. The resolver calls `lazy_resolve_slot()` which:

1. Scans `.rel.dyn` (or `.rel.plt`) for the JUMP_SLOT entry matching `got_offset`
2. Extracts the symbol index from `r_info`
3. Looks up the symbol name from `.dynstr`
4. Resolves it via the `symbol_source` callback
5. Returns the target address (with bit 0 set for Thumb)

**Why lazy:** Not all functions are called. Lazy binding defers resolution
until first use, speeding up load time.

**Why bit 0 set (`| 1u`):** ARM Thumb mode requires bit 0 of the target
address to be 1. The `| 1u` ensures the `BX` instruction switches to Thumb.

---

## 9. How Each Loader Orchestrates This

### 9.1 Static Loader (`scl::static`)

```
1. open_elf(path)                    → fd_byte_source + elf_image::parse
2. load_segments(buffer)             → copy PT_LOAD to RAM
3. populate_helper_sections()        → find .rel.dyn, .dynsym, .dynstr, .hash
4. find_shlib_symbols_to_patch()     → build patch array (reloc_entry[])
5. patch_shlib(global_syms)          → for each entry:
     R_ARM_RELATIVE: *slot += base
     R_ARM_GLOB_DAT/JUMP_SLOT: resolve(name) → *slot = address
     R_ARM_ABS32: *slot = address
     R_ARM_COPY: memcpy from main
6. execute_constructors()            → call .init_array functions
```

**Resolution chain:** `global_syms->find(name)` → `efr_.find_symbol_address(name, m_main)`

### 9.2 Dynamic-nolib Loader (`scl::dynamic-nolib`)

```
1. dlopen_source(src, name)
2.   elf_image::parse(src)           → validate
3.   image_arena().alloc(size)       → allocate in .image_arena
4.   copy PT_LOAD segments
5.   g_sc.load(img, src)             → cache section headers
6.   sec_in_image(".dynsym/.dynstr/.hash") → spans into loaded image
7.   link_context::from_self()       → main's own symbol tables
8.   build combined_resolver         → self + all loaded handles + main
9.   for .rel.dyn and .rel.plt:
       reloc_applier::apply_rel(image, base, rel, dynsym, dynstr, resolve)
10.  set up lazy JUMP_SLOT trampolines
11.  return handle
```

**Resolution chain (combined_resolver):**
```
search order:
  1. current library's own .dynsym (st_value != 0 → base + st_value)
  2. g_handles[dep].resolver (previously loaded libraries)
  3. main's link_context (from _DYNAMIC)
```

### 9.3 Dynamic-newlib Loader (`scl::dynamic-newlib`)

```
1. dynamic_linker::load_from_source(src)
2.   elf_image::parse(src)
3.   image_memory::load_segments()    → heap-allocated buffer
4.   collect_relocations()            → concatenate all SHT_REL sections
5.   find .dynsym, .dynstr, .hash via section headers
6.   relocator::compute(reldyn, dynsym, dynstr) → relocation_plan
7.   resolve each entry → apply
8.   link_context::from_self() for main's symbols
```

**Key difference:** Uses `std::vector` for the relocation plan (hosted only).

### 9.4 XIP Loader (`scl::xip-*`)

```
1. classify_segments(image)          → find R-X and RW segments
2. R-X segment stays in flash (text_base)
3. RW segment copied to RAM (data_base)
4. relocate(image, base, reldyn, dynsym, dynstr, resolve)
   → patches RW segment only (GOT, .data)
5. Code in flash reaches GOT via r10 register (PIC base)
```

**Key difference:** Two-base model. The code's addresses are flash-based;
the GOT's addresses are RAM-based. `r10` points to the GOT base.

### 9.5 Build-Time Symbol Table Generation (`tools/gen-symtab.cpp`)

The XIP dynamic-newlib loader (`xip::newlib`) needs a **symbol table embedded
in the firmware's flash** so it can resolve library imports at runtime without
semihosting or a debug connection. This is the job of `tools/gen-symtab.cpp` —
a C++20 host tool compiled at build time.

#### Why it is needed

When `xip::newlib::dlopen()` loads a `.so` library, every external symbol
(`printf`, `malloc`, `sinf`, ...) must be resolved against the firmware's own
exports. On standard dynamic-nolib, this happens via `link_context::from_self()`
which parses the firmware's `_DYNAMIC` segment. On dynamic-newlib, it uses the
`_reloc` table. But in XIP mode with flash-resident code, the firmware may not
have a `_DYNAMIC` segment or `_reloc` table — it needs a **pre-built lookup
table** embedded directly in the firmware binary.

The symbol table is stored in a `.scl_symtab` section in the firmware ELF. At
runtime, `xip::newlib::set_symtab()` parses this blob to populate the
`link_context`'s symbol table, enabling the loader to resolve imports.

#### How `tools/build.sh` compiles the host tool

```bash
#!/usr/bin/env bash
# tools/build.sh — Compile the C++20 host symbol table generator
g++ -std=c++20 gen-symtab.cpp ../common-elf-reader/src/elf-image.cpp \
    -I../common-elf-reader/include \
    -o gen-symtab
```

The tool links against the **same `common-elf-reader` library** used by the SCL
itself — it reuses `loader::elf_image`, `loader::fd_byte_source`, and the ELF
type definitions. This ensures the host tool parses ELF files exactly the same
way the on-target loader does.

The CMake build system compiles this automatically via a `gen-symtab-host`
custom target (see `CMakeLists.txt` lines 218–235):

```cmake
set(SCL_HOST_GEN_SYMTAB "${CMAKE_BINARY_DIR}/tools/gen-symtab" CACHE INTERNAL "")
add_custom_command(
    OUTPUT "${SCL_HOST_GEN_SYMTAB}"
    COMMAND c++ -std=c++20
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-symtab.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/src/elf-image.cpp"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/include"
            -o "${SCL_HOST_GEN_SYMTAB}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-symtab.cpp"
    COMMENT "Building C++20 gen-symtab host tool"
)
add_custom_target(gen-symtab-host DEPENDS "${SCL_HOST_GEN_SYMTAB}")
```

#### How `gen-symtab.cpp` works

The tool runs as a **POST_BUILD step** after the firmware ELF is linked. It
takes two arguments: `<firmware.elf>` and `<out.bin>`.

**Pipeline:**

```
1. Open firmware ELF via fd_byte_source + elf_image::parse()
2. Find three sections: .scl_symtab (placeholder), .symtab, .strtab
3. Read the symbol table (.symtab) and string table (.strtab)
4. Filter symbols:
   - Keep only Global (bind=1) and Weak (bind=2) symbols
   - Skip symbols with st_value == 0 (undefined) or st_shndx == 0 (SHN_UNDEF)
   - Skip symbols starting with '_' UNLESS whitelisted:
     _impure_ptr, _Znw (new), _Zdl (delete), __cxa (exceptions),
     __errno, __exidx, __gnu_Unwind, __register/deregister_frame_info
5. Build binary blob:
   - Header: magic 0x534C4353 ('SCLS') + symbol count
   - Table: (name_addr, value)[] pairs — 8 bytes per symbol
   - Names: NUL-terminated symbol name strings
6. Pad blob to .scl_symtab section size (fills unused space with zeros)
7. Write binary output
```

**Symbol pruning is critical:** Linking `newlib` and `libstdc++` pulls in
thousands of internal symbols. Without filtering, the table would be ~87 KB —
too large for the STM32F411's flash. The whitelist approach reduces it to:
- **~7.5 KB** (299 symbols) for XIP newlib
- **~6.6 KB** (264 symbols) for non-XIP newlib

#### How it integrates into the test build

In the test CMakeLists (`test-xip/test-xip-dynamic-newlib/test1/CMakeLists.txt`):

```cmake
# 1. Declare dependency on the host tool
add_dependencies(scl-fw-xip-newlib1 gen-symtab-host)

# 2. POST_BUILD: run gen-symtab on the linked firmware ELF
add_custom_command(TARGET scl-fw-xip-newlib1 POST_BUILD
    COMMAND "${SCL_HOST_GEN_SYMTAB}"
            $<TARGET_FILE:scl-fw-xip-newlib1>
            $<TARGET_FILE:scl-fw-xip-newlib1>.symtab.bin
    COMMAND ${CMAKE_OBJCOPY} --update-section
            .scl_symtab=$<TARGET_FILE:scl-fw-xip-newlib1>.symtab.bin
            $<TARGET_FILE:scl-fw-xip-newlib1>
    COMMENT "Embedding loader symbol table into scl-fw-xip-newlib1"
)
```

**Step by step:**
1. `gen-symtab` reads the **linked** firmware ELF (which now has final symbol
   addresses) and produces `symtab.bin`
2. `objcopy --update-section .scl_symtab=...symtab.bin` replaces the placeholder
   section in the ELF with the real symbol table
3. A subsequent `objcopy -O ihex` produces the `.hex` file for flashing

The linker script (`sections-flatfs-hosted.ld`) reserves space for the table:

```ld
.scl_symtab : ALIGN(4) {
    KEEP(*(.scl_symtab))
} >FLASH
```

#### Which tests use it

| Test | Uses gen-symtab? | Why |
|------|-----------------|-----|
| `scl-fw-xip-newlib1` | **Yes** | XIP dynamic-newlib needs flash-resident symbol table |
| `scl-fw-dynamic-newlib1/2` | No | Uses `_reloc` table model instead |
| `scl-fw-xip-nolib1` | No | Uses `link_context::from_self()` via `_DYNAMIC` |
| `scl-fw-xip-static-*` | No | Uses `bind_host_imports()` provision-time prelink |
| `scl-fw-dyn1/2` | No | Uses `link_context::from_self()` via `_DYNAMIC` |

#### Runtime flow

```
Build time:
  Link firmware → gen-symtab parses ELF → symtab.bin → objcopy embeds in .scl_symtab

Boot time:
  Firmware starts → xip::newlib::set_symtab() reads .scl_symtab blob
  → parses (magic, count, (name_addr, value)[] entries)
  → populates link_context's symbol table

Load time:
  xip::newlib::dlopen("c1.so") → parse ELF → classify segments
  → resolve "printf" via symbol table → patch GOT → run constructors
```

---

## 10. The `r10` Register — Single PIC Base

In XIP mode, the library's code is in flash (read-only) and the GOT is in
RAM (writable). Code can't use PC-relative GOT access (that would compute
a flash address, but the GOT is in RAM).

Instead, `r10` holds the GOT base address:

```asm
; Function call through GOT (r10-based)
ldr  r12, [r10, #got_offset]   ; load from GOT via r10
blx  r12
```

The `pic-trampoline.c` sets r10 before entering a library:

```c
void pic_call_1(uint32_t got_base, uint32_t fn, uint32_t arg0) {
    register uint32_t r10 __asm__("r10") = got_base;
    ((void(*)(uint32_t))fn)(arg0);
}
```

**Why `-msingle-pic-base -mpic-register=r10`:** These GCC flags tell the
compiler to use r10 as the PIC register instead of computing GOT addresses
relative to PC.

---

## 11. Summary — The Full Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│  1. PARSE ELF                                               │
│     elf_image::parse() → validate magic/class/machine       │
│     → Ehdr, Phdr[], Shdr[]                                  │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  2. LOAD SEGMENTS                                           │
│     copy PT_LOAD filesz bytes → RAM + vaddr                 │
│     zero BSS (memsz - filesz)                               │
│     base = RAM address where image[0] lives                 │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  3. FIND SYMBOL TABLES                                      │
│     .dynsym  — symbol entries (name, value, size, info)     │
│     .dynstr  — NUL-terminated name strings                  │
│     .hash    — SysV hash: [nbucket, nchain, buckets, chain] │
│     .rel.dyn — relocation entries (offset, info)            │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  4. BUILD RESOLUTION CHAIN                                  │
│     self (own .dynsym) → siblings → main (via _DYNAMIC)     │
│     symbol_source { fn(ctx, name) → address }               │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  5. APPLY RELOCATIONS                                       │
│     for each Elf32_Rel in .rel.dyn:                         │
│       RELATIVE  : *slot += base                             │
│       GLOB_DAT  : *slot = resolve(name)                     │
│       JUMP_SLOT : *slot = resolve(name)  (or lazy trampoline)│
│       ABS32     : *slot += resolve(name)                    │
│       REL32     : *slot += resolve(name) - (base + slot)    │
│       COPY      : memcpy(slot, resolve(name), size)         │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  6. READY                                                   │
│     All GOT entries patched.                                │
│     Code can call external functions via GOT.               │
│     Run .init_array constructors.                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 12. Why This Design

### Why `byte_source` interface?

The parser never touches files or memory directly. On target, `fd_byte_source`
uses semihosting. In host tests, `memory_byte_source` uses a span. Same
parser, same code, different I/O.

### Why `symbol_source` callback?

Each loader has a different resolution chain. The static loader searches a
global table + main's `.dynsym`. The dynamic loader searches all loaded
handles. The callback abstracts this — `reloc_applier` doesn't know or care
how symbols are found.

### Why `result<T>` everywhere?

Bare-metal has no exceptions. Every fallible operation returns `result<T>`
(success value or error code). The caller checks `has_value()` and either
uses the value or propagates the error. No hidden control flow.

### Why hash table, not linear scan?

A `.so` with 100 symbols and 20 relocations would need 2000 string comparisons
with linear scan. With the hash table, it's ~20 hash computations + ~20 chain
walks. On a Cortex-M4 at 100 MHz, this saves milliseconds per library load.

### Why lazy binding?

Not all functions are called. A library might import `printf` but only use it
in an error path. Lazy binding defers the resolution cost to first use,
speeding up `dlopen`.

### Why r10 for XIP?

The code is in flash (read-only). PC-relative GOT access would compute a
flash address, but the GOT is in RAM. `r10` holds the GOT base, so code
can do `[r10, #offset]` to reach any GOT slot regardless of where the
code is executing from.

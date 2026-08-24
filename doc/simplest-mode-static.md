# Simplest Mode: Loading Shared Libraries with `scl::static`

The zero-friction way to load a shared library on a Cortex-M (ARM32) target: the
**main application does nothing special in code — it only changes build options —
and calls library functions as if they were ordinary linked functions.**

---

## 1. The design constraint

You want a shared-library loading mechanism where:

1. **`main` does nothing special** — no trampoline, no GOT-base register, no
   hand-written symbol table, no `dlopen`/`dlsym` plumbing.
2. **Only build options differ** — the *fixture* (the `.so`) is built one way,
   the *main firmware* another; the source of `main` is a normal program.
3. **Calls are plain** — `library_function(7)`, exactly like a statically linked
   call, with no preparation before the call.

Inside the SCL family this is exactly what **`scl::static`** provides, in its
**direct / pre-linked mode** (`test-static/test-static-direct`). Every other
loader forces *some* preparation into the application code.

---

## 2. The answer in one look

```c
#include "mylib1.h"                 // ordinary header for the library

// ... fixed RAM buffers the loader will place the .so into ...
alignas(8) static uint8_t ram_sh1[32768];

scl::Module mod1;

// THE ONLY runtime step: place the .so bytes and patch the GOT/PLT.
mod1.load_file("libsh1/libsh1.so", ram_sh1, sizeof ram_sh1, &g_global_syms);

// ... afterwards, every call is a plain, ordinary call ...
z   = library_function(7);          // no dlsym, no got_base, no trampoline
*(uint32_t*)aa = 20;                // even imported *data* works directly
```

Compare that with what the other loaders ask of `main`:

| Loader | How you call a library function | Extra work forced into `main` |
|---|---|---|
| **`scl::static` (direct)** | `library_function(7)` | **none** (build flags only) |
| `scl::dynamic-newlib` | `fn(42)` after `dl::dlsym(h,"name")` | dlopen/dlsym facade; hosted runtime, `_reloc` + `-u` anchors + embedded symtab |
| `scl::reloc` | `fn(42)` after `reloc::sym(set,"name")` | must **hand-write** a `main_resolve()` symbol table (`test-relocatable/src/main.cpp:113`) |
| `scl::dynamic-nolib` | `pic_call_u32(fn, got_base, arg)` | carry `got_base`, r10 trampolines |
| `scl::overlay` | plain `long_call` | modules frozen into the image, no runtime choice |
| `scl::xip-*` | `pic_call_*` (r10) | same r10/GOT handling as nolib |

---

## 3. The idea in three sentences

The library is compiled **position-independent** (`-fPIC -shared`) so its
code and data can run at **any** address; the firmware **links against it at
build time** (`-lsh1 -lsh2`) so the compiler emits ordinary PLT/GOT call
sequences; at **run time** the loader copies the `.so` into RAM and **patches
the GOT/PLT** so those ordinary calls land in the loaded code. Nothing in
`main` knows any of this happened.

> The "static" in `scl::static` does **not** mean the library is statically
> linked into the firmware — the `.so` is still loaded from a file at run time.
> "Static" refers to the fact that **name resolution happens once at load time
> and is then patched in place**: there is no per-call dynamic lookup.

---

## 4. Where it lives

```
include/static/
├── module.h             # scl::Module — the high-level "load + call" wrapper
├── shlib-loader.h       # elf_loader::shlib_loader — segment load + GOT patching
├── elf-reader.h         # elf_loader::elf_reader — thin adapter over common-elf-reader
└── logger.h             # elf_loader::log — centralised diagnostic output

src/static/
├── module.cpp           # load_file/load_buf/sym pipeline
├── shlib-loader.cpp     # load_bin / patch_shlib / patch_main / execute_constructors
├── elf-reader.cpp       # ELF parse + relocation/symbol enumeration
└── logger.cpp           # log implementation

common-elf-reader/       # the shared ELF32 parser (loader::elf_image, byte_source)
test-static/test-static-direct/   # the reference app (WeAct/NUCLEO F411)
test-static/test-static/          # lower-level didactic app (same technique)
test-static/test-static-cpp/      # C++ fixture variant
```

`CMakeLists.txt` exposes it as an **INTERFACE source library** (no `.a` —
sources are compiled into the consumer with the consumer's own flags):

```cmake
add_library(scl-static INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-static INTERFACE
    src/static/elf-reader.cpp
    src/static/shlib-loader.cpp
    src/static/logger.cpp
    src/static/module.cpp)
target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)
add_library(scl::static ALIAS scl-static)
```

Note: `scl::static` links **`common-elf-reader::elf-reader` directly**, not
`scl::common` — it is a self-contained patcher.

---

## 5. The two halves of the build contract

"Nothing special in code" works because *all* the specialness is moved into the
build. There are exactly two things to get right, and both are flags + linker
scripts, not code.

### 5.1 The fixture `.so` side — position-independent, base address 0

From `test-static/test-static-direct/CMakeLists.txt:50`:

```cmake
set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra -DSTM32F411RxE)
```

and the `.so` link (same file, `add_shlib`):

```bash
arm-none-eabi-gcc ${LIB_CFLAGS} mylib1.o -o libsh1.so \
    -shared -nostartfiles \
    -T gcc.ld \
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments
```

Key points:

- **`-fPIC`** — every global access goes through a GOT; every external call goes
  through a PLT. This is what lets the same bytes run at any address. (`-fpic`
  is the *other*, r10-based model used by `scl::dynamic-nolib` — **not** this one.)
- **`-fno-inline-functions`** — keeps the exported functions as real entry points
  with visible symbols.
- **`-fno-common`** — puts tentative definitions in `.bss` instead of `COMMON`,
  so they survive `--gc-sections` and land in the loaded image.
- **`-mfloat-abi=soft`** — the static patcher is soft-float. The direct app and
  its fixtures share this ABI (hard-float is the `dynamic-*`/XIP domain).
- **`-shared`** — emits `.dynsym` / `.dynstr` / `.hash` / `.rel.dyn` /
  `.rel.plt` / `.got` / `.got.plt` and marks `e_type = ET_DYN`.
- **`-nostartfiles`** — no CRT; the "startup" of a library is its `.init_array`.

The library link script (`test-static/test-static/libsh1/ldscripts/mem.ld`)
places the whole library at a **run address of 0**:

```
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35k }
```

with `sections2.ld` laying out `.inits`, `.plt`, `.got`, `.got.plt`, `.text`,
`.rodata`, `.data`, `.bss` all in that one region. Linking at vaddr 0 means the
final loaded address is simply `load_base + link_address` — which is why the
loader's job reduces to "add the buffer address" (`base + st_value`).

### 5.2 The main firmware side — non-PIC, linked against the `.so`

The firmware is a **normal non-PIC** program (soft float, no exceptions/RTTI)
that links the libraries at build time (`test-static/test-static-direct/CMakeLists.txt:172`):

```cmake
target_link_options(scl-fw-static-direct PRIVATE
    -T${ST}/ldscripts/mem-sh.ld
    -T${ST}/ldscripts/libs-sh.ld
    -T${ST}/ldscripts/sections-sh.ld
    -nostartfiles -Wl,--gc-sections --specs=nano.specs -u _printf_float
    -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
    -L${APP_DIR}/libsh1 -L${APP_DIR}/libsh2 -L${APP_DIR}/new-lib
    -lsh1 -lsh2 -lcpp_features -lstdc++)
```

What each piece does:

| Flag | Purpose |
|---|---|
| `-lsh1 -lsh2 -lcpp_features` | the link-time contract: the compiler emits ordinary calls through the PLT/GOT for these libraries' symbols |
| `-Wl,--no-dynamic-linker` | no `ld.so` on the target — the SCL is the "dynamic linker" |
| `-T mem-sh.ld libs-sh.ld sections-sh.ld` | custom scripts that reserve `.plt/.dynsym/.dynstr/.hash/.rel.dyn` in FLASH and `.got/.got.plt` in RAM, and **emit the `_reloc` block** |
| `--specs=nano.specs -u _printf_float` | newlib-nano, with `%f` support pulled in |
| `-Wl,--no-warn-rwx-segments` | the R-X/RW overlap of a bare-metal image is deliberate |

Because the firmware is non-PIC and links the library, calling
`library_function(7)` compiles to a *normal* `bl` through a PLT stub whose GOT
slot the loader will later fill — **the call site in `main` needs no knowledge
of loading at all.**

### 5.3 The `_reloc` block — the bridge the linker script builds

The loader must find the *firmware's* dynamic tables to resolve names the
library imports. Instead of parsing the firmware's ELF (there is no file on
target), the linker script emits a fixed struct into `.inits`
(`test-static/test-static/ldscripts/sections-sh.ld`):

```
.inits : ALIGN(4)
{
    _reloc = .;
    LONG(LOADADDR(.rel.dyn));  LONG(SIZEOF(.rel.dyn));
    LONG(LOADADDR(.dynsym));   LONG(SIZEOF(.dynsym));
    LONG(LOADADDR(.dynstr));   LONG(SIZEOF(.dynstr));
    LONG(LOADADDR(.hash));     LONG(SIZEOF(.hash));
    LONG(LOADADDR(.inits));    LONG(SIZEOF(.inits));
    ...
} >FLASH
```

The loader reads it as a flat struct (`src/static/shlib-loader.cpp:42`):

```c
struct init_lds {
    uint32_t reldyn;   uint32_t reldyn_size;
    uint32_t dynsym;   uint32_t dynsym_size;
    uint32_t dynstr;   uint32_t dynstr_size;
    uint32_t hash;     uint32_t hash_size;
    uint32_t inits;    uint32_t inits_size;
};
extern uint32_t _reloc;                       // the linker provides this symbol
```

---

## 6. The runtime pipeline, step by step

The whole load is one call: `scl::Module::load_file()`. Internally
(`src/static/module.cpp`):

```
Module::load_file(path, buf, bufsz, syms)
  └─ Module::build(efr, buf, bufsz, syms)
       ├─ efr.open_elf(path)                       # parse ELF over semihosting
       ├─ find ".dynsym" / ".dynstr" section idx    # must be a dynamic .so
       ├─ shlib_loader shl(std::move(efr))
       ├─ shl.load_bin(buf, bufsz)                 # (1) copy PT_LOAD segments
       ├─ shl.populate_helper_sections()           # (2) locate the 6 helper sections
       ├─ find_shlib_symbols_to_patch()            # (3) enumerate library fixups
       ├─ find_main_symbols_to_patch()             # (3) enumerate firmware fixups
       ├─ shl.patch_shlib(syms)                    # (4) patch the library's GOT
       ├─ shl.patch_main(syms)                     # (5) patch the firmware's GOT
       └─ shl.execute_constructors()               # (6) run .init_array
```

Each step:

1. **`load_bin`** (`shlib-loader.cpp:56`) — walks `PT_LOAD` program headers,
   copies `p_filesz` bytes into the RAM buffer at `p_vaddr`, zeroing BSS
   (`p_memsz - p_filesz`). `get_binary_size()` must fit the buffer or
   `INVALID_SEGMENT` is returned. Also `scan_dependencies()` collects
   `DT_NEEDED` sonames.

2. **`populate_helper_sections`** (`shlib-loader.cpp:106`) — resolves the six
   runtime section descriptors (`.inits`, `.rel.dyn`, `.rel.plt`, `.dynsym`,
   `.dynstr`, `.hash`) for **both** images: the library's come from its section
   table (offset + `buffer`), the firmware's from the `_reloc` block. If a
   library has no `.inits` it falls back to `.init_array`.

3. **`find_*_symbols_to_patch`** — streams `.rel.dyn` + `.rel.plt` and the
   dynamic symbol table into flat `symbol_patch` arrays
   (`{name, got_address, virtual_address, type, index, binding}`). The array
   sizes are queried first via `calculate_patch_array_size`.

4. **`patch_shlib`** (`shlib-loader.cpp:301`) — writes into the *library's*
   RAM image:
   - `R_ARM_RELATIVE` → `*slot += base` (self-referential offsets),
   - `R_ARM_ABS32` → `*slot = resolve(name)` (e.g. `.init_array` entries),
   - `STB_GLOBAL`/`STB_WEAK` (`GLOB_DAT`/`JUMP_SLOT`) → `*slot = resolve(name)`,
   - and **registers** every exported (self-defined) symbol into the
     `symbol_table` for cross-module interposition.
   Every write is bounds-checked against the image size.

5. **`patch_main`** (`shlib-loader.cpp:463`) — the *reverse* direction, patching
   the firmware's own GOT slots so `main`'s ordinary calls reach the loaded
   library. Handles `R_ARM_COPY` (see §11), `GLOBAL`, and `WEAK` slots.

6. **`execute_constructors`** (`shlib-loader.cpp:572`) — calls each pointer in
   the library's `.init_array`, then `scl_memory_barrier()` (DSB/ISB) so the
   newly written GOT/PLT words are visible to the instruction path.

After this, `Module::sym()` and `Module::operator[]` can also do *name-based*
calls by walking the loaded `.dynsym` (`module.cpp:71`):

```c
mod1["cppsh_make"](7);      // operator[] + call functor; plain lookup at call site
```

---

## 7. The API surface

### 7.1 `scl::Module` — the high-level wrapper (`include/static/module.h`)

```cpp
namespace scl {
enum class module_error { OK, FILE_NOT_FOUND, NOT_AN_ELF, NO_DYNSYM, LOAD_FAILED };
enum class scope      { GLOBAL, LOCAL };                       // RTLD_GLOBAL / RTLD_LOCAL
using dep_loader = bool (*)(const char* soname);               // DT_NEEDED auto-load hook

class Module {
public:
    bool load_file(const char* path, uint8_t* exec_buf, uint32_t exec_sz,
                   elf_loader::symbol_table* syms,
                   scope s = scope::GLOBAL, dep_loader load_dep = nullptr);
    bool load_buf(const uint8_t* raw, uint32_t rawsz,
                  uint8_t* exec_buf, uint32_t exec_sz,
                  elf_loader::symbol_table* syms,
                  scope s = scope::GLOBAL, dep_loader load_dep = nullptr);

    static uint32_t image_size(const char* path);             // buffer size needed (0 on error)

    void   retain();                                          // +1 reference
    void   release();                                         // -1; tear down at 0
    void   unload();                                          // == release(); idempotent
    uint32_t refcount() const;

    void* sym(const char* name) const;                        // name -> address (hash-accelerated)
    template<typename T> T sym(const char* name) const;       // typed cast helper
    Call operator[](const char* name);                        // callable functor

    module_error last_error() const;                          // coarse error
    int         last_detail() const;                          // elf_loader::loader_error as int
    const char* last_error_string() const;                    // "InvalidSegment", ...
    bool        loaded() const;                               // true once loaded
    const std::vector<std::string>& dependencies() const;     // DT_NEEDED sonames
    uint32_t    tls_offset() const;                           // module offset in the TLS area
};
}

// Weak reentrancy hooks (override in an RTOS app):
void scl_loader_lock();    void scl_loader_unlock();

// TLS (multi-thread area model) — see static/tls.h:
uint32_t scl_tls_register(const void* tdata, uint32_t tdata_sz, uint32_t tls_size);
void     scl_tls_unregister(uint32_t offset);
uint32_t scl_tls_area_size(void);
void     scl_tls_init_area(void* area);
void*    scl_tls_prepare(void);                 // single-thread convenience
void     scl_tls_set_pointer(void* p);          // per-thread area base
void*    scl_tls_get_pointer(void);
```

### 7.2 `elf_loader::elf_reader` — parse + enumerate (`include/static/elf-reader.h`)

Thin adapter over `loader::elf_image` from `common-elf-reader`:

```cpp
class elf_reader {
public:
    bool open_elf(std::string_view path);                     // fd_byte_source
    bool open_buf(const uint8_t* data, uint32_t size);        // memory_byte_source
    loader_error verify_elf();
    uint32_t get_binary_size();                               // max PT_LOAD end
    uint32_t find_section_by_name(std::string_view name);
    loader_error load_segments(std::span<uint8_t> ram, const Elf32_Phdr& ph);
    loader_error read_relocation_symbols(const pass_args&, span<symbol_patch>, uint32_t&);
    uint32_t calculate_patch_array_size(const pass_args&);
    uint32_t find_symbol_address(std::string_view, const section_map&);
    loader_error scan_dependencies(std::vector<std::string>&);
    // ...
};
```

### 7.3 `elf_loader::shlib_loader` — the patcher (`include/static/shlib-loader.h`)

```cpp
class shlib_loader {
public:
    loader_error load_bin(uint8_t* buffer, uint32_t bufsize);
    loader_error populate_helper_sections();
    loader_error find_shlib_symbols_to_patch(std::span<symbol_patch>);
    loader_error find_main_symbols_to_patch(std::span<symbol_patch>);
    uint32_t patch_shlib(symbol_table* global_syms);          // library GOT
    uint32_t patch_main(symbol_table* global_syms);           // firmware GOT
    loader_error execute_constructors();
    unsigned  resolve_weak_function(funcm f);
    loader_error resolve_dependencies(symbol_table*, load_dep_cb);
};
```

### 7.4 `elf_loader::symbol_table` — interposition registry

```cpp
class symbol_table {
public:
    void add_global(std::string_view name, uint32_t addr);
    void set_global(std::string_view name, uint32_t addr);   // overwrite
    void add_weak(std::string_view name, uint32_t addr);
    uint32_t find(std::string_view name) const;              // 0 if absent
    bool has(std::string_view name) const;
    void clear();
};
```

In the direct app the table starts **empty**: `static elf_loader::symbol_table
g_global_syms;`. Symbols are found automatically from the firmware's own
`.dynsym` via the `_reloc` block — the app never populates it by hand.

### 7.5 `elf_loader::log` — diagnostics (`include/static/logger.h`)

`log_info/warn/error/debug(fmt, ...)` macros capturing `__FILE__/__LINE__`,
plus `hex(v)` / `hex16(v)` (rotating buffer ring so several can share one
`printf`) and `binding_str()`.

---

## 8. Relocation types handled

`patch_shlib` + `patch_main` understand the subset of ARM relocations a
`-fPIC -shared` Cortex-M `.so` actually emits (constants from `<elf.h>`):

| Type | Value | Meaning | Applied to |
|---|---|---|---|
| `R_ARM_NONE` | 0 | registration-only entry (no GOT slot) | — |
| `R_ARM_ABS32` | 2 | absolute 32-bit pointer (`.init_array`, vtables) | library |
| `R_ARM_REL32` | 3 | PC-relative data reference | library |
| `R_ARM_COPY` | 20 | main's slot ← library symbol address (see §11) | firmware |
| `R_ARM_GLOB_DAT` | 21 | data GOT slot | both |
| `R_ARM_JUMP_SLOT` | 22 | function PLT/GOT slot | both |
| `R_ARM_RELATIVE` | 23 | `*slot += base` (self-relative) | library |

---

## 9. Symbol resolution chain

For a library import of `name`, resolution tries, in order:

1. **Self-defined** (`st_value != 0`) → `buffer + st_value`.
2. **`symbol_table`** (`global_syms`) → address registered by an earlier
   library (cross-module interposition, first-loaded-wins).
3. **The firmware's own `.dynsym`** (`find_symbol_address(name, m_main)`), read
   through the `_reloc` block.

Weak symbols follow the same order but are legal to resolve to `0` (undefined
weak). Exported symbols are *published* into the `symbol_table` during
`patch_shlib`, which is how later-loaded libraries reach earlier ones.

---

## 10. Why this is "the simplest" — the comparison

The table in §2 is the whole argument, restated with *why* each cost exists:

- **`dynamic-nolib`** compiles libraries with `-fpic -msingle-pic-base
  -mpic-register=r10`. The library reaches its globals *through the r10
  register*, so every entry into a library must set r10 first — hence
  `pic_call_u32(fn, got_base, arg)`. That `got_base` you must carry around is
  precisely the "preparation" this document wants to avoid.
- **`reloc`** has no GOT at all (ET_REL, `-mlong-calls -fno-pic -r`), so it
  needs an explicit name→address table on the firmware side to resolve imports —
  the ~30-line `if (n == "trace_event") ...` chain in `main_resolve()`.
- **`overlay`** resolves everything at link time to a fixed window, so there is
  nothing to prepare — but nothing to choose at run time either.
- **`dynamic-newlib`** needs no trampoline (plain `fn(42)`), but still needs a
  hosted runtime (newlib + heap + `std::vector`), a `_reloc` table, `-u`
  keep-alive anchors, and an embedded symbol table — more moving parts than
  `scl::static`, for the same "plain call" ergonomics.

`scL::static` is the only one where the call site is literally
`library_function(7)` **and** the runtime contract is a single `load_file()`.

---

## 11. The one thing you cannot avoid

Exactly one runtime statement is unavoidable, because the `.so` bytes have to
physically land in memory somewhere:

```c
mod1.load_file("libsh1/libsh1.so", ram_sh1, sizeof ram_sh1, &g_global_syms);
```

That is the *entire* "preparation" in `main`. Everything else — the headers, the
calls, the data accesses — is ordinary C. There is no way to reach zero runtime
statements: even a hypothetical "magic" loader must be told which file to load
and where to put it.

You *can* reduce it to a one-liner per library and tuck the buffer sizes behind
`constexpr`, which is exactly what `test-static-direct` does with three
`alignas(8) static uint8_t ram_*[32768]` buffers and three `Module` objects.

---

## 12. What you give up (costs of this mode)

| Cost | Detail |
|---|---|
| **Build-time linking** | the *which library* decision is made by `-lsh1 …`, not by a free-form runtime path. (The `.so` is still read from a file at run time — only the *symbol binding* is pre-linked.) |
| **Soft-float ABI** | fixtures and firmware share `-mfloat-abi=soft`. Hard-float belongs to the `dynamic-*`/XIP loaders. |
| **No lazy binding** | every GOT slot is patched eagerly at load; there is no on-first-call PLT resolver. |
| **Manual unload** | `Module::unload()` is byte-exact (journal revert + `.fini_array`) and reference-counted (`retain`/`release`), but still explicit: nothing unloads automatically at scope exit (the destructor tears down as a safety net). |
| **RAM buffers are manual** | you reserve the `ram_*[]` buffers yourself; `dynamic-newlib` uses the heap, `dynamic-nolib` an arena. |
| **Exceptions/RTTI off** | the direct reference app compiles with `-fno-exceptions -fno-rtti` (C++ fixture still works for plain classes). |

---

## 12b. Does it work for `scl::xip-static` too?

**Partially.** There *is* an XIP-static "direct" variant (`test-xip/test-xip-static-direct`),
and it reuses the same build-time-linking idea, but it does **not** reach the
zero-ceremony call site described in §2. Execute-in-place forces two things back
in.

### 12b.1 What is different about XIP

XIP keeps the **R-X segment in flash** (executed in place) and copies only the
**RW segment** (GOT/`.data`/`.bss`) to RAM — the "two-base model" documented in
`include/xip/static/adapter.hpp`. That single fact changes everything at the
call site:

- `scl::static` (non-XIP) loads the whole image into one RAM buffer and compiles
  fixtures with **`-fPIC`** (PC-relative GOT). The library finds its data via
  the program counter, so a plain `bl library_function` works with **no register
  setup**.
- `scl::xip-static` compiles fixtures with **`-fpic -msingle-pic-base
  -mpic-register=r10`** (`test/fixtures-src/c1/c1.h:7`). The library finds its
  data through the **r10 register**, because flash-resident code cannot reach the
  RAM GOT PC-relatively. Entering the library therefore requires loading r10
  with the *callee's* `got_base` first.

### 12b.2 The two XIP-only costs

**1. Function calls still need r10.** Even the "direct" test calls through
helpers that set r10 and use the trampoline (`test-xip/test-xip-static-direct/src/main.cpp`,
via the `test-helpers.hpp` `call_symN`/`set_r10` helpers):

```c
register void* r10_val asm("r10");
inline uint32_t call_sym1(const scl::Module& m, const char* name, uint32_t a) {
    void* fn = m.sym(name);
    r10_val = (void*)m.get_smodule().got_base;   // r10 setup, still required
    return pic_call_u32(fn, r10_val, a);
}
// ...
call_sym1(m, "library_function", 39);   // NOT the plain library_function(39)
```

**2. A provision-time pre-link is mandatory.** A PLT stub running from flash
reaches its GOT slot PC-relatively (`add ip, pc`) — so it reads the **flash**
copy of the GOT, never the RAM copy the loader writes. External *function*
imports must therefore be bound **before** the blob is programmed, via
`bind_host_imports()` (`include/xip/static/adapter.hpp:75`, called from the
`provision()` path at `main.cpp:76`). The loader *verifies* this happened and
refuses to load an under-pre-linked image (`src/xip/static/adapter.cpp:211`).
Non-XIP static has no such step — it patches everything at load time.

### 12b.3 What *does* transfer

- **Build-time linking.** `test-xip-static-direct` links the fixtures with
  `-l:slib.so -l:mylib1.so …` under `-pie -Wl,-E -Wl,-Bsymbolic` (main is PIC
  here — it is a hosted platform).
- **Data by name, no trampoline.** `load(blob, out, patch_host_got=true)` walks
  main's `_DYNAMIC` and writes module exports into main's own GOT/PLT slots
  (`src/xip/static/adapter.cpp:79`), so global *data* such as `s_info.tag` is
  read by plain name — the same `R_ARM_COPY`-injection effect as
  `*(uint32_t*)aa = 20` in §11.
- **Resolve-once-at-load semantics.** No `dlopen`/`dlsym` service, no lazy
  binding — the classic static-linker model.

### 12b.4 Side-by-side

| | `scl::static` (direct) | `scl::xip-static` (direct) |
|---|---|---|
| Code executes from | RAM buffer (copied) | flash (in place) |
| RAM used | whole image | only the RW segment |
| PIC model | `-fPIC` (PC-relative, no r10) | `-fpic` + r10 (`-mpic-register=r10`) |
| Function calls | plain `library_function(7)` | `set_r10` + `pic_call_u32(fn, got, a)` |
| Data by name | plain | plain (via `patch_host_got`) |
| Extra load-time step | none | `bind_host_imports()` at provision time |
| Reference app | `test-static/test-static-direct` | `test-xip/test-xip-static-direct` |

**Rule of thumb:** pick `scl::xip-static` when RAM is the scarce resource and
you accept the r10 trampoline plus the provision-time pre-link; pick
`scl::static` when you want the truly transparent `library_function(7)` call
site.

---

## 13. Decision tree

```
Need to call library functions with zero call-site ceremony?
│
├─ Yes, and I can link the fixtures at build time  ──►  scl::static  (direct)
│
├─ Yes, but I need runtime dlopen (unlinked fixtures) and have newlib+heap
│                                                     ──►  scl::dynamic-newlib
│
├─ Yes, but freestanding / no heap, and I accept r10 trampolines
│                                                     ──►  scl::dynamic-nolib
│
├─ Yes, but RAM is scarce and code must execute in place from flash (accept
│  the r10 trampoline + provision-time bind_host_imports)
│                                                     ──►  scl::xip-static
│
├─ Kernel-module style (ET_REL, no GOT), accept a hand-written symbol table
│                                                     ──►  scl::reloc
│
└─ Fixed set of alternates, no runtime choice, max speed
                                                      ──►  scl::overlay
```

For the specific constraint stated in §1 — *"main does nothing special, only
build options, plain calls"* — the answer is **`scl::static` (direct mode)**.
When execute-in-place is required, the closest variant is **`scl::xip-static`**
(direct), with the caveats of §12b.

---

## 14. Build & run recipe

```bash
# 1. Reference app (direct mode), WeAct board, under QEMU or flashed via OpenOCD
cmake -S test-static/test-static-direct -B test-static/test-static-direct/build \
      -DCMAKE_TOOLCHAIN_FILE=test-static/test-static-direct/cmake/arm-none-eabi.cmake
cmake --build test-static/test-static-direct/build -j

# 2. Or via the workspace's one-command runner (flashes + runs on hardware)
./build-and-run.sh weact static-direct
```

The runner names from `README-unify-cortx-m.md` §4: `static`,
`static-cpp`, `static-direct`, `drv` all exercise `scl::static`.

---

## 15. Gotchas the tests taught us

These are documented in `doc/manual-scl-loaders.md` §7.5 and visible in the
code; they matter when you extend the loader:

1. **`patch_shlib()` returns a count, not a bool.** `0` means "nothing to
   patch", not "failure". Treating it as a boolean caused a real early-return
   bug that skipped symbol resolution.
2. **`elf_image` holds a raw `byte_source*`.** After moving an `elf_reader`
   into a `shlib_loader`, the pointer stays valid only while the source is
   alive — extract section metadata *before* the move (the pattern in
   `Module::build`, `module.cpp:35-43`).
3. **`R_ARM_COPY` is address-injection here, not data copy.** Standard
   `R_ARM_COPY` would `memcpy` the library's variable into main's BSS; this
   loader deliberately writes the library symbol's *address* into main's slot
   so `main` dereferences the library's own storage (`*(uint32_t*)aa = 20`).
   See the comment at `shlib-loader.cpp:490`.
4. **`<elf.h>` vs `elf-types.hpp` macro collision.** The `elf-reader.h` header
   must include `loader/elf-types.hpp` **before** `<elf.h>`, because the latter
   defines `SHF_ALLOC`, `PT_LOAD`, `EM_ARM`, etc. as macros that would clash
   with the constexpr names (see the comment block at `elf-reader.h:4`).
5. **Separate RAM buffers per library.** A single shared buffer caused dangling
   function pointers when a second `.so` overwrote the first's code. Keep one
   buffer per concurrently-loaded module.
6. **Zero-reloc `.so` is legal.** A fixture that only touches hardcoded
   peripheral addresses can compile with `readelf -r` showing **no**
   relocations; `patch_shlib` then falls back to scanning `.dynsym` directly to
   register exports (`shlib-loader.cpp:432`).
7. **`--gc-sections` requires `-fno-common` and kept anchors.** Without
   `-fno-common` (fixture) and `-u` anchors (firmware, where relevant), the
   linker can drop symbols the loader must resolve, leaving null GOT slots.

---

## 16. Reference index

| Topic | File |
|---|---|
| `scl::Module` API | `include/static/module.h`, `src/static/module.cpp` |
| Segment load + GOT patch | `src/static/shlib-loader.cpp` |
| ELF parse / enumerate | `include/static/elf-reader.h`, `src/static/elf-reader.cpp` |
| `_reloc` block layout | `test-static/test-static/ldscripts/sections-sh.ld` |
| Fixture link (base 0) | `test-static/test-static/libsh1/ldscripts/{mem,sections2}.ld` |
| Reference app (direct) | `test-static/test-static-direct/src/main.cpp` |
| Fixture + firmware flags | `test-static/test-static-direct/CMakeLists.txt` |
| The SCL target | `CMakeLists.txt` (`scl-static`) |
| XIP adapter (two-base model) | `include/xip/static/adapter.hpp`, `src/xip/static/adapter.cpp` |
| XIP reference app (direct) | `test-xip/test-xip-static-direct/src/main.cpp` |
| XIP fixture flags (r10) | `test/fixtures-src/c1/c1.h` |
| TLS thread pointer | `include/static/tls.h`, `src/static/tls.c` |
| On-target TLS fixture | `test-static/test-static/tls-lib/tls_lib.{c,h}` (loaded by `test-static-direct`) |
| ELF32 parser (shared) | `common-elf-reader/include/loader/*`, `src/elf-image.cpp` |
| Full loader taxonomy | `doc/manual-scl-loaders.md`, `doc/scl-guide.md` |
| Migration rationale | `doc/scl-analysis-and-migration-plan.md` |

---

## 17. Worked example: from fixture source to a call in `main`

This chapter is a self-contained walkthrough of the *same* fixture library,
built and consumed **twice**: once with `scl::static` (RAM, plain calls) and once
with `scl::xip-static` (flash + r10). The fixture sources are the real
`test/fixtures-src/mylib1/` files; the main programs are minimal versions of the
two reference apps.

### 17.1 The fixture source

`mylib1.h` — the public contract (`test/fixtures-src/mylib1/mylib1.h`):

```c
#ifndef MYLIB1_H_
#define MYLIB1_H_
#include <stdio.h>
#include <string.h>
#if defined(__cplusplus)
extern "C" {
#endif

struct Str_Lib { char *a; float f; char *bb; unsigned int b, c; };
typedef unsigned int (*func)(unsigned int);

unsigned int library_function(unsigned int x);   /* x + 3 + aa (exported global) */
unsigned int library_function2(unsigned int x);  /* x*3 - cc + dd (internal statics) */
unsigned int library_function3(unsigned int x);  /* intra-lib calls + main's w() */
unsigned int callback_f(unsigned int x);         /* main's AA + f()               */
unsigned int g(unsigned int x);
unsigned int fff(unsigned int x);                /* main's AA + BB               */
char *retstr(void);
char *retstr_(void);
void print_lib(void);
void print_lib2(char *s);
struct Str_Lib ret_struct(struct Str_Lib stm);
struct Str_Lib ret_struct_lib(void);

#pragma weak foo
unsigned int foo(void);   /* weak: main may override */
#pragma weak boo
unsigned int boo(void);
#pragma weak boo2
unsigned int boo2(void);
unsigned int lkj(void);

#if defined(__cplusplus)
}
#endif
#endif /* MYLIB1_H_ */
```

`mylib1.c` — the fixture body (`test/fixtures-src/mylib1/mylib1.c`). It is the
"kitchen sink" that exercises every relocation the loaders support:

```c
#include "mylib1.h"

/* ---- externals left undefined: resolved from MAIN at load time ---- */
extern unsigned int AA;                 /* data in main  */
extern unsigned int BB;                 /* data in main  */
extern char        *s1;                 /* string in main */
extern unsigned int f(unsigned int);    /* function in main */
extern unsigned int w(unsigned int);    /* function in main */

/* ---- exported globals (R_ARM_GLOB_DAT) ---- */
unsigned int aa = 10;
unsigned int bb;                        /* zero-init (.bss) */

static unsigned int cc = 5, dd = 7;     /* internal statics (.data) */
static unsigned int bss1;               /* internal scratch (.bss) */

char *str  = "Hello mylib1!";           /* R_ARM_RELATIVE */
char *str_ = "Hello from mylib1!";

struct Str_Lib s_lib = { "15_", 125.236f, "1235", 120, 30 };

/* ---- constructors -> .init_array (collected into .inits by the ldscript) ---- */
__attribute__((constructor)) static void ctor_main(void) { aa += aa; bb = aa + 20; }
__attribute__((constructor)) static void ctor_banner(void) {}

unsigned int library_function(unsigned int x)  { return x + 3 + aa; }
unsigned int library_function2(unsigned int x) { return x * 3 - cc + dd; }
unsigned int library_function3(unsigned int x) {
    unsigned int a = library_function(x);     /* intra-lib call (GOT) */
    unsigned int b = library_function2(x);
    bb   = a + b + w(x);                      /* w resolved from main */
    bss1 = bb + a;
    return bb + bss1;
}
unsigned int callback_f(unsigned int x) { return AA + f(aa + bb + x); }
unsigned int g(unsigned int x)          { return aa + bb + x; }
unsigned int ptrfunc(func p, unsigned int x) { return p(x) + x; }
unsigned int gg(unsigned int x)         { return x + ptrfunc(g, x); }
unsigned int fff(unsigned int x)        { return AA + BB + x; }
char *retstr(void)  { return str; }
char *retstr_(void) { return str_; }
void print_lib(void)     { bss1 = 10; }
void print_lib2(char *s) { (void)s; }

/* ---- weak defaults: main may override ---- */
unsigned int foo(void)  { return 20; }
unsigned int boo(void)  { return 30; }
unsigned int boo2(void) { return boo() + 30; }
unsigned int lkj(void)  { return BB + 100; }
```

### 17.2 Fixture linker scripts

**Static flavour** — the whole library at run address 0 (`mem.ld`):

```
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35k }
```

`gcc.ld` includes the memory map plus a section layout:

```
INCLUDE "mem.ld"
INCLUDE "sections.ld"
```

`sections.ld` (the base-0 layout; a minimal version of the repo's
`test-static/test-static/libsh1/ldscripts/sections2.ld`):

```
SECTIONS
{
    .inits : ALIGN(4) { KEEP(*(.init)) KEEP(*(.fini))
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) } >RAM
    .plt     : { *(.plt) *(.plt.*) } >RAM
    .got     : ALIGN(4) { _lsgot = .; *(.got*)  . = ALIGN(4); _elsgot = .; } >RAM
    .got.plt : ALIGN(4) { _lsgot_plt = .; *(.got.plt*) . = ALIGN(4); _elsgot_plt = .; } >RAM
    .rodata  : ALIGN(4) { *(.rodata .rodata.*) } >RAM
    .text    : ALIGN(4) { *(.text .text.*) *(vtable) } >RAM
    .data    : ALIGN(4) { *(.data .data.*) } >RAM
    .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.*) *(COMMON) } >RAM
    /DISCARD/ : { *(.interp) }
}
```

**XIP flavour** — split R-X | RW (`test-xip/fixtures-src/ldscripts/xip-split.ld`):

```
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)

PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X: stays in flash */
  data PT_LOAD FLAGS(6);   /* RW : copied to RAM  */
}

SECTIONS
{
  . = SIZEOF_HEADERS;          /* makes text_vaddr == text_off */
  /* ---- R-X (flash) ---- */
  .hash         : { *(.hash) } :text
  .gnu.hash     : { *(.gnu.hash) } :text
  .dynsym       : { *(.dynsym) } :text
  .dynstr       : { *(.dynstr) } :text
  .rel.dyn      : { *(.rel*) } :text
  .text         : { *(.text .text.*) } :text
  .rodata       : { *(.rodata .rodata.*) } :text
  .plt          : { *(.plt) } :text
  .ARM.extab    : { *(.ARM.extab*) } :text
  .ARM.exidx    : { *(.ARM.exidx*) } :text
  . = ALIGN(16);
  /* ---- RW (RAM) ---- */
  .preinit_array : { KEEP(*(.preinit_array)) } :data
  .init_array    : { KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) } :data
  .fini_array    : { KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) } :data
  .data.rel.ro   : { *(.data.rel.ro .data.rel.ro.*) } :data
  .dynamic       : { *(.dynamic) } :data
  .got           : { *(.got*) } :data
  .data          : { *(.data .data.*) } :data
  .bss           : { *(.bss .bss.*) *(COMMON) } :data
}
```

The critical difference: the static script packs everything at vaddr 0 (one load
bias); the XIP script splits the image into two `PT_LOAD` segments so the text
executes in place from flash and only the RW half is copied to RAM.

### 17.3 Build the fixture — static flavour

```bash
TC=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
CC=$TC/arm-none-eabi-gcc

# PC-relative GOT, soft-float: the -fPIC model (no r10).
$CC -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
    -fPIC -fno-inline-functions -fno-common -O0 -g3 \
    -I mylib1 -c mylib1/mylib1.c -o mylib1.o

# link a base-0 .so
$CC -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
    mylib1.o -o libmylib1.so \
    -shared -nostartfiles -T ldscripts/gcc.ld \
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments
```

### 17.4 Build the fixture — XIP flavour

```bash
TC=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
CC=$TC/arm-none-eabi-gcc

# r10-based PIC, hard-float, split R-X | RW segments.
$CC -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
    -fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative \
    -O0 -ffunction-sections -fdata-sections -fno-inline-functions \
    -shared -nostartfiles -Wl,-E -Wl,-Bsymbolic \
    -T ldscripts/xip-split.ld \
    -I mylib1 -o mylib1.so mylib1/mylib1.c
```

`-Wl,-Bsymbolic` binds intra-library calls locally (direct `bl`, no PLT) so only
genuine external (main) calls route through the GOT — required because a PLT
stub running from flash would otherwise read the *flash* copy of the GOT.

### 17.5 `main` — static flavour

```c
// main_static.cpp — scl::static direct mode: one load_file(), then plain calls.
#include "mylib1.h"                 // ordinary #include of the library header
#include "static/module.h"          // scl::Module
#include "static/elf-reader.h"      // elf_loader::symbol_table
#include <cstdint>

// ---- exports the library imports; the linker places them in main's .dynsym
//      (they are referenced by the linked .so), the loader finds them via _reloc.
extern "C" {
unsigned int AA = 5;
unsigned int BB = 55;
char*        s1 = (char*)"Help....\n";
unsigned int f(unsigned int x) { return 3 * x; }
unsigned int w(unsigned int x) { return AA + x; }
}

alignas(8) static uint8_t ram_lib[32 * 1024];     // the loader's RAM buffer
static elf_loader::symbol_table g_syms;           // cross-module interposition
static scl::Module g_mod;                          // the loaded module

int main() {
    if (!g_mod.load_file("libmylib1.so", ram_lib, sizeof ram_lib, &g_syms))
        return 1;

    // constructor ran: aa 10->20, bb = 40 (the .so's ctor is executed at load).
    unsigned int r1 = library_function(7);    // plain call — no ceremony at all
    unsigned int r2 = library_function3(7);   // calls back into main's w()
    unsigned int r3 = callback_f(7);          // uses main's AA + f()
    char* s = retstr();                       // R_ARM_RELATIVE string
    (void)r1; (void)r2; (void)r3; (void)s;
    return 0;
}
```

The firmware links the fixture at build time (`-lmylib1`), non-PIC, with a
linker script that emits `_reloc` and reserves `.plt/.got/.got.plt`
(`test-static/test-static-direct/CMakeLists.txt`):

```cmake
add_executable(main-static main_static.cpp)
target_link_libraries(main-static PRIVATE scl::static)   # INTERFACE: sources
target_link_options(main-static PRIVATE
    -T mem-sh.ld -T libs-sh.ld -T sections-sh.ld
    -nostartfiles --specs=nano.specs -u _printf_float
    -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
    -L${CMAKE_CURRENT_SOURCE_DIR}/libs -lmylib1 -lstdc++)
```

### 17.6 `main` — XIP flavour

```c
// main_xip.cpp — scl::xip-static direct mode: provision -> load -> r10 call.
#include "xip/static/adapter.hpp"
#include "scl/pic-trampoline.h"
#include "fs-archive.hpp"            // app-owned flat-fs (flash provisioning)
#include "firmware/semihost-file.hpp" // app-owned semihosting file source
#include "scl/image-memory.hpp"       // fw::image_arena
#include "mylib1.h"
#include <cstdint>

extern "C" {
unsigned int AA = 42;                 // main is built -pie -Wl,-E: everything
unsigned int BB = 84;                 // defined here lands in main's .dynsym.
char*        s1 = (char*)"main-s1";
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
}

register void* r10_val asm("r10");    // the PIC base register

// Read the .so, PRE-LINK its external function imports into the flash-bound
// image, and store it in flash. bind_host_imports() MUST run before flashing.
static bool provision(const char* name) {
    firmware::semihost_file src(name);
    if (!src.is_open()) return false;
    auto memp = fw::image_arena().alloc(src.size());
    auto* buf = static_cast<std::byte*>(*memp);
    if (!src.read_at(0, std::span<std::byte>{buf, src.size()})) return false;
    xip::stat::bind_host_imports(std::span<std::byte>{buf, src.size()});
    bool ok = fw::fs_add(name, buf, static_cast<uint32_t>(src.size())).has_value();
    fw::image_arena().free(buf);
    return ok;
}

int main() {
    provision("mylib1.so");

    const fw::FsEntry* e = fw::fs_find("mylib1.so");
    xip::stat::SModule m;
    // patch_host_got=true: also point main's own GOT/PLT slots at this module's
    // exports, so data reads by name work with no trampoline.
    xip::stat::load(std::span<const std::byte>{fw::fs_blob(*e), e->size}, m,
                    /*patch_host_got=*/true);

    // function call — code runs from flash, so set r10 to the module's got_base:
    void* fn = xip::stat::resolve(m, "library_function");
    r10_val  = reinterpret_cast<void*>(m.got_base);
    unsigned int r = pic_call_u32(fn, r10_val, 7);   // NOT a plain call

    xip::stat::unload(m);
    return r;
}
```

The firmware links the fixture at build time (`-l:mylib1.so`), **PIC** (`-pie`)
because it is a hosted platform (`test-xip/test-xip-static-direct/CMakeLists.txt`):

```cmake
add_executable(main-xip main_xip.cpp ${APP_OWNED_FS_SRCS})
target_link_libraries(main-xip PRIVATE platform-board-hosted scl::xip-common
                                     ${SCL_LIBSUPCXX} ${SCL_LIBGCC})
target_link_options(main-xip PRIVATE
    -pie -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic LINKER:-E
    LINKER:--unresolved-symbols=ignore-in-shared-libs
    -T${XIP_FLATFS_LD}
    -L${CMAKE_CURRENT_SOURCE_DIR}/fixtures -l:mylib1.so -lstdc++)
```

### 17.7 The two flavours, one table

| Step | `scl::static` | `scl::xip-static` |
|---|---|---|
| Fixture PIC model | `-fPIC` (PC-relative) | `-fpic -msingle-pic-base -mpic-register=r10` |
| Fixture link script | `mem.ld` (base 0) + `sections.ld` | `xip-split.ld` (R-X \| RW) |
| Fixture float ABI | `-mfloat-abi=soft` | `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| Main build | non-PIC, `-lmylib1`, `--no-dynamic-linker` | `-pie -Wl,-E -Bsymbolic`, `-l:mylib1.so` |
| Load call | `mod.load_file(path, buf, sz, &syms)` | `bind_host_imports()` (prov) + `load(blob, m, true)` |
| Function call | `library_function(7)` | `set r10; pic_call_u32(fn, got_base, 7)` |
| Data read | plain (COPY slot) | plain (via `patch_host_got`) |
| Code runs from | RAM buffer | flash |

---

## 18. Summary

For "load shared libraries with a `main` that does nothing special and calls
the libraries plainly", the SCL's answer is **`scl::static` in direct mode**:

- the **fixture** is built `-fPIC -shared -nostartfiles` at vaddr 0,
- the **firmware** links it with `-l<lib>` and a linker script that reserves
  `.plt/.got/.got.plt` and emits the `_reloc` block,
- **`main`** writes exactly one `mod.load_file(...)` and then calls
  `library_function(7)` as if the library had always been there.

Every alternative either moves preparation into the call site (`got_base`,
r10 trampolines), into a hand-written symbol table, or into a heavier hosted
runtime. When the goal is *minimum ceremony in application code*, `scl::static`
is the better solution.

One caveat: the execute-in-place flavour **`scl::xip-static`** keeps the
resolve-once-at-load model and the "data by name" trick, but because flash code
reaches its RAM GOT through r10, function calls keep the `pic_call_u32` + r10
ceremony and require a `bind_host_imports()` pre-link at provision time (§12b).
For a fully transparent `library_function(7)` call site, use the non-XIP
`scl::static` direct mode.

---

## 19. Starting a new application that loads shared libraries

You have an application folder with `src/`, `include/` and a `CMakeLists.txt`.
This chapter is the step-by-step recipe for wiring `scl::static` (and its
dependency `common-elf-reader`) into it as **INTERFACE source libraries**, and
for deciding where your fixture lives.

### 19.1 The dependency layout

The SCL is `cortex-m-scl` (this repository). It is consumed by **source**
(`add_subdirectory`), never as a prebuilt `.a`. It depends on
`common-elf-reader`, also an INTERFACE library. Both are INTERFACE targets, so
their `.cpp` files are compiled *into your application target* with your flags.

Recommended tree:

```
my-app/
├── CMakeLists.txt
├── src/
│   └── main.cpp                 # your application
├── include/
├── deps/
│   ├── cortex-m-scl/            # <== add_subdirectory(deps/cortex-m-scl)
│   │   ├── CMakeLists.txt
│   │   ├── include/  src/  ...
│   │   └── common-elf-reader/   # the SCL's own copy (vendored) OR a submodule
│   └── ...
├── cmake/
│   └── arm-none-eabi-hosted.cmake   # copied from the SCL's cmake/ dir
├── fixtures/
│   ├── mylib1.h                 # header (compile-time)
│   └── libmylib1.so             # .so (link-time AND runtime)
└── ldscripts/                   # YOUR board linker script (app-owned)
```

### 19.2 Obtaining the two repositories

`cortex-m-scl` expects `common-elf-reader` at the path in the
`COMMON_ELF_READER_DIR` cache variable (default: `./common-elf-reader` *beside
the SCL's own `CMakeLists.txt`*). Three equivalent ways to satisfy it:

```bash
# (a) vendor the SCL, then place the reader beside its CMakeLists.txt
git clone <scl-url> deps/cortex-m-scl
git clone <reader-url> deps/cortex-m-scl/common-elf-reader    # or copy it in

# (b) keep both as git submodules of my-app (the SCL's own .gitmodules style)
git -C my-app submodule add <scl-url>    deps/cortex-m-scl
git -C my-app submodule add <reader-url> deps/cortex-m-scl/common-elf-reader

# (c) let the SCL stay put and point at the reader explicitly (CMake cache)
cmake -DCOMMON_ELF_READER_DIR=/path/to/elf32_read_parse ...
```

> The reader is standalone C++20 and self-contained (`common-elf-reader/CMakeLists.txt`
> exposes the alias `common-elf-reader::elf-reader`). If you already have it,
> option (c) is the least work.

### 19.3 Pick the toolchain

`scl::static` uses `std::vector` / `std::unique_ptr` and links `libstdc++`, so
it needs the **hosted** (newlib) toolchain — *not* the freestanding one:

| Loader | Toolchain file | Notes |
|---|---|---|
| `scl::static`, `scl::dynamic-newlib` | `cmake/arm-none-eabi-hosted.cmake` | newlib + heap, non-PIC, soft/hard float via `-DSCL_HF` |
| `scl::dynamic-nolib`, `scl::reloc`, `scl::overlay` | `cmake/arm-none-eabi.cmake` | freestanding, `-fpic` |

Copy the file you need into your `cmake/` directory (or point at the SCL's
copy) and pass it with `-DCMAKE_TOOLCHAIN_FILE=`.

### 19.4 The application `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(my-app LANGUAGES C CXX)

# scl::static needs the hosted C++ parts (std::vector). Force before add_subdirectory.
set(SCL_HOSTED ON CACHE BOOL "" FORCE)
# Where the SCL finds the ELF reader (only needed if not beside the SCL).
set(COMMON_ELF_READER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/cortex-m-scl/common-elf-reader"
    CACHE PATH "Path to common-elf-reader")

# The SCL: INTERFACE source libraries, compiled into my-app below.
add_subdirectory(deps/cortex-m-scl scl)

# (Your app-owned board/startup/platform target goes here — see §19.7.)

add_executable(my-app src/main.cpp)
target_link_libraries(my-app PRIVATE scl::static)     # <== the whole loader

# Link the fixture at build time (direct mode) and let the loader patch the GOT.
target_link_options(my-app PRIVATE
    -nostartfiles --specs=nano.specs
    -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
    -T${CMAKE_CURRENT_SOURCE_DIR}/ldscripts/my-board.ld
    -L${CMAKE_CURRENT_SOURCE_DIR}/fixtures -lmylib1 -lstdc++)
```

That is the entire integration: `add_subdirectory` + one
`target_link_libraries(... scl::static)`. The loader sources
(`elf-reader.cpp`, `shlib-loader.cpp`, `module.cpp`, `logger.cpp`, and the
reader's `elf-image.cpp`) are pulled in transitively and compiled with your
target's flags.

### 19.5 Where does the fixture live?

The direct mode needs **three things** from a fixture, at three different times:

| What | When | Why |
|---|---|---|
| `mylib1.h` | compile time | so `main` can call `library_function()` with the right prototype |
| `libmylib1.so` | **link time** | `-lmylib1` binds the symbols and emits the PLT/GOT stubs |
| `libmylib1.so` | **run time** | `load_file()` reads its bytes into the RAM buffer |

So there are two valid ways to keep a fixture, and in **both** cases the header
and the `.so` are required at build time — the only question is whether the
`.so` is built from source or shipped prebuilt:

**(a) Headers + prebuilt `.so` only** — drop `mylib1.h` and `libmylib1.so` into
`fixtures/` and you are done. No fixture source, no fixture linker script. Use
this when the fixture is a third-party blob or built elsewhere. **Caveat:** the
prebuilt `.so` must match your firmware ABI exactly — same `-mcpu`, same float
ABI (`-mfloat-abi=soft` for `scl::static`), and the `-fPIC` model (not
`-fpic`+r10). A mismatched `.so` links or runs wrongly with no clear error.

**(b) Build from source** — vendor `mylib1.c` + `mylib1.h` + the fixture linker
script (`mem.ld`/`gcc.ld`, see §17.2), and let CMake build the `.so` as a
custom target with your flags. This is reproducible and keeps the fixture ABI
in lock-step with your firmware. Recommended when you own the fixture.

### 19.6 Building the fixture from source in CMake

```cmake
set(SO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3)

add_custom_command(
    OUTPUT "${SO_DIR}/libmylib1.so"
    COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} -I"${SO_DIR}"
            -c "${SO_DIR}/mylib1.c" -o "${SO_DIR}/mylib1.o"
    COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} "${SO_DIR}/mylib1.o"
            -o "${SO_DIR}/libmylib1.so"
            -shared -nostartfiles -T "${SO_DIR}/ldscripts/gcc.ld"
            -Wl,--gc-sections -Wl,--no-warn-rwx-segments
    DEPENDS "${SO_DIR}/mylib1.c" "${SO_DIR}/mylib1.h"
    COMMENT "Building libmylib1.so (PIC)"
    VERBATIM)
add_custom_target(mylib1 DEPENDS "${SO_DIR}/libmylib1.so")
add_dependencies(my-app mylib1)     # build the .so before linking my-app
```

`-L"${SO_DIR}" -lmylib1` (from §19.4) then finds this `.so` at link time.

### 19.7 What remains app-owned (the SCL will not provide it)

Per the SCL design contract, your application owns everything platform-specific.
The minimal set for a `scl::static` app is:

- **startup / vector table / `sbrk`** — the hosted newlib bootstrap (the SCL's
  tests vendor `test/platforms/board-hosted/` for reference; copy that pattern).
- **board init** — clocks, LED, etc.
- **linker script** — your board's `mem.ld`/`sections.ld` **plus** the
  `_reloc` block and `.plt/.dynsym/.dynstr/.hash/.rel.dyn` (FLASH) and
  `.got/.got.plt` (RAM) sections from `test-static/test-static/ldscripts/sections-sh.ld`
  (§5.3).
- **the byte source for `load_file`** — a semihosting file reader, a flash
  filesystem, or a raw buffer (`Module::load_buf` for embedded byte arrays).
- **the export surface** — the globals/functions the fixture imports from your
  app (`AA`, `BB`, `s1`, `f`, `w` in §17.5).

### 19.8 Checklist

- [ ] `deps/cortex-m-scl` present, with `common-elf-reader` beside it (or
      `COMMON_ELF_READER_DIR` set).
- [ ] `SCL_HOSTED=ON` set before `add_subdirectory`.
- [ ] `add_subdirectory(deps/cortex-m-scl scl)`.
- [ ] `target_link_libraries(my-app PRIVATE scl::static)`.
- [ ] hosted toolchain file (`arm-none-eabi-hosted.cmake`) on
      `-DCMAKE_TOOLCHAIN_FILE`.
- [ ] fixture: `mylib1.h` + `libmylib1.so` in `fixtures/` (prebuilt) **or** a
      `mylib1.c` custom target (from source).
- [ ] `-L<fixtures> -lmylib1` + `--no-dynamic-linker` on the link line.
- [ ] board linker script emits `_reloc` and reserves GOT/PLT sections.
- [ ] `main` defines the symbols the fixture imports, and calls
      `mod.load_file(...)` before first use.

Once all boxes are ticked, `main` calls `library_function(7)` exactly as if the
library had been linked in statically — but its bytes are loaded and patched at
run time by `scl::static`.

---

## 20. The rules, in plain English

Everything in §17–§19 boils down to **two promises** between your program and
the library. Think of the library as a person who moves into a furnished flat:
it brings its own furniture (its code and its own variables), but it expects to
borrow certain things from the flat (your functions and variables), and it
refuses to be tied to one specific address — it must work no matter which flat
it ends up in.

1. **The library promises** to be *self-contained and address-agnostic*: no
   assumptions about where it lives, no `main`, and an explicit list of what it
   borrows.
2. **Your program promises** to *supply the borrowed things*, to put the library
   somewhere in memory, and to keep those borrowed things actually present in
   the final binary.

Every flag and every "gotcha" in this chapter is just one of those two promises
being enforced.

### 20.1 What the library must promise

**It must not care where it is.** On a normal computer a program runs at a fixed
address; a `.so` here must run *anywhere* in RAM (or, for XIP, its code must run
from flash while its variables live in RAM). That single rule is why the fixture
is built `-fPIC` (or `-fpic` + the `r10` register). In plain terms: the compiler
is told "never write down an absolute address — always find things *relative to
where you are*." If you forget this, the code loads fine and then jumps to or
reads from a hard-coded address that no longer exists.

**It has no "start" of its own.** A library is not a program, so there is no
`main()`, no reset vector, no C startup code (`-nostartfiles`). If it has
one-time setup to do — "when I arrive, double my counter and remember the
total" — it does that in a **constructor** (C: `__attribute__((constructor))`;
C++: a global object). The loader runs these constructors the moment the
library is placed in memory.

**It must say clearly what it borrows.** Any function or variable the library
uses but does not own must be declared as *external* and left undefined — e.g.
`extern unsigned int AA;` or `extern float sinf(float);`. The loader's whole job
is to fill these "IOUs" with real addresses from your program. Two subtleties:

- **Only public names are lendable.** A name marked `static` (or hidden inside a
  C++ anonymous namespace) is *private* to the library — it is invisible to the
  loader, and equally, your program's private names are invisible to the
  library. Both sides can only exchange names that are deliberately made
  *public* (external linkage).
- **Only certain kinds of "IOU" are supported.** The loader knows how to fix a
  handful of ARM relocation kinds — roughly: "fill this slot with an address",
  "add my base address to this value", "copy this one from main". It does *not*
  know how to handle thread-local variables (TLS) or the absolute-addressing
  tricks used by a different loader (`scl::reloc`). So: no `thread_local`, and
  keep the build PIC.

**It must speak the same "accent" as your program.** The library and your
firmware must agree on the *floating-point ABI* (soft vs hard float) and the CPU
(`-mcpu`, `-mthumb`). This is the same rule as any static link — but here the
compiler can't check it for you at link time, so a mismatch shows up as
mysterious wrong results or crashes instead of a build error.

### 20.2 What your program must promise

**Supply everything the library borrows.** If the library borrows `AA`, `w()`,
`printf`, `malloc`, `sinf`, or C++ `operator new`, your program must actually
provide them — either by defining them, or by linking the library (libc/libm)
that defines them and then *re-exporting* those names so the loader can see
them. The exact mechanism differs per loader (`-Wl,-E` / the `_reloc` block /
`-u` anchors / a hand-written table), but the promise is the same: **every
borrowed name must resolve to a real address at load time.**

**Don't let the linker throw borrowed things away.** Here is the classic
Cortex-M trap. If you build with `--gc-sections` (the usual size-saving flag),
the linker removes any function it thinks nobody calls. But the library *does*
call some of your functions (`printf`, say) — only the linker can't see those
calls, because they happen at run time through the loader. So the linker
quietly deletes `printf`, and the first library call to it Hard Faults. The fix
is to "anchor" the borrowed names (`-u symbol`, `KEEP()`, `[[gnu::used]]`) or
turn `--gc-sections` off.

**Make the borrowed names public, and unmangled.** C++ renames ("mangles")
functions to encode their type, so `f(unsigned int)` becomes `_Z1fj` in the
symbol table. The loader looks names up by the *string* in the `.so`, so both
sides must agree. The practical rule: **declare the shared surface `extern "C"`**
on both the library and the program.

**Put the library somewhere, and read it.** You must reserve a RAM buffer (or
arena) and hand it to `load_file()` before the first call. There is no way
around this one step — the bytes have to physically land somewhere.

**Keep the symbol table.** The loader finds "your" symbols by reading your
program's own symbol table, so don't strip it (`-s`/`--strip-all`).

**In direct mode, link the library too.** For the plain
`library_function(7)` call to compile into an ordinary function call, the linker
must already know `library_function` exists — so the fixture is *also* linked at
build time (`-lmylib1`). Remember: this only binds the *name*; the actual bytes
are still loaded at run time.

### 20.3 The C++ twist

C++ adds four extra rules, all in plain English:

- **Exceptions and RTTI are a flavour choice.** In the `scl::static` world, turn
  them off (`-fno-exceptions -fno-rtti`) — simplest, and everything just works
  for plain classes. If you *need* exceptions to cross the library boundary, use
  `scl::dynamic-nolib`, where your program provides the exception machinery
  (`__cxa_*`, the personality routine, `__dynamic_cast`) and the loader
  registers the library's unwind tables (`.ARM.exidx`) so a `throw` inside the
  library can be caught by your program.
- **Global objects are built, and optionally torn down.** A C++ global object with
  a constructor runs when the library loads (that's the constructor story again).
  Its *destructor* runs only if you call `Module::unload()`, which walks
  `.fini_array` in reverse. Keep in mind `-fno-use-cxa-atexit` means "don't
  register my destructor to run later" — so teardown happens *only* through an
  explicit `unload()`, never automatically. If you never unload, destructors
  simply never run (which is fine for objects that need no tidy teardown).
- **Function-local statics need `-fno-threadsafe-statics`.** A "magic static"
  (`static Foo f;` inside a function) normally gets hidden once-only
  initialization guards; on bare metal we turn those off, so such a static
  initializes the plain way.
- **`new`/`delete` come from your heap.** The library's `new` is not linked in —
  it borrows your `operator new`, which you implement on top of `malloc`/`free`.
  So every allocation the library makes lives in *your* heap, and cross-module
  allocation/deallocation stays consistent (same heap, same rules).

### 20.4 One-table translation

| Plain-English rule | The flag / spelling |
|---|---|
| "find things relative to where I am" | `-fPIC` (static) or `-fpic -msingle-pic-base -mpic-register=r10` (XIP/nolib) |
| "I'm not a program" | `-shared -nostartfiles` |
| "don't inline my entry points away" | `-fno-inline-functions` |
| "keep my tentative globals" | `-fno-common` |
| "my globals init via constructors" | `__attribute__((constructor))` / C++ global objects → `.init_array` |
| "I borrow this from the program" | `extern ...;` (left undefined) |
| "make this name public" | no `static`, no anonymous namespace |
| "I have a soft override for this" | `#pragma weak` / `__attribute__((weak))` |
| "same accent as the firmware" | matching `-mfloat-abi`, `-mcpu`, `-mthumb` |
| — | — |
| "I provide what the library borrows" | define it, or link libc/libm and re-export (`-Wl,-E` / `_reloc` / `-u`) |
| "don't delete names only the library uses" | `-u symbol`, `KEEP()`, `[[gnu::used]]`, or drop `--gc-sections` |
| "same names on both sides" | `extern "C"` |
| "put the library somewhere" | reserve a buffer + `load_file()` |
| "keep my symbol table" | don't use `-s`/`--strip-all` |
| "link it so plain calls compile" | `-lmylib1` + `--no-dynamic-linker` |
| "thread_local works" | `__thread` → emulated TLS (`__emutls_*`), ordinary relocations (§21) |

If you remember nothing else: **the library must be position-independent and
explicit about what it borrows; your program must provide and keep-alive every
borrowed name, using matching ABI and C linkage, and must place the library in
memory before the first call.**

---

## 21. What `scl::static` still lacks — and what this branch improved

### 21.1 Improvements shipped here

Concrete upgrades to the `scl::Module` facade (all backward-compatible — existing
callers are unaffected):

| Improvement | API | Notes |
|---|---|---|
| **Byte-exact unload** | `Module::unload()` / `release()` | runs `.fini_array` in reverse, **restores every word `patch_main` wrote into main's memory** (patch journal), unpublishes exports, frees the TLS block, zeros the buffer. |
| **Reference counting** | `retain()` / `release()` / `refcount()` | `unload()` is idempotent; the module tears down only when the refcount reaches 0. |
| **Visibility scopes** | `load_file(..., scope::LOCAL)` | `RTLD_LOCAL`/`RTLD_GLOBAL` — LOCAL keeps exports out of the shared `symbol_table`. |
| **DT_NEEDED auto-load** | `load_file(..., dep_loader)` | a callback is invoked for each `DT_NEEDED` soname after a successful load. |
| **Reentrancy lock** | `scl_loader_lock()` / `scl_loader_unlock()` | weak no-op hooks (override for an RTOS mutex or IRQ disable) serialising load/unload. |
| **thread_local (TLS)** | `scl_tls_*`, `Module::tls_offset()`, `src/static/tls.c` | multi-thread-capable: each module gets a byte offset in a shared per-thread TLS area; native `R_ARM_TLS_*` relocations are applied, and `__emutls_*` (emulated TLS) works through ordinary relocations. On-target test in `test-static-direct` (`tls-lib`). |
| **Richer errors** | `last_detail()`, `last_error_string()` | exposes the fine-grained `loader_error` (`InvalidSegment`, `InvalidRelocation`, …). |
| **Buffer sizing** | `Module::image_size(path)` | returns the exact byte count the loader needs. |
| **Faster lookup** | `Module::sym()` | SysV `.hash` walk when present, linear fallback (identical semantics). |
| **Leak fix** | `~Module()` | the temporary patch arrays are freed; the destructor tears down an un-unloaded module (no TLS leak). |
| **Export tracking** | `symbol_table::remove()` | lets `unload()` unpublish exactly what a module registered. |

### 21.2 Still open (intentional)

| Gap | Status |
|---|---|
| **Lazy PLT binding** (patch GOT on first call) | **Deliberate non-goal.** `scl::static` resolves once at load by design; eager binding is both simpler and faster for pre-linked `.so` files (a handful of slots). The machinery exists in `scl::dynamic-nolib` (`lazy-resolve`, `lazy-plt`) for anyone who wants on-first-call binding — porting it here means adding a soft-float resolver trampoline and re-deriving the `-fPIC` PLT slot mapping, for a few microseconds of saved load time. |
| **Hard-float variant** | Config, not code — build fixture+firmware with `-mfloat-abi=hard -mfpu=fpv4-sp-d16` and matching ABI on both sides; no loader change required (plain calls pass floats in `s0`, and the loader contains no VFP assembly). |
| **Multiple concurrent loads of the same `.so`** | Not deduplicated; load the same file into separate buffers and they are independent modules. |
| **Global-dynamic TLS** (`__tls_get_addr`) | Not implemented — native TLS support targets initial-exec (`-ftls-model=initial-exec`); the GD model would additionally need a per-module TLS descriptor table. |

> **How `unload()` stays honest now.** `patch_main` snapshots every word it is
> about to overwrite into a journal (`patch_record { address, original }`,
> `include/static/elf-reader.h`), and `unload()` replays it in reverse before
> freeing the buffer — so after `unload()` the firmware is bit-identical to before
> `load_file()`. This mirrors `scl::dynamic-newlib`'s `patch_journal`, but scoped
> to *main's* writes only (the library's own buffer is freed, so its words need no
> journal). The remaining caveat: `patch_shlib`'s writes into the library buffer are
> not journaled — correct, because that buffer is discarded wholesale.

> **On thread_local — two mechanisms.** On `arm-none-eabi` GCC 15, `__thread`/
> `thread_local` lowers to **emulated TLS** (`__emutls_v.<name>` control structs +
> a call to `__emutls_get_address` from libgcc) — *not* native PT_TLS. That path
> needs nothing special from the loader: the control structs are ordinary
> GLOB_DAT/RELATIVE relocations and `__emutls_get_address` is either self-contained
> in the `.so` (built with `-nostartfiles`) or a JUMP_SLOT resolved from main. The
> on-target test (`test-static/test-static-direct/tls-lib`) exercises exactly this.
>
> The **native TLS** path (`PT_TLS` segment + `R_ARM_TLS_TPOFF32/DTPOFF32/DTPMOD32`)
> is also implemented for toolchains that emit it: the loader assigns each module a
> byte offset in a shared per-thread area (`scl_tls_register`), bakes
> `offset + st_value` into the GOT, and `scl_tls_init_area()`/`scl_tls_set_pointer()`
> let an RTOS lay out one area per thread (`__aeabi_read_tp` returns the current
> thread's base). It assumes `-mtp=soft` and "variant I" (thread pointer == area base).

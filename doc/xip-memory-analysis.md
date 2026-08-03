# XIP Memory Analysis: RW-Only RAM Loading for Symbol Patching/Resolution

## 1. Two-Base XIP Model

```text
FLASH (memory-mapped, 0x08000000+)           RAM (0x20000000+)
┌──────────────────────────────────┐         ┌─────────────────────────┐
│  R-X segment (never copied)      │         │  RW segment (only RAM   │
│                                  │         │  allocation)            │
│  ┌─────────────────────────┐     │         │  ┌───────────────────┐  │
│  │ .text (code)            │     │         │  │ .got              │  │
│  │ .rodata (strings,       │     │         │  │ .data             │  │
│  │         constants)      │     │         │  │ .data.rel.ro      │  │
│  │ .plt (PLT stubs)        │     │         │  │ .init_array       │  │
│  │ .got.plt (GOT slots)    │     │         │  │ .bss (zeroed)     │  │
│  │ .ARM.exidx              │     │         │  └───────────────────┘  │
│  │ .dynsym / .dynstr       │     │         │                         │
│  │ .hash                   │     │         │  text_base = flash addr │
│  │ .rel.dyn / .rel.plt     │     │         │  data_base = RAM addr   │
│  └─────────────────────────┘     │         │  got_base  = data_base  │
│  text_base = &flash[text_off]    │         │              + .got_off │
│                                  │         │  (= r10 for pic_call)   │
└──────────────────────────────────┘         └─────────────────────────┘
```

---

## 2. ELF Segment Classification (classify_segments)

```text
Input: raw .so bytes in flash (std::span)

Program headers (PT_LOAD):
┌──────┬──────┬───────┬──────────┬──────────┐
│ type │perm  │p_vaddr│ p_offset │ p_memsz  │
├──────┼──────┼───────┼──────────┼──────────┤
│ LOAD │ R-X  │ 0x100 │ 0x100    │ 0x8000   │  ──►  text_vaddr, text_off, text_size
│ LOAD │ RW   │ 0x8100│ 0x8100   │ 0x400    │  ──►  data_vaddr, data_off, data_memsz
└──────┴──────┴───────┴──────────┴──────────┘

Requirement for XIP eligibility:
  1. Exactly 2 PT_LOAD segments: one R-X, one RW
  2. p_vaddr == p_offset for both segments
     (PLT stubs compute GOT addresses PC-relative,
      so VA must equal file offset for the math to work)
  3. text_is_immutable(): no relocation targets the R-X range

Output: SegmentMap { text_vaddr, text_off, text_size,
                     data_vaddr, data_off, data_filesz, data_memsz,
                     split = true }
```

### PHDR FLAGS values

Defined in the library linker script (`fixtures-src/ldscripts/xip-split.ld`):

```text
PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X: stays in flash */
  data PT_LOAD FLAGS(6);   /* RW:  copied to RAM  */
}

ELF p_flags bitmask:
  PF_X = 1  (execute)
  PF_W = 2  (write)
  PF_R = 4  (read)

  FLAGS(5) = 4 + 1 = PF_R | PF_X   →  R-X  (read+execute)
  FLAGS(6) = 4 + 2 = PF_R | PF_W   →  RW-  (read+write)
```

The loader's `classify_segments()` (`src/xip/segment-map.cpp:29-30`) reads `p_flags` to decide
which segment is R-X (stays in flash) vs RW (copied to RAM).

### Safety scan: text_is_immutable

```text
For each SHT_REL section in the image:
  For each Rel entry:
    ┌─────────────┬──────────────┐
    │ r_offset in │ Action       │
    │ writable    │              │
    │ section?    │              │
    ├─────────────┼──────────────┤
    │ Yes         │ Skip (legit  │
    │             │ RW fixup)    │
    ├─────────────┼──────────────┤
    │ No, in R-X  │ Return false │
    │ range       │ → fall back  │
    │             │   to copy-   │
    │             │   whole      │
    └─────────────┴──────────────┘
```

---

## 3. Provision Lifecycle (one time, pre-deployment)

```text
┌─────────────────────────────────────────────────────────────────────┐
│  PROVISION (main.cpp:124-145)                                       │
│                                                                     │
│  semihost_file src(path)                                            │
│       │                                                             │
│       │  src.size() = sz                                           │
│       ▼                                                             │
│  ┌──────────────────────────────┐                                   │
│  │  fw::image_arena().alloc(sz) │  ← WHOLE FILE in temp arena      │
│  │  ┌────────────────────────┐  │    (peak: largest .so, ~20-50KB) │
│  │  │ raw .so bytes          │  │                                   │
│  │  │ (headers + .text +     │  │                                   │
│  │  │  .data + .got.plt +   │  │                                   │
│  │  │  .dynsym + .rel.* ...)│  │                                   │
│  │  └────────────────────────┘  │                                   │
│  └──────────────────────────────┘                                   │
│       │                                                             │
│       ▼                                                             │
│  bind_host_imports()                   ← patch JUMP_SLOT GOT slots │
│       │                                  in-place (no alloc)       │
│       ▼                                                             │
│  fw::fs_add(name, buf, sz)             ← program to FLASH_FS      │
│       │                                  from RAM source           │
│       ▼                                                             │
│  fw::image_arena().free(buf)            ← temp buffer released    │
│                                                                     │
│  Result: .so stored in memory-mapped FLASH_FS at 0x08040000+       │
└─────────────────────────────────────────────────────────────────────┘
```

### bind_host_imports detail

```text
Only needed for STATIC XIP (pre-binding):
  For each JUMP_SLOT in .rel.plt / .rel.dyn
    where st_value == 0 (external function import):
      resolve symbol name against main's .dynsym (link_context::from_self)
      store_word(image, r_offset, resolved_addr)
                     │
                     ▼
              .got.plt slot in the
              staging buffer (will be
              written to flash as-is)

Why pre-bind?  Static loader is r10-free for main.
  The PLT in flash reads .got.plt PC-relative from flash.
  Pre-binding writes the final address before flashing.
  No trampoline needed at runtime.
```

---

## 4. Runtime Load Lifecycle (every boot)

```text
┌─────────────────────────────────────────────────────────────────────┐
│  RUNTIME LOAD (xip-static-loader.cpp:91)                           │
│                                                                     │
│  fw::fs_blob(e) → span<const byte> pointing into FLASH_FS          │
│       │             (zero-copy, memory-mapped)                     │
│       ▼                                                             │
│  memory_byte_source src(blob)          ← non-owning span wrapper   │
│  elf_image::parse(src)                 ← reads 52-byte Ehdr only   │
│       ▼                                                             │
│  classify_segments(blob, seg)          ← reads Phdrs on demand     │
│  text_is_immutable(blob, seg)          ← reads Shdrs + relocs on   │
│       │                                   demand, stack-only       │
│       ▼                                                             │
│  Tighten data bounds from SHF_WRITE sections (Shdr loop)           │
│       ▼                                                             │
│  ┌──────────────────────────────────────────┐                      │
│  │  fw::image_arena().alloc(seg.data_memsz)  │  ← ONLY RW to RAM  │
│  │  ┌────────────────────────────────────┐   │                     │
│  │  │ .got + .data + .data.rel.ro       │   │  data_filesz bytes  │
│  │  │   (memcpy from flash)             │   │  copied from flash  │
│  │  ├────────────────────────────────────┤   │                     │
│  │  │ .bss (zero-filled)                │   │  (memsz - filesz)   │
│  │  ├────────────────────────────────────┤   │                     │
│  │  │ .init_array (ctor pointers)       │   │                     │
│  │  └────────────────────────────────────┘   │                     │
│  └──────────────────────────────────────────┘                      │
│       │                                                             │
│       ▼                                                             │
│  apply_rel_xip(seg, text_base, data_base,                           │
│                ram_data, .rel.dyn, .rel.plt,                        │
│                .dynsym, .dynstr, resolve)                           │
│       │           ← patches ONLY ram_data (RW segment)             │
│       ▼                                                             │
│  Run .init_array constructors  (r10 = got_base)                    │
│       │                                                             │
│       ▼                                                             │
│  Module ready: code in flash, data in RAM                           │
│                                                                     │
│  ... later: xip::stat::unload(m) → fw::image_arena().free(ram)     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 5. Runtime Memory Layout (static XIP, after load)

```text
FLASH_FS (0x08040000+):
┌──────────────────────────────────────┐
│  slib.so:                            │
│  ├─ 0x000000 ELF header (52 bytes)  │
│  ├─ 0x000100 .text (R-X, code)      │◄──── text_base
│  ├─ ...                              │       (PC executes here)
│  ├─ 0x000??0 .got.plt (R-X)         │       PLT reads GOT slots
│  ├─ ...                              │       PC-relative from flash
│  ├─ 0x008100 .got (RW init data)    │──┐
│  ├─ 0x008??0 .data (RW init data)   │  │  copied to RAM at load
│  └─ ...                              │  │
│                                      │  │
│  mylib1.so:                          │  │
│  ├─ ...                              │  │
│  └─ ...                              │  │
└──────────────────────────────────────┘  │
                                          │
RAM (0x20000000+):                        │
┌──────────────────────────────┐          │
│  firmware .data / .bss       │          │
│  ────────────                │          │
│  image_arena (64KB):         │          │
│  ├─ [free]                   │          │
│  ├─ slib.so RW segment       │◄─────────┘
│  │  ├─ .got                  │  data_base
│  │  ├─ .data                 │  got_base = data_base
│  │  ├─ .data.rel.ro          │           + (.got_vaddr - data_vaddr)
│  │  ├─ .init_array           │
│  │  └─ .bss (zeroed)         │
│  ├─ [free]                   │
│  └─ ...                      │
├──────────────────────────────┤
│  firmware HEAP (newlib sbrk) │  ← malloc/free from loaded lib
│  (_sheap .. _eheap)          │     uses this, NOT the module's
│                              │     data segment (no heap in RW)
└──────────────────────────────┘
```

---

## 6. Split-Base Relocation (apply_rel_xip)

```text
Only RW words are patched (r_offset must be within data_vaddr..data_memsz).

                                  ┌─────────────┐
                                  │  Rel entry   │
                                  │  r_offset ────┤────► ram_data[off]
                                  │  r_info      │       (RW in RAM)
                                  └──────┬───────┘
                                         │
                    ┌────────────────────┼────────────────────┐
                    │ type               │                    │
                    ▼                    ▼                    ▼
            R_ARM_RELATIVE         GLOB_DAT/JUMP_SLOT    ABS32/REL32/COPY
            ┌──────────────┐      ┌──────────────┐      ┌──────────────┐
            │ addend =     │      │ sym_target:   │      │ sym_target +
            │ load(ram)    │      │  st_val==0 →  │      │ existing val │
            │              │      │  resolve()    │      └──────────────┘
            │  in_text(    │      │  st_val!=0 →  │
            │   addend)?   │      │  in_text?     │
            │  ┌──┴──┐     │      │  ┌──┴──┐      │
            │ YES   NO    │      │ YES   NO      │
            │  │     │    │      │  │     │      │
            │  ▼     ▼    │      │  ▼     ▼      │
            │text   data  │      │text   data    │
            │bias   bias  │      │base   base    │
            └──────────────┘      └──────────────┘

  Bias = text_base - text_vaddr   (for addend/symbol in R-X)
       = data_base - data_vaddr   (for addend/symbol in RW)
```

---

## 7. Copy-Whole Fallback Path

When XIP is not possible (fused RWX segment, or relocation targets R-X):

```text
├────────────────────────────────────────────────────────────────┤
│  copy-whole (adapter.cpp:86-257 in nolib, same pattern)        │
│                                                                │
│  fw::image_arena().alloc(total)     ← ENTIRE image in RAM     │
│       │                                                       │
│       ▼                                                       │
│  for each PT_LOAD: memcpy(ram + (vaddr - min_va),             │
│                          flash_data + offset, filesz)          │
│       │                                                       │
│       ▼                                                       │
│  base = &ram; text_base = base + text_vaddr - min_va          │
│  data_base = base + data_vaddr - min_va                       │
│       │                                                       │
│       ▼                                                       │
│  apply_rel_xip (ALL relocs this time,                          │
│                  including text-range ones,                    │
│                  with r_offset - min_va mapping)               │
│       │                                                       │
│       ▼                                                       │
│  Patch JUMP_SLOTs → &xip_cross_call trampoline                │
│  (flash-resident PLT reads RAM GOT PC-relative,               │
│   needs deferred r10-swap via cross-call)                     │
│                                                                │
│  Note: text and data share ONE RAM allocation.                │
│  All of code executes from RAM (not flash).                   │
└────────────────────────────────────────────────────────────────┘
```

---

## 8. Variant Comparison

### 8.1 Provision Strategy

| Variant | Whole-file staging buffer? | Pre-binding? |
|---------|---------------------------|--------------|
| **xip-dynamic-nolib** | No (fixtures pre-flashed, or provisioned by host tool) | N/A — uses xip_cross_call trampoline at runtime |
| **xip-dynamic-newlib** | No (fixtures pre-flashed, or provisioned by host tool) | N/A — uses xip_cross_call trampoline at runtime |
| **xip-static** | Yes, `main.cpp:130` alloc(sz) | `bind_host_imports()` writes resolved JUMP_SLOT addresses into staging buffer before flash |

### 8.2 Runtime RAM Allocation

| Variant | RAM allocation | Text location | Import resolution |
|---------|---------------|---------------|-------------------|
| **xip-dynamic-nolib** | `data_memsz` only (adapter.cpp:286) | Flash | `xip_cross_lookup()` trampoline (call-time r10 swap) |
| **xip-dynamic-newlib** | `data_memsz` only (adapter.cpp) | Flash | `xip_cross_lookup()` trampoline (call-time r10 swap) |
| **xip-static** | `data_memsz` only (adapter.cpp) | Flash | Pre-bound GOT slots in flash (no trampoline for host imports) |
| **copy-whole fallback** | `total` (whole image, adapter.cpp:101 or nolib diopen) | RAM (copied) | xip_cross_call trampoline |

### 8.3 Implementation Status

| Variant | File(s) | Status | Lines of code |
|---------|---------|--------|---------------|
| **xip-dynamic-nolib** | `src/xip/dynamic-nolib/adapter.cpp` + `src/xip/adapter-base.hpp` | Hardware-verified (WeAct BlackPill STM32F411CE) | 391 + 94 |
| **xip-dynamic-newlib** | `src/xip/dynamic-newlib/adapter.cpp` + `src/xip/adapter-base.hpp` | Hardware-verified (WeAct BlackPill STM32F411CE) | 436 + 94 |
| **xip-static** | `src/xip/static/adapter.cpp` + `src/xip/adapter-base.hpp` | Hardware-verified (WeAct BlackPill STM32F411CE) | 407 + 94 |
| **Shared primitives** | `src/xip/{segment-map,relocate,compact}.cpp` + `src/xip/adapter-base.hpp` + `src/xip/xip-cross-call.c` | Done, reusable | ~393 total |

---

## 9. Firmware Link: No `-llib` for Libraries

The firmware is linked without any `-lslib -lmylib1`. Libraries are standalone `.so` files
compiled separately with `xip-split.ld`, provisioned to FLASH_FS, and loaded at runtime.

```text
Firmware link (CMakeLists.txt:95-100):     Library build (build-xip-libs.sh):
┌──────────────────────────────┐           ┌──────────────────────────────┐
│ scl-fw-xip-static            │           │ slib.so                     │
│  ├─ main.cpp                 │           │  ├─ lib.c                  │
│  ├─ xip-static-loader.cpp    │  ← NO     │  └─ xip-split.ld link      │
│  ├─ platform (HAL, newlib)   │  ──────   └──────────────────────────────┘
│  ├─ scl::xip-common          │   link            │
│  └─ libsupc++ / libgcc       │    to libs        │ provision to FLASH_FS
└──────────────────────────────┘                   ▼
                                             xip::stat::load(flash_blob, ...)
```

The firmware only needs:
- **Headers** from the libraries (function signatures for the calling code)
- A **symbol registry** so the loader can resolve library imports

---

## 10. Symbol Resolution: Two Approaches

### 10.1 Former: `g_host[]` static registry (removed 2026-07-24 — see §10.4)

> **Historical.** Static XIP originally used the explicit table below. It has since
> been replaced by `link_context::from_self()` (§10.2), so **both** XIP loaders now
> resolve against main's `.dynsym`. Kept here to show the before/after.

```cpp
// (former) main.cpp
extern const xip::stat::HostSym g_host[] = {
    { "printf", reinterpret_cast<uint32_t>(&printf) },
    { "malloc", reinterpret_cast<uint32_t>(&malloc) },
    { "AA",     reinterpret_cast<uint32_t>(&AA) },
    // ... every symbol the lib imports must be listed here
};
```

```text
Pros:  Simple, explicit, no ELF metadata needed in firmware
Cons:  Manual maintenance — every new library import must be added
       Linear scan at load time
       Firmware author must know exactly what each lib imports
```

### 10.2 Current (both loaders): `link_context::from_self()`

Used by the nolib XIP adapter (`adapter.cpp:312`) and, since 2026-07-24, the static
XIP loader as well:

```cpp
auto linkr = fw::link_context::from_self();  // parses firmware's _DYNAMIC
```

Works because the firmware is linked `-pie -E` which embeds `.dynsym`/`.dynstr`/`.hash`
in the firmware binary. The loader walks the ELF hash table (O(1) average) instead
of a linear array.

```text
Pros:  No g_host[] — all exported symbols auto-discovered
       ELF hash walk (fast, reuse loader::symbol_resolver)
       No maintenance when adding new imports
       Also eliminates all -u,<sym> flags

Cons:  Requires -pie -E (already used for both XIP variants)
       Firmware symbol table must not be stripped
```

### 10.3 Comparison

| Aspect | `g_host[]` | `link_context::from_self()` |
|--------|-----------|---------------------------|
| Maintenance | Add entry per imported symbol | None (auto) |
| Resolution | Linear scan O(n) | Hash walk O(1) avg |
| Firmware link | Needs `-E` + `-u` per symbol | Needs `-E`; plus one `[[used,retain]]` anchor for borrowed libc/libsupc++ symbols main never calls itself (see §10.4) |
| Stripping safe? | Yes (table is explicit) | No (.dynsym must survive) |
| Used by | (former) static XIP | **both** static and nolib XIP (current) |

### 10.4 How `g_host[]` was eliminated from static XIP

Changes needed in `xip-static-loader.cpp`:

```diff
- struct host_resolver { ... };
+ #include "scl/link-context.hpp"
  ...
- host_resolver hr{ host };
- auto resolve_src = hr.source();
+ auto linkr = fw::link_context::from_self();
+ main_resolver mr{ &*linkr };
+ auto resolve_src = mr.source();
```

And in `CMakeLists.txt`:

```diff
- LINKER:-u,printf
- LINKER:-u,malloc
- LINKER:-u,AA
- ... (all 20+ -u flags removed)
+ # -u flags no longer needed; -E keeps .dynsym
```

And remove `g_host[]` from `main.cpp` (`bind_host_imports()` stays — it still
patches the flash `JUMP_SLOT`s at provision — but it now resolves each name via
`link_context::from_self()` instead of the table).

#### Implemented 2026-07-24 — hardware-verified, with one caveat

Done and verified on all 11 fixtures (WeAct BlackPill STM32F411CE): resolution is now 100 %
against main's `.dynsym`, `g_host[]` is gone, and the ~28 `-u` flags are gone.
**But "all `-u` removed / `-E` keeps everything" is only half true**, and the
build proves it:

- Symbols main **defines** (`AA BB f w s1 HOST_K host_f fm_* trace_*`) do land in
  `.dynsym` on `-E` alone — no `-u`, no reference needed. ✓
- Symbols the libraries **borrow but main never calls** (`printf`, `_Znwj` =
  operator new, the C++ EH runtime, `sinf/…`) live in `libc.a`/`libsupc++.a` and
  are pulled from an archive **only when referenced**. With neither `-u` nor a
  reference they are never linked in, so `-E` has nothing to export and the
  libraries can't resolve them.

`-E` controls *export*, `--gc-sections`/archive selection controls *inclusion* —
they are orthogonal (see §11). So the `-u` pile is replaced not by nothing but by
**one linkage anchor** in `main.cpp` — a `[[gnu::used, gnu::retain]]` array that
takes the address of exactly those borrowed symbols, forcing them into the link so
`-E` can export them. It is *not* a resolver (the loader never reads it). Net:
no `-u`, no `g_host` resolver table, one small honest anchor for borrowed libc/
libsupc++ symbols.

---

## 11. `--gc-sections` vs `-u` Flags

Two orthogonal concerns that are often confused:

```text
┌───────────────────────────────────────────┬──────────────────────────────────────┐
│  --gc-sections                            │  -u,<sym>                           │
├───────────────────────────────────────────┼──────────────────────────────────────┤
│  "Drop unreferenced sections from output" │  "Pretend this symbol is referenced  │
│                                           │   so GC keeps its section"           │
├───────────────────────────────────────────┼──────────────────────────────────────┤
│  WITHOUT --gc-sections:                   │  Without -u (with --gc-sections):    │
│    all sections kept, binary is larger    │    symbol dropped from output        │
│    no -u flags needed                     │    lib can't find it at runtime      │
├───────────────────────────────────────────┼──────────────────────────────────────┤
│  WITH --gc-sections + -u:                │  Without --gc-sections:               │
│    only used sections + specifically      │    -u flags are no-ops               │
│    forced symbols survive                 │    (all symbols kept anyway)          │
└───────────────────────────────────────────┴──────────────────────────────────────┘
```

Trade-off:

```text
--gc-sections + -u flags:        no --gc-sections:
┌────────────────────────┐       ┌────────────────────────┐
│ firmware .text         │       │ firmware .text         │
│  - used functions      │       │  - used functions      │
│  - -u forced symbols   │       │  - ALL functions       │← kept
├────────────────────────┤       ├────────────────────────┤
│ flash size = minimal   │       │ flash size = larger    │
│      (pay for what     │       │      (pay for           │
│       you use)         │       │       everything)       │
└────────────────────────┘       └────────────────────────┘
```

If using `link_context::from_self()` + `-E`, the `-u` flags are unnecessary because
`-E` already forces all `STB_GLOBAL` symbols into `.dynsym`, which `--gc-sections`
respects (exported symbols are considered referenced).

---

## 12. Streaming Provision Optimization

Current: `alloc(sz)` for entire .so → patch → flash_program → free.

Optimized (eliminates full-file staging buffer):

```text
                 ┌──────────────┐
  semihost src ──►  stream read │
                 │  in blocks   │
                 └──────┬───────┘
                        │
          ┌─────────────┼──────────────┐
          ▼             ▼              ▼
    Parse Ehdr     Accumulate      block buffer
    + Phdrs        CRC-32          (e.g. 256 bytes)
    + Shdrs                         (heap: 0)
    (heap: 0)       (heap: 0)
          │             │              │
          ▼             ▼              ▼
    Collect JUMP_SLOT fixups     For each block:
    into small array              if block contains
    ├─ r_offset: addr             a GOT slot needing
    ├─ resolved_addr              patching:
    └─ (heap: small,              store_word(block, offset,
      ~N_JUMP_SLOT * 8)             resolved_addr)
                                  flash_program(block)
                                    │
                                    ▼
                              FLASH_FS

  Peak RAM = max(Ehdr/Phdr/Shdr scratch,
                 fixup array (~200 bytes),
                 block buffer (256 bytes))
           ≈ ~1-2 KB (vs 20-50 KB currently)
```

Trade-offs:
- More complex (two-pass: scan then stream-write with patches)
- Requires keeping fixup list in memory during streaming
- Fixup list size = number of external JUMP_SLOTs × 8 bytes (typically < 200 bytes)
- Not needed for runtime — only affects provision tooling

---

## 13. Summary

```text
┌─────────────────────────────────────────────────────────────────┐
│  RUNTIME: Only RW segment in RAM  ✓ already done (both nolib    │
│           and static XIP loaders)                                │
│                                                                  │
│  PROVISION: Whole-file staging buffer in arena                   │
│             (transient, per-file, freed immediately)             │
│                                                                  │
│  OPTIMIZATION PATH 1: Stream provision to eliminate              │
│                       staging buffer (provision tooling only)    │
│                                                                  │
│  OPTIMIZATION PATH 2: Replace g_host[] with                     │
│                       link_context::from_self()                  │
│                       (remove -u flags, remove manual registry)  │
└─────────────────────────────────────────────────────────────────┘
```

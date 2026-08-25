# XIP/Static without r10 — the `-fno-pic` fixed-address loader

`test-xip/test-xip-static-no-pic` demonstrates a second way to do **execute-in-place
static loading** on the SCL static loader family: instead of building the module as
PIC (`-fpic -msingle-pic-base -mpic-register=r10`) and paying the r10 tax on every
call from main, build it **non-PIC** (`-fno-pic -mlong-calls`) and **link it at fixed
addresses**. The result is the *simplest possible* static-loader model:

```
main → module    an ordinary C++ function-pointer call.
                 no pic_call_*, no got_base, no `register void* r10_val`.
module → main    an ordinary indirect call (the R_ARM_ABS32 slot is patched at load).
module → module  an ordinary indirect call (the slot is patched to the other module).
```

There is no GOT, no PLT, no PIC base register, and no trampoline anywhere.

---

## 1. Why the r10 model is complicated

The existing XIP-static flavour (`scl::xip-static`, `test-xip-static-direct`) builds
its fixtures as PIC shared objects:

```
-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative
```

Under that codegen, a module function reaches **its own globals** through
`ldr r3, [r10, #off]` — r10 is the module's GOT base, which lives in RAM at an
address the loader chose at runtime. Whoever enters the module must load r10 first.
That forces, on the caller side:

```cpp
// test-xip/test-xip-static-direct — the r10 tax
register void* r10_val asm("r10");            // global register variable
r10_val = reinterpret_cast<void*>(m.got_base); // set r10 before a batch of calls
auto r = pic_call_u32(fn, m.got_base, 39);     // ... or a per-call trampoline
```

plus a provision-time binding pass (`bind_host_imports`) that patches the
**flash-resident** GOT before the image is programmed (flash is read-only at run
time), and a cross-call trampoline (`xip_cross_call`) for module→module calls.

None of that is *required* by "static loading". It is required by **PIC code whose
data base is chosen at runtime**.

---

## 2. The alternative: fixed-address, non-PIC modules

Build the module instead as a **static shared object**:

```
-fno-pic -mlong-calls            # no GOT/PLT/r10; absolute addresses
-shared -nostartfiles
-Wl,-E                           # export globals to .dynsym
-Wl,--unresolved-symbols=ignore-all
-T nopic-split.ld                # places text/data at FIXED VAs
```

Because the module is non-PIC, every intra-module reference is an **absolute
address** the linker computed at link time. The module is linked into a reserved
range of the firmware's RAM (the `_sxip_text_*` / `_sxip_data_*` windows), so those
absolute addresses are correct at run time **with a 0 load bias**:

| Symbol | Address | Holds |
|---|---|---|
| `_sxip_text_start` … `_sxip_text_end` | `0x20018000` … `0x2001C000` | module R-X segments |
| `_sxip_data_start` … `_sxip_data_end` | `0x2001C000` … `0x2001D000` | module RW segments |

Each module is linked at its **own** sub-slot inside these windows, so several
modules can be resident at once and call each other — the loader copies each
module to *its own* link-time VA. Neither the window bases nor the slots are
hand-picked:
- the **window bases** are decided once in the app linker script (they are the
  heap↔module-window boundary in the 128K SRAM map) and are the single source
  of truth;
- `fixtures-src/build.sh` **reads those bases straight out of the app linker
  script**, builds each module once at a placeholder base, measures its two
  `PT_LOAD` sizes with `readelf`, then **packs every module contiguously**
  inside the windows and re-links with the real addresses. Overlap is impossible
  by construction, and an overflow of the windows fails the build.

### What the linker leaves behind

Two relocation kinds appear in a `-fno-pic -shared` ARM object:

| Relocation | Meaning | What the loader does |
|---|---|---|
| `R_ARM_RELATIVE` (23) | local absolute pointer, **addend already written into the word** | nothing — the value is correct because we load at the link VAs (0 bias) |
| `R_ARM_ABS32` (2) | `value = S + A`; the word holds the addend (0 for a literal slot), **not** the resolved value | write `S + A` at load: defined-in-module → `st_value`; undefined → `resolve()` against loaded modules, then main |

That single rule is the whole loader. There is no lazy binding, no journal, no
revert — a "load" is a memcpy + bss-zero + patch + `.init_array`.

### Why `-mlong-calls`

On an STM32F411 the `bl` range is ±16 MB; flash (`0x0800_0000`) and RAM
(`0x2000_0000`) are ~400 MB apart. `-mlong-calls` makes every cross-space call go
through a literal-pool + `blx` (a veneer-safe indirect call), which is exactly what
the loader's patched `R_ARM_ABS32` slots already are.

---

## 3. The loader — `xip::nopic`

`include/xip/static/nopic.hpp` + `src/xip/static/nopic.cpp` (INTERFACE library,
compiled into the firmware like every other SCL source):

```cpp
namespace xip::nopic {
struct PModule {
    bool           ok;
    std::uintptr_t text_base, data_base;   // == the module's link-time VAs
    std::uint32_t  text_vaddr, text_size;
    std::uint32_t  data_vaddr, data_memsz;
    loader::symbol_resolver resolver;      // over the blob's .dynsym/.dynstr/.hash
};

bool load(std::span<const std::byte> blob, PModule& out) noexcept;
void* resolve(const PModule& m, const char* name) noexcept;  // st_value (0 bias)
void* resolve_any(const char* name) noexcept;                // modules, then main
void  resolve_all() noexcept;    // re-resolve cyclic module<->module imports
void  patch_main(const PModule& m) noexcept;  // fill MAIN's GOT for BY-NAME calls
void  unload(PModule& m) noexcept;
}
```

`load()`:

1. **Parse** the ELF (the shared `common-elf-reader`), find the two `PT_LOAD`
   segments (R-X and RW) of the split image.
2. **Range-check** — text must land inside `[_sxip_text_start, _sxip_text_end)`,
   data inside `[_sxip_data_start, _sxip_data_end)`.
3. **Copy** the R-X file image to `text_vaddr` and the RW image to `data_vaddr`
   (0 bias), zero the RW bss.
4. **Patch** every `R_ARM_ABS32`: defined symbol → `st_value + addend`; undefined →
   `resolve_any(name) + addend`. `R_ARM_RELATIVE` is left alone.
5. **Register** the module (so later modules can import from it) and run
   `.init_array` from the data window.
6. **`resolve()`** returns the symbol's `st_value` directly — because the module
   runs at its link-time address, the symbol value *is* the runtime address.

### By-name calls from main (`patch_main`)

The loader can also let main call the exports **directly by name**, exactly like
the r10 "direct" flavour — but with no r10 at all:

1. main declares the exports (`extern "C" unsigned s_add(unsigned, unsigned);`)
   and **links the fixtures** (`-l:slib.so …`). Because main is compiled `-fpic`
   and the app links as an EXEC (no `.dynamic`), the linker emits `R_ARM_GLOB_DAT`
   / `R_ARM_JUMP_SLOT` slots in **main's own GOT/PLT**, left at 0.
2. After `load()` + `resolve_all()`, the app calls `xip::nopic::patch_main(m)`
   for each loaded module. It walks main's `.rel.dyn` (exposed by the app linker
   script as `__rel_dyn_start/_end`, `__dynsym_*`, `__dynstr_*`) and, for every
   slot whose symbol that module exports, writes the module's fixed address into
   main's GOT.
3. main then calls `s_add(10, 5)`, `cpp3_method(7, 5)`, reads `s_a`, `s_info.tag`
   … — plain calls, no casts, no `sym()`, no r10.

`test-xip/test-xip-static-no-pic-simple` demonstrates this (its `main.cpp` is all
by-name). Two caveats:
- The exception-enabled fixtures (`libcppeh`, `ehx_catch`) cannot be linked into
  main — that pulls libgcc's hidden `__aeabi_unwind_cpp_pr1` through `-Wl,-E`
  ("hidden symbol referenced by DSO") — so those two are still reached via
  `resolve()` + a cast (and their throwing path needs module `.ARM.exidx`, so it
  stays skipped anyway).
- `patch_main` only touches slots whose symbol the module **defines**; unrelated
  slots (main's own externals like `printf`) are left for the normal boot path.

#### Who calls directly by name (plain English)

"Direct calls" means main writes `s_add(10, 5)` like an ordinary function and the
loader silently points it at the library — no lookup, no cast, no trampoline. The
test folders that do this:

| Folder | Loader | Direct by name? | Extra machinery |
|---|---|---|---|
| `test-static/test-static` (f411ce-sh) | `scl::static` | ✅ yes | main links libs; loader patches main's GOT (`_reloc` table) |
| `test-static/test-static-direct` | `scl::static` | ✅ yes | same |
| `test-xip/test-xip-static-direct` | `scl::xip-static` (r10) | ✅ yes | must set `r10_val` first |
| `test-xip/test-xip-interrupt-static-direct` | r10 XIP | ✅ yes | same r10 trick |
| `test-xip/test-xip-static-no-pic-simple` | `scl::xip::nopic` | ✅ yes | **none** — no r10, no trampoline |

Folders that do **not** direct-call (they look the symbol up and cast it, or use a
trampoline):

| Folder | What they do instead |
|---|---|
| `test-xip/test-xip-static/semihosting` | `sym()` + `pic_call_*` (r10 trampoline) |
| `test-xip/test-xip-static-no-pic` | `sym()` + cast to a function pointer |
| `test-xip/test-xip-interrupt-static` | same, via trampoline |
| `test-static/test-drv` | `scl::Module` shim + `sym()` |

The `scl::Module` shim is only a load/unload/sym convenience — it is never needed
for the call itself. Drop it and keep a raw `PModule` on the stack; the direct
calls are unchanged.

### Imports from main

`resolve_any()` falls back to `fw::link_context::from_self()`, which reads main's
own `.dynsym`/`.dynstr`/`.hash` (`main` is linked `-pie -Wl,-E`). So a module
calling `host_f` or reading `HOST_K` gets the real firmware address written into
its RAM text slot at load — **no provision-time binding**, because the text window
is RAM and can be patched after the image is stored.

### Why a RAM text window (and not flash)?

A non-PIC module can only run at its link-time VA. For true *execute-from-flash*
XIP the link-time text VA must be a flash address and the bytes must be
**programmed** there at provision time (the r10 model's `bind_host_imports` is
precisely the "flash is immutable" workaround). Under QEMU the emulated flash is
read-only, so a flash-resident text slot cannot be demonstrated on every board.

The no-pic loader therefore keeps the text window in **RAM** and copies the R-X
segment there at load. This keeps the demo identical on `weact`, `nucleo` and
`qemu`, at the cost of the "R-X stays in flash" property. If you have real flash
you can point `_sxip_text_start` at a dedicated flash sector instead: the loader is
unchanged; you would add a provision-time `flash_erase_sector` +
`flash_program` of the R-X segment and skip the load-time text copy.

---

## 4. The test app

`test-xip/test-xip-static-no-pic/` is a hosted firmware (same platform as
`test-xip-static-direct`) with its own fixtures, linker scripts, CMake and runner.

### Fixtures (`fixtures-src/`)

Sub-slot addresses are **packed automatically** by `build.sh` (measure →
pack → re-link); the table shows what each fixture exercises:

| Fixture | Exercises |
|---|---|
| `nslib.so` | full surface: `.data`/`.bss`, constructor, strings, struct global, intra-lib calls, function-pointer call, imports `HOST_K`/`host_f`/`printf` from main |
| `nmylib1.so` | constructor effects + imports; loaded/unloaded/reloaded to prove a clean data window |
| `nprov.so` | the cross-module **provider** |
| `ncons.so` | the cross-module **consumer** (`ncons` calls `nprov.p_fn`) |

`build.sh` templates `ldscripts/nopic-split.ld` (`__TEXT_BASE__`/`__DATA_BASE__`)
per module and links `ncons` against `-l:nprov.so`, so `p_fn` is a known dynamic
symbol whose `R_ARM_ABS32` slot the loader fills when `ncons` loads after `nprov`.

### The call sites in `main.cpp` — the whole point

```cpp
// test-xip/test-xip-static-no-pic/src/main.cpp
auto s_add = reinterpret_cast<unsigned (*)(unsigned, unsigned)>(m.sym("s_add"));
unsigned r = s_add(10, 5);          // plain call, no r10
```

No trampoline, no register variable, no `got_base`. The `call<>` helper in
`test-helpers.hpp` is nothing but a cast + `operator()`.

### Running it

```sh
# all boards — same binary semantics (RAM text window)
test-xip/build-and-run.sh qemu   static-no-pic
test-xip/build-and-run.sh weact static-no-pic
test-xip/build-and-run.sh nucleo static-no-pic
```

Expected QEMU tail (the exact lines are also the `--done` marker):

```
scl-fw-xip-static-no-pic: fixed-address static loader, NO r10
...
  ncons: c_fn(10)         = 38   (p_fn(10)+1)
  ncons: c_double(10)     = 118   (p_fn(p_fn(10)))
  cross-module call ran through a plain indirect call
scl-fw-xip-static-no-pic: done
```

The firmware (`scl-fw-xip-static-no-pic`) reserves the two windows in
`src/sections-flatfs-hosted-nopic.ld` (board) and
`src/sections-flatfs-qemu-nopic.ld` (QEMU), asserts `bss + arena` stay below the
text window, and leaves the heap between the arena and `_sxip_text_start`.

### `test-xip-static-no-pic-simple` — the by-name variant

A second app, `test-xip/test-xip-static-no-pic-simple/`, demonstrates the same
loader with **calls by name** (see "By-name calls from main" above): main links
the shared fixtures (`-l:slib.so -l:libprov.so -l:libcons.so -l:libcppsh.so
-l:cpp3.so -l:c2.so -l:c1.so`), declares their exports as plain `extern`, and
calls `s_add(10, 5)`, `cpp3_method(7, 5)`, reads `s_a`, `c2_data`, … — no
`sym()`, no casts. It loads the full set (`slib`, `libprov`/`libcons`, `libcppsh`,
`cpp3`/`c2`/`c1` incl. the cyclic cross-module cluster, plus `libcppeh`/`ehx_catch`
via `resolve()`), then `resolve_all()` + `patch_main()` for each module, then
prints every result.

```sh
test-xip/build-and-run.sh qemu   static-no-pic-simple
test-xip/build-and-run.sh weact static-no-pic-simple
```

---

## 5. Trade-offs vs. the r10 XIP model

| | `scl::xip-static` (r10) | `scl::xip::nopic` |
|---|---|---|
| Module codegen | `-fpic -msingle-pic-base -mpic-register=r10` | `-fno-pic -mlong-calls` |
| GOT / PLT | yes (RAM GOT, flash PLT) | none |
| Calling in from main | `pic_call_*` or `r10_val` + direct | plain function pointer, or **by name** via `patch_main` (no r10) |
| Module→main | flash PLT pre-linked at provision | RAM `R_ARM_ABS32` patched at load |
| Module→module | `xip_cross_call` trampoline (r10 swap) | plain indirect call |
| Provision-time binding | required (`bind_host_imports`, flash) | not needed (RAM windows) |
| Runtime relocation | GOT/data, journaled unload | `R_ARM_ABS32` slots, nothing to revert |
| Text location | executes from flash (XIP) | copied to a fixed RAM sub-slot |
| Module residency | runtime arena slots, multi-instance | fixed sub-slots; no two modules may share a VA |
| RAM cost | code stays in flash, only RW in RAM | code + RW both in RAM windows |
| Complexity to call | high | minimal |

**When to prefer no-pic:** when the pain of r10 plumbing outweighs saving a few KB
of RAM — small systems with a few fixed modules, where the linker-time address
plan is acceptable, and where every board (including QEMU) must behave identically.

---

## 6. Extending to real flash XIP (boards with writable flash)

Keep everything, but:

1. Move `_sxip_text_start/_end` to a dedicated, otherwise-unused flash sector
   (e.g. sector 5 on the F411, `0x08030000`), outside the firmware image and the
   flat_fs region.
2. At provision time, after building the image in RAM: `flash_erase_sector(5)`,
   `flash_program(0x08030000, blob + text_off, text_filesz)`.
3. At load, skip the text memcpy (text is already in flash); the R-X segment is
   then genuinely execute-in-place and only the RW window costs RAM.

The loader's contract (range-checked fixed VAs, 0-bias, `R_ARM_ABS32` patching)
is unchanged — it does not care whether the text window is flash or RAM.

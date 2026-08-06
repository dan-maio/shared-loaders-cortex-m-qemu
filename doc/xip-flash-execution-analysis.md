# Executing `.so` Code from Flash (XIP) — Feasibility Analysis

## Scope

For the ET_DYN loaders in `test/{dynamic-nolib, dynamic-newlib, static}`, where the
`.so` are stored in a memory-mapped **flash / filesystem** (no semihosting), can we —
after a modified symbol-resolution/patching scheme — **execute code directly from
flash** instead of copying the whole shared object into RAM? I.e. load only the
writable parts into RAM and leave code (and read-only data) resident on flash,
executed in place (XIP).

This document analyses feasibility, the required changes, where those changes live
(shared vs per-loader), the quantified RAM payoff, and the caveats. It is grounded
in the current sources; file:line anchors are given throughout.

## Verdict

**Feasible — and now implemented and hardware-verified for the nolib loader.** Two
single-pic-base libraries execute their full exported surface from flash on the
WeAct BlackPill STM32F411CE, with only the RW segment in RAM. See **§Implementation** below for what
was built and the causes/fixes found on the way (several were not visible from the
static analysis). For code-heavy modules the runtime-RAM saving is ~85–96 %, and it
makes the dynamic loader the **lightest** RAM option in the repo — overturning the
ranking in `test-relocatable/doc/architecture-reloc-manual.md` §12.5.

> The rest of this document is the original feasibility analysis (blockers,
> payoff, toolchain options). It remains accurate; §Implementation records how the
> plan was actually carried out and where reality was subtler than the analysis.

## Why it is close: three enabling facts already present

1. **The `.so` are built `-fpic -msingle-pic-base -mpic-register=r10`**
   (`test/fixtures-src`, build flags). This is decisive. Plain ARM PIC computes the
   GOT address *PC-relative* — it assumes text and GOT sit at a fixed displacement
   within one contiguous mapping, which is exactly what makes flash-text + RAM-data
   impossible. `-msingle-pic-base` removes that assumption: code reaches all globals
   through **r10**, so **text and data are already decoupled** and may live at
   independent addresses (flash vs RAM).
2. **The loader already manages `r10 = got_base` per cross-library call**
   (`src/port/dl-cross-call.c`, `src/dynamic-nolib/dl.cpp:313`). The runtime
   "data-base swap" that XIP needs is already implemented — XIP just points that
   base at RAM while text stays in flash.
3. **ET_DYN + PIC means text needs no load-time patching in principle.** All fixups
   target the GOT/data, never the instruction stream — the opposite of the ET_REL
   reloc loader, which bakes `R_ARM_ABS32` *into* `.text` and therefore can **never**
   be XIP'd.

## The three blockers (all fixable; none is a redesign)

### Blocker A — segment layout

Program-header dump of the shipped fixtures (`test/fixtures/*.so`):

| `.so` | PT_LOAD layout | relocs into the exec segment |
| --- | --- | ---: |
| `c1`, `libprov`, `mylib1`, `mylib2`, `libcons` | **single `RWE` segment** (code+data+relocs fused, align 4) | all |
| `cpp3` | **split: `R E` 2260 B + `RW` 248 B** (align 0x1000) | **0** |

Most fixtures are deliberately linked as **one `RWX` blob** for RAM compactness (the
"Memory Compaction" advantage). That is precisely what defeats XIP: code and writable
data are fused, and relocations sit inside the executable segment. **`cpp3` is the
proof of concept** — linked with a normal two-segment layout, its executable segment
takes **zero** relocations, so those 2,260 B are immutable and can execute straight
from flash.

**Fix:** relink the XIP-candidate libraries with a split `R-X | RW` PHDRS layout
(as `cpp3` already has). This is a linker-script change to the fixtures, not loader
work.

### Blocker B — the loader copies the whole image

Today each loader allocates one RAM buffer spanning the whole image and copies every
`PT_LOAD` into it:

* nolib — `src/dynamic-nolib/dl.cpp:210` ("Copy PT_LOAD segments to their vaddr offsets")
* newlib — `src/dynamic-newlib/image-memory.cpp:28`
* static — `src/static/elf-reader.cpp:303`

**Fix:** for the R-X segment keep a `std::span` into its flash address and execute in
place (flash is already memory-mapped — the `flatfs` demos already load modules from a
flash span); allocate RAM only for the RW segment; copy + relocate only that.

### Blocker C — how the `.so` is stored on the flash filesystem

Blocker B assumes "the R-X segment is a flash span we can execute from." That is only
true if the filesystem stores the module the right way. XIP turns the R-X segment's
**on-flash byte offset into a program-counter address** — `text_base = archive_base +
entry_offset + p_offset` — so the *storage layout*, not just the ELF layout, becomes
part of the contract. Two things the current packer (`test-relocatable/tools/pack-flatfs.py`)
does not guarantee:

**1. Alignment — the FS honours 8 B, the linker asked for `p_align`.** The packer
aligns each entry to **8 bytes** (`align8`) and marks the archive `aligned(8)`. But
`cpp3.so`'s R-X segment declares `p_align = 0x1000`:

```
LOAD  Offset 0x000000  VirtAddr 0x0000  FileSiz 0x8d4  R E  Align 0x1000
LOAD  Offset 0x0008d4  VirtAddr 0x18d4  FileSiz 0x0f0  RW   Align 0x1000
```

On this MMU-less Cortex-M4, 8 B is *enough for execution correctness* — Thumb-2 fetch
needs only 2 B — so it happens to run. But the declared 0x1000 is silently dropped, and
that alignment stops being cosmetic the moment you (a) want an **MPU region** around the
R-X range to enforce no-write (bigger part, or a hardening pass), or (b) load a library
whose code/rodata alignment is load-bearing. A XIP-aware packer must align each module's
entry to **`max(p_align)` of its segments**, not a fixed 8 B.

**2. Compaction / compression breaks the offset→address mapping.** A production flash
filesystem normally **compacts** stored files — concatenates them back-to-back,
compresses, or relayouts on GC. Any of those destroys the `p_offset → flash address`
relationship XIP depends on: compressed bytes are not executable, and a relayout that
does not preserve the R-X segment's alignment moves it off a usable boundary. `flatfs`
is XIP-safe today only by accident — it stores each `.so` **raw and contiguous**. For
XIP that accident must become a **guarantee**: XIP modules are stored uncompressed,
contiguous, and byte-identical to the linked image.

**Fix — a XIP-aware pack step / FS contract.** Teach `pack-flatfs.py` (and any real FS)
to, for each XIP module: align the entry to the module's R-X `p_align`, store it
uncompressed and contiguous, and record the R-X segment's flash offset for the loader.
Optionally **strip the non-alloc tail** (`.symtab`/`.strtab`/section headers/`.debug*`)
to reclaim the flash the alignment padding costs — the loader only needs the program
headers plus the two LOAD segments. This is a **build-tool + FS-contract change**, the
storage-side peer of Blocker A (linker) and Blocker B (loader); like them it needs no
per-library or per-app source change.

## Required symbol-resolution / patching changes

Move from **one load bias** to **two bases per module**:

* `text_base` = flash address of the R-X segment (where the `.so` sits in the FS/flash),
* `data_base` (= r10 / `got_base`) = RAM address of the writable copy.

The relocator then changes as follows:

| Reloc | Target location | Change |
| --- | --- | --- |
| `R_ARM_JUMP_SLOT`, `R_ARM_GLOB_DAT` | GOT (RW → **RAM**) | unchanged mechanics; slot in RAM, value may be a flash text addr — fine |
| `R_ARM_ABS32` (in data) | `.data` / `.data.rel.ro` (RW → **RAM**) | write `S + A` into RAM |
| `R_ARM_RELATIVE` | GOT or `.data.rel.ro` (RW → **RAM**) | **the subtle one** — see below |
| `R_ARM_COPY` | main's `.bss` | unaffected (already main-side RAM) |
| *(any reloc whose `r_offset` ∈ R-X range)* | — | **must not exist** — assert it; `cpp3` confirms 0 |

The one genuinely new piece is **`R_ARM_RELATIVE` with split bases**: `*P += bias`,
but the correct bias depends on whether the *addend* points into text or data. With a
single mapping there is one bias; with flash-text + RAM-data you must resolve each
addend against the segment map (is this vaddr in the R-X range or the RW range?) and
add `text_base` or `data_base` accordingly. This is bookkeeping, not difficulty — but
it silently corrupts pointers if skipped.

**Fail-safe:** before committing to XIP for a module, scan its relocation table and
confirm **no `r_offset` falls inside the R-X segment**. If any does (e.g. a badly
laid-out `.rodata` with embedded relocated pointers), fall back to copying that
module whole.

## Where the change lives: shared core + thin per-loader adapters

The three ET_DYN loaders are separate CMake targets (different memory models), but
they share lower layers:

| Layer | Shared? | Where |
| --- | --- | --- |
| ELF parsing (`program_header`, `image_size`) | **all three** | `common-elf-reader` |
| Relocation-**plan** computation | **nolib + newlib** | `src/relocator.cpp` (`scl-common`) |
| Relocation-plan for **static** | separate | `src/static/shlib-loader.cpp` (static links `common-elf-reader` directly, not `scl-common`) |
| PT_LOAD **copy** loop | **duplicated ×3** | `dl.cpp:210`, `dynamic-newlib/image-memory.cpp:28`, `static/elf-reader.cpp:303` |
| Relocation **apply** + base register | **per-loader** | nolib (r10 cross-call), newlib (patch-journal / revert), static (own patcher) |

So the XIP change is **layered**, not one monolithic edit and not three independent
rewrites:

* **Shared core — write once** (in `common-elf-reader` + `relocator.cpp`): segment
  classification (which PT_LOAD is R-X vs RW), the two-base address model, the
  split-base `R_ARM_RELATIVE` addend resolution, and the "no reloc targets R-X"
  safety scan. The plan-level part covers nolib + newlib automatically; static reuses
  the `common-elf-reader` classification helpers.
* **Per-loader plumbing — three small edits**: each loader's copy loop changes from
  "copy all → RAM" to "R-X = flash span, copy only RW → RAM," then feeds the two bases
  into its *existing* apply path and sets its data base (r10 for nolib). These stay
  separate because the memory models genuinely differ — you are not unifying the
  loaders, only teaching each to skip the R-X copy.

`scl-static` is the most independent (its own relocation path in `shlib-loader.cpp`),
so it duplicates the most of the per-loader work.

## Does it need per-library or per-app changes afterwards?

Once the one-time loader + shared-linker-script work is done, the answer is **no
source changes — ever — to the libraries or to the main/test app**, for any
well-behaved `-fpic` shared library. The precise boundaries:

**What never changes**

* **Library source code** — untouched. XIP is a build+load concern, not a source
  concern: no `#ifdef`, no annotations, nothing per-lib.
* **Main / test app source** — untouched. Main still calls `dlsym`-returned function
  pointers; the r10-swap trampoline already enters each lib with its `got_base`, and
  XIP only makes that `got_base` point at the RAM data segment. Main never sees where
  the code lives. `R_ARM_COPY` (main pulling a lib's data into its own `.bss`) resolves
  exactly as before.
* **The data / instance model** — unchanged. Each `dlopen` still gets its own RAM
  writable segment, so `.bss`, per-instance globals, and reload behave identically.
  XIP changes only *where code executes from*, not the semantics.

**The one-time, uniform requirement (not per-lib code)**

* **The split linker script** — every XIP library must be *linked* with the two-segment
  `R-X | RW` PHDRS layout (as `cpp3` already is). This is **one shared linker script
  applied to all libraries**, not a per-library modification: swap the fixture build
  rule once and every library built afterwards is XIP-ready transparently. The compile
  flags (`-fpic -msingle-pic-base -mpic-register=r10`) are already uniform.
* **The loader work** — the shared core + three thin adapters, done once.

So **adding a new shared library later needs no new code**: build it with the standard
recipe (existing flags + the shared split script) and the loader XIP's it — no touching
main, no touching the loader again.

**The one runtime caveat — "universal for well-behaved libs", not literally all**

A library only XIP's if **no relocation targets its read-only segment**. Standard
`-fpic` C/C++ passes this — all fixups go to the GOT / `.data` / `.data.rel.ro`, which
are on the RAM side (`cpp3` already shows zero text relocations). An exotic library that
plants relocated pointers in a truly read-only section would fail the scan; the design's
**safety scan + copy-whole fallback** means such a library still *works* — it just loads
the old way (whole image in RAM) instead of XIP. Fail-safe, never fail-broken.

Two scope notes:

* This applies to the **ET_DYN PIC world** (`test/{dynamic,static}`). It does **not**
  extend to the `test-relocatable` ET_REL libraries — those bake absolute addresses into code
  and fundamentally cannot XIP.
* A library shipped as a prebuilt single-`RWX` `.so` (like most current fixtures) must be
  **re-linked** once with the split script — a rebuild, not a source edit. Libraries you
  build yourself get it for free.

## The payoff (measured on `cpp3`, the one already split)

| | current (whole image → RAM) | XIP (R-X in flash, RW in RAM) |
| --- | ---: | ---: |
| `cpp3` RAM | **6,604 B** | **248 B** (−96 %) |
| `cpp3` flash-resident | 0 | 2,260 B |

Two points about that 6,604:

* It includes a **~2.9 KB page-alignment hole** between cpp3's two segments
  (0x8d4 → 0x18d4) that the vaddr-addressed buffer counts as RAM (`image_size()` =
  highest `PT_LOAD` end, `common-elf-reader/src/elf-image.cpp:84`). XIP drops the code
  *and* the hole.
* It **inverts the §12.5 ranking**. There the order was: reloc (code in RAM) < current
  dynamic (whole image in RAM). An **XIP dynamic loader beats both** — including the
  reloc loader — because reloc *must* copy code to RAM to patch absolute addresses into
  it, while XIP-PIC leaves code in flash entirely. The "heavy" loader, done XIP, becomes
  the lightest.

The single-`RWX` fixtures need the relink before their split can be measured, but
`cpp3` is representative: the executable segment (code + read-only data) is the bulk
everywhere, and that is what moves to flash.

### All-in-RAM vs XIP — the difference, hardware-confirmed

"All in RAM" is what the copy-whole loader does: it allocates one buffer spanning
the whole image (`image_size()` = highest `PT_LOAD` end, alignment gap included)
and copies **every** segment into it — code, read-only data and writable data all
live in RAM, and the code executes from RAM. XIP splits that:

* the **R-X segment stays in flash** and executes in place — 0 RAM, 0 copy;
* only the **RW segment** (GOT + `.data` + `.bss` + `.data.rel.ro`) is allocated in
  RAM and relocated.

Measured on the WeAct BlackPill STM32F411CE, classifying each module straight from the on-chip
flat-fs (`fw::fs_blob`, memory-mapped flash) — two representative fixtures:

| module | all-in-RAM (copy whole) | XIP RAM (RW only) | code from flash (R-X) | RAM |
| --- | ---: | ---: | ---: | ---: |
| `cpp3.so`     | **6,604 B** | **248 B** | 2,260 B @ `0x08042f5c` | **−96.2 %** |
| `libcppsh.so` | **5,520 B** | **224 B** | 1,200 B @ `0x08046394` | **−95.9 %** |

The `all-in-RAM` column is the *whole* image span (so it also carries the
`0x1000` page-alignment gap between the two segments); XIP drops the code **and**
the gap, leaving only the tiny writable half in RAM. Live console:

```
[boot] flat-fs files = 10
  loaded: XIP;  library_function @ 0x08040d3d (FLASH)   # mylib1
  loaded: XIP;  lf @ 0x08042729 (FLASH)                 # mylib2
  c2 loaded (XIP); ...  c2_add @ 0x080444bd (FLASH)
  c1 loaded (XIP)   cpp3 loaded (XIP)
  libprov/libcons/libcppeh/libcppsh/ehx_catch loaded (XIP)
```

Once every fixture is rebuilt single-pic-base + split (`build-xip-libs.sh`), all ten
report `loaded (XIP)` with code at `0x0804xxxx`. The copy-whole fallback remains for any
module that genuinely cannot XIP — a **fused-`RWX`** blob (code and writable data in one
segment) or a split image whose `VA != file_offset` — but none of the current fixtures
hit it. See `test-xip/comparison.md` for the full per-library size/RAM-savings table.

## "Only needed parts in RAM, rest on flash"

With XIP this is *stronger* than partial loading: "the needed parts in RAM" collapses
to **just the writable segment** (GOT + `.data` + `.bss` + `.data.rel.ro`). All code
and pure `.rodata` stay on flash and execute there — code is never demand-loaded
because it is never in RAM at all.

Partial/lazy **code** loading (pull a function into a RAM cache on first call, evict
later — the existing `scl-overlay` / `test-overlay` machinery) becomes relevant only as
a **fallback** for modules that cannot XIP (relocs in text, or a non-PIC module). Where
single-pic-base XIP applies, it dominates.

## Caveats / hard edges

* **`.data.rel.ro`** (vtables, typeinfo, function-pointer tables) needs patching, so it
  must be on the RAM side. `-fpic` already emits it into the RW load segment, so it goes
  to RAM naturally — but C++-heavy modules shift some "read-only-ish" data into RAM
  (cpp3's 248 B RW is mostly this). Still tiny.
* **RELRO hardening** (re-protect the GOT read-only after relocation) is a no-op here —
  no MMU — which the loaders already rely on.
* **Flash placement is free** *for correctness*: text is position-independent
  (PC-relative internal branches + r10 for data), so the `.so` may sit at any flash
  address the filesystem gives it — provided that address is stored uncompressed and
  contiguous (see **Blocker C**).
* **Alignment**: cpp3's 0x1000 page split wastes *flash*, not RAM — acceptable, or
  relink with a smaller split alignment. Note the split alignment is a *ceiling*: the FS
  must still place the entry on it if you want an MPU region over the R-X range
  (Blocker C, point 1).
* The **call ABI is unchanged** — cross-library calls already r10-swap; XIP changes
  only *where* text and data live, not how calls work.

## Toolchain options, ranked

1. **Recommended — keep the current model, relink for split segments.** Stay
   `-fpic -msingle-pic-base -mpic-register=r10`; give the XIP libraries a two-`PT_LOAD`
   linker script like cpp3's. Minimal change, already proven in-tree.
2. **ROPI/RWPI (`-fropi -frwpi`)** — ARM's formal read-only/read-write position
   independence (text PC-relative → flash, data sb/r9-relative → RAM). Cleaner
   semantics but overlaps what single-pic-base already buys, and moves the base register
   to r9; a recompile for little gain here.
3. **FDPIC (`-mfdpic`)** — the general MMU-less shared-library ABI (function descriptors
   `{entry, got}`), which solves the split-base `R_ARM_RELATIVE` problem natively and
   allows fully independent text/data placement across many libraries. The correct
   long-term answer if scaling to many independently-relocated libs, but a heavier ABI
   change than needed given single-pic-base already works.

## Suggested implementation path

1. **Relink one fixture** (e.g. `mylib1`) with a split `R-X | RW` PHDRS layout and
   confirm (statically, via `readelf -r` + segment ranges) that zero relocations target
   the R-X segment — the same check `cpp3` passes today.
2. **Add the shared core** to `common-elf-reader` / `relocator.cpp`: `rx_segment()` /
   `rw_segment()` classification, the two-base resolver, and the R-X-reloc safety scan.
3. **Adapt one loader end-to-end** first (nolib is the natural choice — it already owns
   the r10 machinery): map R-X as a flash span, copy only RW, set `got_base`/r10 to the
   RAM segment, relocate with two bases.
4. **Measure** the RW-only RAM footprint on hardware (the `module::used`-style trace) and
   compare to the current whole-image figure.
5. **Port the adapter** to newlib and static once the nolib path is proven.

## Implementation — what was built, and the problems solved on the way

The nolib path is now **implemented and hardware-verified** on the WeAct BlackPill STM32F411CE.
Two single-pic-base libraries (`mylib1`, `mylib2`) execute their **entire exported
surface from flash** — every function, global, constructor effect, string literal,
struct member, function-pointer path, weak symbol, and call back into `main` — with
only their RW segment in RAM. This section records the causes and fixes, because
several were not visible from the static analysis above.

### What was built

* **Shared core** (`src/xip`, `include/xip`, target `scl-xip-common`):
  * `segment-map` — `classify_segments()` (split R-X | RW from `PT_LOAD` flags) and
    the `in_text`/`in_data` predicates.
  * `relocate` — `text_is_immutable()` (safety scan) and `apply_rel_xip()`, the
    split-base relocator (`R_ARM_RELATIVE` addend routed to the flash-text or
    RAM-data bias; `GLOB_DAT`/`JUMP_SLOT`/`ABS32`/`REL32`/`COPY`).
  * `compact` — the in-place XIP decision for a flash-resident blob.
* **adapter base** (`src/xip/adapter-base.hpp`, `scl-xip-common`): shared template
  implementations of `dlsym`, `get_got_base`, `is_xip`, `get_xip_map`,
  `patch_main_imports` — used by both `dynamic-nolib` and `dynamic-newlib`.
* **cross-call trampoline** (`src/xip/xip-cross-call.c`, `scl-xip-common`): shared
  `xip_cross_call` / `dl_cross_call` (parameterized by `XIP_CROSS_LOOKUP`/`XIP_CROSS_FN`).
* **nolib adapter** (`src/xip/dynamic-nolib/adapter.cpp`, `scl-xip-dynamic-nolib`):
  `xip::nolib::dlopen`, the copy-whole fallback, and the `xip_cross_lookup` resolver.
  Simple functions (`dlsym`, `dlclose`, etc.) delegate to the adapter base.
* **test tree** (`test-xip`): the hosted-platform firmware app, a full-surface
  print-only test, and `fixtures-src/build-fixtures.sh` (builds the split fixtures).

### Cause → fix, in the order they surfaced

1. **Runtime environment (not a loader bug).** On the plain freestanding platform the
   first `dlopen` HardFaulted before any XIP logic mattered. **Cause:** a loaded
   library imports libc/libm/C++ runtime and needs `main`'s constructors, a real heap
   and exported symbols; the freestanding platform provides none. **Fix:** build
   `test-xip` on the **newlib-hosted platform** (`weact-f411ce-hosted`): newlib
   `startup.c` runs `main`'s `.init_array`, `sbrk` gives a heap, `libc`/`libm`/
   `libstdc++` are linked, `-pie -Wl,-E` exports `main`'s symbols, and the exception
   handlers dump registers. A reviewed linker script (`sections-flatfs-hosted.ld`)
   places the heap *after* the 64 KB image arena so `malloc` cannot corrupt a loaded
   image.

2. **`got_base` reference point.** Functions loaded fine but returned garbage.
   **Cause:** r10 was set to the RW-segment start, but a single-pic-base library
   addresses globals relative to the **`.got`** — matching the stock loader's
   `got_base = base + .got_addr`. **Fix:** `got_base = data_base + (.got_vaddr −
   data_vaddr)`; `data_base` stays the segment base for data-symbol routing, `got_base`
   (the `.got` runtime address) is r10 for `pic_call` and constructors.

3. **Intra-library PLT calls.** The function-pointer path (`gg` → `ptrfunc`) and
   sibling calls crashed. **Cause:** `-fpic` routes even in-module calls through PLT
   stubs, and every linker PLT stub computes its GOT slot **PC-relative** (`add ip,
   pc`) — wrong once text is in flash and the GOT is in RAM. `-fno-plt` is a **no-op on
   ARM** (verified: identical relocations). **Fix:** link the fixtures with
   `-Wl,-Bsymbolic` so defined symbols bind locally → direct `BL`, no PLT.

4. **Calls into `main` (the real wall).** `-Bsymbolic` cannot help genuine externals
   (`f`, `w`, `printf`), which still go through the PC-relative PLT. The caller's `bl
   <flash .plt>` is baked into immutable flash and the PLT reads its slot from **flash
   `.got`**, so the fix must reach that flash slot. **Fix — provision-time patch to a
   runtime trampoline:** `patch_main_imports()` rewrites every external `JUMP_SLOT` in
   the blob (before it is flashed) to `&xip_cross_call`. At call time the flash PLT
   jumps to `xip_cross_call` (naked) with r12 = the GOT-slot address;
   `xip_cross_lookup()` turns that address into the reloc offset, reads the symbol name
   from the module's `.rel.dyn`/`.dynsym`/`.dynstr`, resolves it (other loaded XIP libs
   first, then `main` via `link_context`), and re-issues the call with r10 set to the
   callee's `got_base` (0 for `main`, which is r10-free). This is the stock
   `dl_cross_call` mechanism reworked for XIP handles. **Note:** resolving at provision
   time instead (with `link_context`) does *not* work — it fails that early; patching
   to the fixed trampoline address needs no provision-time resolution, which is the
   whole point.

### Build recipe — every flag, script, and *why*

The whole scheme exists to solve one problem: **the same library must have its code at
one address (flash) and its writable data at a different address (RAM), at the same
time.** A stock shared object cannot do this because the compiler ties data access to the
code's location. Every option below is there to break that tie, or to keep the two halves
addressable independently. This section lists the exact configuration and, for each
piece, what it means and what fails without it.

#### 1. Compiling the libraries

`build-fixtures.sh` builds `mylib1/2`; `build-xip-libs.sh` rebuilds the eight shared
C/C++ fixtures. Both use the same core flags:

```
MCU   -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
PIC   -fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative
OPT   -O0 -ffunction-sections -fdata-sections [-fno-inline-functions]
C++   -std=c++20  (+ per-lib EH/RTTI choice, below)
LINK  -shared -nostartfiles -Wl,-E -Wl,-Bsymbolic -Wl,--gc-sections -T xip-split.ld
```

| flag | what it means | why / implication |
| --- | --- | --- |
| `-fpic` | emit position-independent code (data reached through a GOT, calls through a PLT) | baseline for any `.so`: the image must run wherever it is loaded. On its own, though, ordinary `-fpic` on ARM computes the GOT address *from the PC* — which glues the GOT (and therefore data) to the code. That is exactly what XIP cannot tolerate. |
| `-msingle-pic-base` | use **one fixed register as the GOT pointer** for the entire library instead of recomputing it per function | this is the load-bearing flag. The GOT base is now an *input* (a register value the caller supplies), not something derived from where the code sits. So the code can be in flash while the GOT it points to is in RAM. Without it, every function re-derives the GOT from its own PC → data is forced adjacent to code → no XIP. |
| `-mpic-register=r10` | name **r10** as that GOT-base register | fixes the ABI contract between loader and library: the loader puts the RAM GOT base in r10 before entering the library (`pic_call_*` do this). r10 becomes reserved inside the library. If it is not set, or set wrong, every global/GOT access reads garbage. |
| `-mno-pic-data-is-text-relative` | forbid the "data lives at a constant offset from text" assumption | the default (`…-is-text-relative`) lets gcc address some data as `text ± const`, re-gluing data to code even under single-pic-base. Turning it **off** forces *all* data through the r10 GOT, which is the only form that survives text-in-flash / data-in-RAM. Without it you get correct-looking code that reads the wrong (flash) address for a RAM variable. |
| `-O0`, `-ffunction-sections -fdata-sections`, `-fno-inline-functions` | no optimisation; one section per function/datum; no inlining | a **testing** choice, not a correctness one: it preserves the entire exported surface so every function and global can be resolved and called by name. At higher `-O`, symbols would be inlined or folded away and the full-surface test could not reach them. The `-*-sections` split pairs with `--gc-sections` to still drop genuinely unused code. |
| `-shared` | produce an `ET_DYN` object with `.dynsym` / `.dynstr` / dynamic relocations | required for dlopen-style loading and for `dlsym` to find symbols by name. |
| `-nostartfiles` | no crt0, no `_start` | the library is never "started"; the loader runs its constructors explicitly by walking `.init_array` with r10 already set. |
| `-Wl,-E` (`--export-dynamic`) | put **all** globals in the dynamic symbol table | so `dlsym` can find them and so lib↔main symbol binding works. Without it most symbols are invisible to the runtime resolver and lookups fail. |
| `-Wl,-Bsymbolic` | bind intra-library references to local definitions **at link time** → direct `BL`, no PLT | the single most important *linker* flag for XIP. The PLT is the problem child: a PLT stub reads its GOT slot with PC-relative arithmetic, which is wrong when the stub is in flash and the GOT in RAM. `-Bsymbolic` removes the PLT for every call that stays inside the library, leaving only genuinely external calls (into `main` or another lib) as PLT/`JUMP_SLOT` — a small set the trampoline can handle. Without it, ordinary calls between two functions in the same library would crash. |
| `-Wl,--gc-sections` | drop unreferenced sections | keeps the flash image tight (pairs with `-ffunction/-fdata-sections`). |

**Per-library C++ choice.** `cpp3` and `libcppsh` add
`-fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib -nodefaultlibs`: they need no
exception tables, no RTTI, and no libstdc++ — so those are stripped, making the images
smaller and self-contained with fewer imports to resolve. `-fno-threadsafe-statics` drops
the `__cxa_guard_*` calls around function-local statics (the MCU is single-threaded).
`libcppeh` and `ehx_catch` are built with **exceptions ON** precisely to test that path;
they consequently import the EH runtime (`__aeabi_unwind_cpp_pr0`, `__cxa_*`) from `main`.

#### 2. The library linker script — `xip-split.ld`

```
. = SIZEOF_HEADERS;              /* first section VA == its file offset      */
  … .hash .dynsym .dynstr .rel.dyn .text .rodata .plt .ARM.exidx  → :text (R-X)
. = ALIGN(16);                   /* tiny gap, NOT a 0x1000 page               */
  … .init_array .data.rel.ro .dynamic .got .data .bss             → :data (RW)
```

- **Two PHDRS (`:text` = R-X, `:data` = RW).** Every read-only/executable section goes
  into one PT_LOAD segment, every writable section into the other. This is what produces
  exactly **two** loadable segments, so the loader can leave segment 1 in flash and copy
  only segment 2 to RAM. Note `.got` is deliberately in `:data` — relocations *write* into
  it and r10 must point at it in **RAM**.
- **`. = SIZEOF_HEADERS` + no page alignment ⇒ VA == file offset everywhere.** This is the
  hard requirement. A flash-resident PLT stub computes its GOT-slot address as
  `PC + (GOT_vaddr − PLT_vaddr)`, a constant fixed at link time. At runtime the code
  executes from `fs_blob + file_offset`, and the provision-time patch writes the slot at
  `buf[r_offset]` — again by file offset. Those two only land on the same bytes when
  virtual address equals file offset. A normal `-fPIC` `.so` inserts a `0x1000` page gap
  between segments (VA ≠ offset) and therefore **cannot** XIP — the classifier
  (`segment-map`) detects this and falls back to copy-whole.
- **`ALIGN(16)`** keeps the two segments adjacent with a 16-byte gap instead of a page, so
  the VA==offset invariant holds and no flash/RAM is wasted on alignment padding.

##### Why the `PHDRS` block is necessary, in plain English

An ELF has two parallel coordinate systems for the same bytes: the **file offset**
(where a byte sits inside the `.so` file/blob) and the **virtual address (VA)** (where the
linker *pretends* the byte will live when it runs). A **program header** (`PT_LOAD`) is the
record that ties a run of file bytes to a VA range and a permission (R-X or RW); it is what
a loader reads to decide "copy these bytes to that address."

If you don't write a `PHDRS` block, the linker invents the program headers for you — and its
default policy is built for an MMU operating system: it starts a **new page (`0x1000`) for
every change of permission**, so the read-only half and the writable half of the module land
in *different* pages. To do that it bumps the writable segment's VA up to the next page
boundary **while its file offset stays packed right behind the code**. The result is that,
for the data segment, `VA ≠ file offset`. On a Cortex-M with no MMU that page gap buys us
nothing and actively breaks XIP (below). The explicit `PHDRS` — two segments we define, `:text`
and `:data` — lets us override that policy: we place the sections ourselves, keep a 16-byte
gap instead of a page, and hold **VA == file offset** across the whole image.

##### Why VA == file offset is the thing that must hold — with numbers

The fragile operation is a call from the library into `main` (`printf`, `f`, `w`). It goes
through a **PLT stub that lives in the R-X (flash) half**, and that stub finds its GOT entry
by *PC-relative* arithmetic: "*my GOT slot is at my current PC, plus a constant the linker
baked in.*" That constant is a difference of **virtual addresses**: `GOT_vaddr − PLT_vaddr`.
But at run time the code is not executing at its virtual address — it is executing from
**flash at its file offset** inside the provisioned blob (`fs_blob + file_offset`). So the
address the stub actually computes is:

```
computed_slot = run_PC              + (GOT_vaddr − PLT_vaddr)
              = (blob + PLT_off)    + (GOT_vaddr − PLT_vaddr)
```

and the slot it *needs* to hit is the real flash location of that GOT word, `blob + GOT_off`.
Setting the two equal and cancelling `blob`:

```
PLT_off + GOT_vaddr − PLT_vaddr  ==  GOT_off
⇔  GOT_vaddr − GOT_off  ==  PLT_vaddr − PLT_off
⇔  (VA − file_offset) is the SAME in the data segment as in the text segment
```

`. = SIZEOF_HEADERS` makes that delta **zero** for the first byte, and packing both segments
with only `ALIGN(16)` (no page jump) keeps it zero everywhere — so `text_vaddr == text_off`
and `data_vaddr == data_off`, and the equality holds by construction.

**Worked example — the split layout (works).** Blob provisioned at flash `0x08040000`:

| item | VA | file offset | VA − off |
|---|---|---|---|
| PLT stub (text) | `0x0120` | `0x0120` | 0 |
| `.got.plt` slot (data) | `0x0684` | `0x0684` | 0 |

The stub runs at `0x08040000 + 0x0120 = 0x08040120` and computes
`0x08040120 + (0x0684 − 0x0120) = 0x08040684`. The real slot is at `0x08040000 + 0x0684 =
0x08040684`. **They match** → the call reaches `main`. ✓

**Worked example — the default `-fPIC` layout (breaks).** Same code, but the linker
page-aligned the data segment, so its VA jumped to `0x1000` while its offset stayed at
`0x05b0` (`VA − off = 0xA50`, not 0):

| item | VA | file offset | VA − off |
|---|---|---|---|
| PLT stub (text) | `0x0120` | `0x0120` | 0 |
| `.got.plt` slot (data) | `0x1024` | `0x05d4` | `0xA50` |

The stub still runs at `0x08040120` and computes `0x08040120 + (0x1024 − 0x0120) =
0x08041024`, but the real slot is at `0x08040000 + 0x05d4 = 0x080405d4`. **`0x08041024 ≠
0x080405d4`** — the stub reads a random flash word and branches to garbage → HardFault. The
two deltas disagree (`0xA50 ≠ 0`), which is exactly the condition the classifier checks; when
it sees `data_vaddr − data_off ≠ text_vaddr − text_off` it refuses XIP and falls back to
copy-whole. (The same VA==offset identity is also what lets the provision-time patch address
the slot as `buf[r_offset]`, and what makes `.data`'s `R_ARM_RELATIVE` addends land correctly
once the RW half is copied to RAM.)

#### 3. Compiling and linking the main test

`main` is **not** XIP-loaded — it is the host that provides the runtime (heap, libc/libm,
the C++ EH runtime, and the imported symbols `AA/BB/f/w/s1/printf/malloc/…`). Its flags
come from `cmake/arm-none-eabi.cmake` and `test1/CMakeLists.txt`:

```
compile  -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
         -fpic -O0 -g -ffunction-sections -fno-common -ffreestanding
         -fno-tree-loop-distribute-patterns  (+ C++: -fno-threadsafe-statics)
         platform/startup sources overridden with -fno-pic
link     -pie -nostartfiles --specs=nano.specs
         -Wl,-Bsymbolic -Wl,-E -Wl,--gc-sections
         -T sections-flatfs-hosted.ld
         + libsupc++.a libgcc.a stdc++ m
         (no -u flags — borrowed libc/libsupc++ symbols are pulled in by a
          [[gnu::used, gnu::retain]] anchor array in main.cpp; see below)
```

| choice | why / implication |
| --- | --- |
| `-fpic` on `main`, **but not** `-msingle-pic-base` | `main` runs normally: its data is placed in RAM by startup and it uses ordinary PC-relative PIC. It does **not** reserve r10 — r10 is meaningful only *inside* a loaded library, and the loader sets it on the way in. Mixing single-pic-base into `main` would needlessly sterilise r10 for the host. |
| platform/startup sources `-fno-pic` | reset, vector table and HAL code must sit at fixed addresses and run before any GOT exists; PIC there would be wrong and pointless. |
| `-ffreestanding`, `-fno-common`, `-fno-tree-loop-distribute-patterns` | no hosted-libc assumptions; every global its own section (for gc); and stop gcc rewriting hand-rolled loops back into `memcpy/memset` calls that the freestanding link may not provide. |
| exceptions / RTTI **ON**, links `libsupc++`/`libgcc` | `main` *is* the C++ runtime the loaded libraries borrow — `__cxa_*`, the personality routine, `operator new/delete`, `__dynamic_cast` all live here. |
| `-pie` | makes `main` a position-independent executable with its own `.dynsym`/GOT, which is what lets a loaded library resolve an import (e.g. `printf`) *against main* at runtime. |
| `-Wl,-E` (export dynamic) | publishes **every global `main` defines** into `.dynsym` so the libraries can bind to them (`AA/BB/f/w/s1`, the trace hooks, `fm_*`). This is literally how a library's call reaches main — no `-u` is needed for these, `-E` keeps them. |
| the `g_link_anchor[]` array in `main.cpp` (replaces ~28 `-u` flags) | `-E` only *exports* what is *linked*; symbols the libraries borrow but `main` never calls itself (`printf`, `operator new` = `_Znwj`, `__cxa_*`, the unwind personality, `__dynamic_cast`, `sinf/cosf/logf/sqrtf`) live in `libc.a`/`libsupc++.a` and are pulled from an archive **only when referenced**. One `[[gnu::used, gnu::retain]]` array takes their address to force the link; `-E` then exports them. It is **not** a resolver — the loader resolves purely via `.dynsym`. See `xip-memory-analysis.md` §10.4 for the derivation and the `g_host[]`→`.dynsym` migration. |
| `--specs=nano.specs` | newlib-nano libc (small). `%f` support (`_printf_float`) is only needed if a fixture actually prints a float; the XIP fixtures print integers, so it is not forced (contrast `test-static/test-static`, whose `main` prints `%f` and keeps `-u _printf_float`). |

#### 4. The main linker script — `sections-flatfs-hosted.ld`

```
FLASH    0x08000000 256K   ← main's code/rodata
FLASH_FS 0x08040000 256K   ← on-chip flat filesystem: the provisioned .so blobs live here
RAM      0x20000000 128K
```

- **Two flash regions.** `main` occupies the low 256 K; the high 256 K (`FLASH_FS`,
  starting at `0x08040000`) is reserved for the on-chip flat-fs. `fw::fs_blob()` returns a
  pointer **into `FLASH_FS`** — that pointer *is* the address the libraries execute from.
  The closing `ASSERT(_sidata + (_edata−_sdata) <= _fs_base, …)` guarantees main's own
  image can never grow past `0x08040000` and corrupt the `.so` store (and vice-versa).
- **`.data … >RAM AT>FLASH`, `_sidata = LOADADDR(.data)`.** Main's initialised data is
  *stored* in flash but *lives* in RAM; startup copies it. Standard firmware practice —
  listed here so the `_sidata/_sdata/_edata` symbols are accounted for.
- **`.image_arena (NOLOAD) 64K` in RAM.** A reserved RAM staging buffer. `provision()`
  reads each `.so` file over semihosting into this arena, patches its external
  `JUMP_SLOT`s, then writes it into the flat-fs. `NOLOAD` = address space reserved but not
  initialised from flash. Its size caps the largest single `.so` that can be provisioned.
- **Heap after the arena, stack at top of RAM.** Gives newlib `sbrk` a real heap, so a
  library's `malloc`/`operator new` (routed to `main`) has somewhere to allocate, and
  gives `main` its stack.

#### 5. Build system / orchestration

The build is **CMake**, driven by the `cmake/arm-none-eabi.cmake` toolchain file, which
generates ordinary Unix Makefiles under `test-xip/build/`. There is no hand-maintained
Makefile any more — the toolchain file notes its flag set matches the legacy Makefile
firmware build. `test-xip/build-and-run.sh` sequences the whole thing, and the **order
matters**:

1. `build-fixtures.sh` → builds `mylib1/2` (single-pic-base, split) into `test-xip/fixtures`.
2. `cmake … configure` → the test's `CMakeLists.txt` stages baseline `-fPIC` copies of the
   other eight fixtures via `file(COPY test/fixtures/*.so)`.
3. `build-xip-libs.sh` → **rebuilds those eight single-pic-base + split, overwriting** the
   baseline copies. Because this runs *after* configure, the XIP-able versions win.
4. `cmake --build` → links `main` + the two test units + the flat-fs backend against
   `scl::xip-dynamic-nolib`.
5. `openocd … program … reset run` → flashes and captures the semihosting console to
   `run.log`. Use `RUN_MS=20000` so slow semihosting reads finish before shutdown.

#### 6. Putting it together — how one call actually executes from flash

1. **Provision.** `main` reads `mylib1.so` over semihosting into the RAM `image_arena`,
   rewrites each external `JUMP_SLOT` in the raw image to `&xip_cross_call | 1`, and stores
   the blob into the flat-fs at `0x0804xxxx`.
2. **dlopen (XIP path).** The adapter classifies the two segments, confirms VA==offset,
   and **leaves the R-X segment in flash** (it just keeps the `fs_blob` pointer). It copies
   only the RW segment to a small RAM block, computes
   `got_base = data_base + (.got_vaddr − data_vaddr)` (r10 must point at `.got`, not the
   segment start), applies relocations with **split bases** (a `R_ARM_RELATIVE` targeting
   code resolves against the flash `text_base`; one targeting data resolves against the RAM
   `data_base`), then runs `.init_array` constructors with r10 = `got_base`.
3. **Calling a function.** `pic_call_u32(fn, got_base, arg)` loads r10 = `got_base` and
   branches to `fn` — an address **in flash**. Inside, the function reads its globals as
   `[r10 + slot]`, hitting the **RAM** GOT/data copy; it reads a string literal by loading
   a GOT slot that a load-time `R_ARM_RELATIVE` set to the **flash** `.rodata` address.
   Code runs from flash, data from RAM, transparently.
4. **Calling into `main` or another library.** That call goes through a PLT stub **in
   flash**, which loads its `.got.plt` slot — the slot we pre-patched to `xip_cross_call`.
   The trampoline calls `xip_cross_lookup(slot_addr)`, which maps the slot back to a symbol
   name, resolves the real target and the *callee's* `got_base`, swaps r10 to the callee's
   base, and tail-calls the target. On return, r10 is restored. This is why external slots
   had to be patched **before flashing** (the PLT reads them PC-relative from flash) and why
   only *external* slots need it — `-Bsymbolic` already turned intra-lib calls into direct
   branches.

The one thing this chain cannot do is carry a **C++ exception** back through the
`xip_cross_call` frame: that trampoline's `.ARM.exidx` entry is `CANTUNWIND` (compact
model 0, no `.personality`), so the unwinder stops there. RTTI/`dynamic_cast` and
non-crossing exceptions work; a throw that must unwind across the trampoline (`ehx_catch`)
is skipped. See `test-xip/comparison.md`.

### Scope and limits (verified)

* **All ten fixtures now XIP.** A library can only XIP if it uses a **single-pic-base
  (r10-relative) GOT**, not a PC-relative one — so the C/C++ libraries that shipped
  `-fPIC` (`c1 c2 cpp3 libprov libcons libcppeh libcppsh ehx_catch`) are **rebuilt**
  single-pic-base + split by `test-xip/fixtures-src/build-xip-libs.sh` (`build-fixtures.sh`
  builds `mylib1/2`). With that, every fixture reports `loaded (XIP)` with code at
  `0x0804xxxx` and only its RW segment in RAM — hardware-verified (`run.log`). The build
  ordering matters: `build-and-run.sh` runs `build-xip-libs.sh` *after* the CMake
  configure, so the single-pic-base rebuilds overwrite the baseline `-fPIC` copies.
* **Cross-library calls** (`c1→c2`, `cpp3↔c2`) run from flash: their external JUMP_SLOTs
  route through the cross-call trampoline (`fw::dl_enable_cross_call(true)`), so each
  callee executes with its own r10 (`got_base`).
* **The one real limitation — C++ exceptions across the trampoline.** A throw whose stack
  unwind must pass *through* the cross-call trampoline cannot be caught: the trampoline's
  `.ARM.exidx` entry is compact model 0 (**CANTUNWIND** — `.fnstart/.fnend` with no
  `.personality`), so the unwinder stops at that frame. RTTI/`dynamic_cast` and
  non-crossing EH work (`eh_dyncast` passes); the cross-boundary catch case (`ehx_catch`)
  is therefore skipped. See `test-xip/comparison.md`.
* Struct-return functions (`ret_struct`) are exercised via their backing global rather
  than a by-value call (the `pic_call_*` shims pass/return scalars only).

## Authoring a new XIP library and a consumer — a template

This chapter is a copy-paste starting point for writing your own XIP-able library (C or
C++) and an app that loads it. Everything here is the same machinery the fixtures use.

### The rules, up front

A library XIPs only if it obeys three rules; the loader falls back to copy-whole
otherwise.

1. **Single-pic-base codegen** — compile with
   `-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative` so all data
   goes through the r10 GOT and code is independent of data placement.
2. **Split, VA==offset layout** — link with `xip-split.ld` (two PT_LOAD segments, no page
   gap). See §"The library linker script".
3. **No PLT call before r10 is live** — do not call an *imported* function (into `main` or
   another lib) from a `__attribute__((constructor))`. Constructors run early and a PLT
   call there dereferences the GOT before routing is fully set. Intra-lib calls and pure
   computation in constructors are fine.

### 1a. A C library — `mylibx.h` / `mylibx.c`

```c
/* mylibx.h — the public ABI (shared by the library and, if it likes, the app) */
#ifndef MYLIBX_H
#define MYLIBX_H
#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int lib_counter;          /* a library-owned global (lands in RAM)   */
extern const char*  lib_name;             /* string literal via R_ARM_RELATIVE       */

unsigned int mx_add(unsigned int a, unsigned int b);
unsigned int mx_tick(void);               /* mutates lib_counter                     */
const char*  mx_name(void);

/* Imported FROM the app (resolved against main at load/patch time): */
extern unsigned int host_scale;           /* app-provided global                     */
unsigned int        host_hook(unsigned int x);   /* app-provided function            */

#ifdef __cplusplus
}
#endif
#endif
```

```c
/* mylibx.c */
#include "mylibx.h"

unsigned int lib_counter = 100;
const char*  lib_name    = "mylibx v1";

/* OK: pure/intra-lib work only — no imported (PLT) calls here. */
__attribute__((constructor)) static void mx_ctor(void) { lib_counter += 1; }

unsigned int mx_add(unsigned int a, unsigned int b) { return a + b; }
unsigned int mx_tick(void) { return ++lib_counter; }
const char*  mx_name(void) { return lib_name; }

/* An export that calls back into the app — this becomes a JUMP_SLOT patched to the
   cross-call trampoline; runs fine because it is NOT in a constructor. */
unsigned int mx_scaled(unsigned int x) { return host_hook(x) * host_scale; }
```

### 1b. A C++ library

Same rules; add `-std=c++20`, and — unless you specifically want exceptions — build with
`-fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib -nodefaultlibs` to stay small
and self-contained. Export a C ABI so the scalar call shims (`pic_call_*`) can reach it:

```cpp
/* mywidget.cpp */
struct Widget { unsigned v; explicit Widget(unsigned s):v(s*2){} unsigned get() const { return v; } };

extern "C" unsigned int wx_make(unsigned int seed) {   /* returns an opaque handle    */
    static Widget w{0}; w = Widget{seed}; return reinterpret_cast<unsigned int>(&w);
}
extern "C" unsigned int wx_get(unsigned int h) {
    return reinterpret_cast<Widget*>(h)->get();
}
```

For real heap objects, call `operator new`/`delete` — they resolve to the app's runtime
(anchor `_Znwj`/`_ZdlPv` in the app; see below).

### 2. Building the library (XIP-split `.so`)

```sh
TC=/path/to/arm-none-eabi          # toolchain bin
MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
LD=test-xip/fixtures-src/ldscripts/xip-split.ld

# C:
$TC/arm-none-eabi-gcc $MCU $PIC -O0 -ffunction-sections -fdata-sections \
    -shared -nostartfiles -Wl,-E -Wl,-Bsymbolic -Wl,--gc-sections \
    -T $LD -I. -o mylibx.so mylibx.c

# C++ (no-EH):
$TC/arm-none-eabi-g++ $MCU $PIC -std=c++20 -O0 -ffunction-sections -fdata-sections \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib -nodefaultlibs \
    -shared -nostartfiles -Wl,-E -Wl,-Bsymbolic -Wl,--gc-sections \
    -T $LD -I. -o mywidget.so mywidget.cpp
```

Verify it produced two `LOAD` segments with VA==offset:
`arm-none-eabi-readelf -l mylibx.so | grep LOAD` — the `VirtAddr` and `Offset` columns
must match. If they differ (a page gap), the loader will copy-whole instead of XIP.

### 3. The consumer app

The app is an ordinary `-pie` firmware (see §"Compiling and linking the main test"). Two
things it must do for a library:

**(a) Export the symbols the library imports.** For each `extern` the library expects from
the app — here `host_scale` and `host_hook` — just **define** it. Because the app links
`-pie -Wl,-E`, every global it defines is exported into `.dynsym`, where the loader resolves
it (`link_context::from_self()`); `--gc-sections` treats exported symbols as referenced, so
**no `-u` flag is needed** for app-defined symbols:

```cpp
extern "C" {
__attribute__((used)) unsigned int host_scale = 4;
__attribute__((used)) unsigned int host_hook(unsigned int x) { return x + 1; }
}
```

If instead the library borrows a symbol the app itself never calls (`printf`, `operator
new`, a libm function…), that symbol won't be linked into the app at all — reference it once
in a `[[gnu::used, gnu::retain]]` anchor array (see `main.cpp`) to pull it from the archive
so `-E` can export it.

**(b) Provision, load, and call.** Using the helpers from `test-helpers.hpp`:

```cpp
#include "xip/dynamic-nolib/adapter.hpp"
#include "scl/pic-trampoline.h"
#include "fs-archive.hpp"

// provision(): read mylibx.so (semihosting) -> patch_main_imports() -> fw::fs_add()
//   (see main.cpp; patch_main_imports rewrites external JUMP_SLOTs to the trampoline)

void use_mylibx() {
    const fw::FsEntry* e = fw::fs_find("mylibx.so");
    void* h = xip::nolib::dlopen({ fw::fs_blob(*e), e->size }, "mylibx.so");
    const std::uint32_t g = xip::nolib::get_got_base(h);   // r10 for this module

    // read a global (RAM copy):
    auto* counter = static_cast<volatile unsigned*>(xip::nolib::dlsym(h, "lib_counter"));

    // call a function FROM FLASH with r10 = got_base:
    void* add = xip::nolib::dlsym(h, "mx_add");
    unsigned s = pic_call_u32_2(add, (void*)g, 10, 20);     // -> 30

    void* scaled = xip::nolib::dlsym(h, "mx_scaled");
    unsigned r = pic_call_u32(scaled, (void*)g, 5);         // calls back into the app

    xip::nolib::dlclose(h);
}
```

### 4. API reference

`#include "xip/dynamic-nolib/adapter.hpp"` — namespace `xip::nolib`:

| call | purpose |
| --- | --- |
| `void* dlopen(std::span<const std::byte> image, const char* name)` | XIP-load a flash-resident image (from `fs_blob`); returns a handle or `nullptr`. Falls back to copy-whole if the image is not XIP-able. |
| `void* dlsym(void* h, const char* name)` | resolve a symbol → runtime address (flash for code, RAM for data). |
| `std::uint32_t get_got_base(void* h)` | the r10 / GOT base to pass to `pic_call_*` when entering this module. |
| `bool is_xip(void* h)` | true if the module runs in place from flash (false ⇒ copy-whole). |
| `void dlclose(void* h)` | release the handle (frees the RAM segment). |
| `int patch_main_imports(std::span<std::byte> image)` | provision-time: rewrite external `JUMP_SLOT`s to the cross-call trampoline. Call on the writable buffer **before** `fs_add`. |

`#include "scl/pic-trampoline.h"` — enter a module with r10 set:

| call | signature |
| --- | --- |
| `pic_call_void(fn, got)` | `void fn(void)` |
| `pic_call_u32(fn, got, a0)` | `uint32_t fn(uint32_t)` |
| `pic_call_u32_2(fn, got, a0, a1)` | `uint32_t fn(uint32_t, uint32_t)` |

The shims pass/return **scalars** only; for wider signatures pass pointers (as `wx_make`
returns an opaque handle above) or add a shim. Always call an XIP function through these —
calling it directly leaves r10 wrong and its globals read garbage.

## Bottom line

The expensive prerequisite — decoupling data from text via `-msingle-pic-base` / r10 —
is already done and already exercised by the r10-swap trampoline. Flash-XIP reduces to
(a) relinking libraries into split R-X | RW segments (cpp3 shows it works with zero text
relocations) and (b) a loader path that maps the R-X segment as a flash span, copies only
RW to RAM, and applies relocations with two bases. Structurally it is one shared ELF/reloc
core plus three thin per-loader adapters. It would make the dynamic loader the lightest
RAM option in the repo.

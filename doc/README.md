# Cortex-M Source Code Libraries (SCL)

Reusable **shared-library loading** infrastructure for Cortex-M microcontrollers
(reference target: **WeAct BlackPill (STM32F411CE)**). Originally extracted from the
`f411ce-libs` research projects, this repository is now the **standalone,
self-contained** home of the SCL: the repo root *is* the library.

The SCL produces **library targets only — never firmware**. Every
board-specific, storage-specific, or OS-specific concern (startup code, linker
scripts, filesystems, logging back-ends) is the consuming application's
responsibility. Every source its test apps compile lives inside `test/`, and the
hosted CMSIS/HAL/newlib platform is vendored under
`test/platforms/board-hosted/vendor/`. One unified board layer
(`test/platforms/board/`) serves both the **NUCLEO-F411RE** and the
**WEACT-F411CE**; only board boot/init differs (`-DSCL_BOARD=nucleo|weact`).

> **ELF32 reader/parser — self-contained, no submodule.** The reader/parser
> (`elf_image`, `byte_source`, ELF32 types, `error`/`result`, and the bounded
> byte accessors in `detail/bytes.hpp` — `load_word`/`load_byte`/`store_word`/
> `name_at`) lives under **[`common-elf-reader`](common-elf-reader)** and is
> vendored **in this workspace** (no git submodule, no links to external
> projects). The resolving/patching layer (`dynamic_info`, `symbol_resolver`,
> `reloc_applier`, `relocator`, …) and all loader modules stay in this repo.

> **New to this?** Read [`scl-guide.md`](doc/scl-guide.md) first — a plain-English
> walkthrough of what each library does differently and how every test
> app's `main()` uses it (start / configure / use / close).

Language level: **C++20** (no C++23 — `loader::result` in
`include/loader/error.hpp` is a hand-rolled C++20 replacement for
`std::expected`; use `loader::unexpected(...)`) and C11.

---

## 1. Directory structure

```
shared-lib-loaders (branch)   # SCL (10 libs) + common-elf-reader submodule + static patcher + reloc loader + XIP adapters
├── CMakeLists.txt            # the SCL library package (targets + options + aliases)
├── README.md                 # this file
├── common-elf-reader/        # ELF32 reader/parser — GIT SUBMODULE (elf32_read_parse.git)
├── doc/                      # documentation
│   ├── manual-scl-loaders.md     # Reference manual for all SCL loaders (dynamic/static/overlay)
│   ├── scl-guide.md              # plain-English guide: what each library does + main() lifecycles
│   ├── build-linking-guide.md    # ldscripts + C/C++ compile/link flags across all 3 repos (why & how)
│   ├── build-and-test.md         # combined build+test recipe for every module (+ expected output)
│   ├── scl-shared-libs.md        # original requirements document (historical)
│   └── scl-analysis-and-migration-plan.md   # codebase analysis + migration record (historical)
│
├── cmake/                    # toolchain files for consumers
│   ├── arm-none-eabi.cmake          # FREESTANDING firmware: -fpic, -ffreestanding,
│   │                                #   hard-float M4 (dynamic/overlay apps)
│   └── arm-none-eabi-hosted.cmake   # HOSTED firmware: newlib/nano, non-PIC,
│                                    #   soft float (-DSCL_HF=ON for hard float)
│                                    #   (static-loader apps)
│
├── include/                  # == PUBLIC HEADERS ==
│   ├── loader/               # common core, shared by every loader flavour:
│   │   │                     #   dynamic_info, symbol_resolver, relocator
│   │   │                     #   (hosted), reloc_applier, LazyResolve,
│   │   │                     #   region_allocator.
│   │   │                     #   (byte_source, error/result, ELF32 types,
│   │   │                     #    elf_image and detail/bytes.hpp — the bounded
│   │   │                     #    byte accessors — come from ./common-elf-reader submodule)
│   ├── scl/                  # common firmware-level headers:
│   │   │                     #   log.h (the logging hook), pic-trampoline.h,
│   │   │                     #   image-memory.hpp (arena), link-context.hpp
│   │   │                     #   (_DYNAMIC self-parsing), unwind-exidx.h
│   ├── dynamic-nolib/        # dlopen loader public API: dl.hpp (handle,
│   │                         #   dlopen_source/dlsym/dlclose, cross-call and
│   │                         #   trace toggles, dump_* diagnostics), lazy-plt.hpp
│   ├── dynamic-newlib/       # newlib static-loader public API: dynamic-linker.hpp
│   │                         #   (dynamic_linker/library), patch-journal.hpp,
│   │                         #   image-memory.hpp (heap), link-context.hpp
│   │                         #   (_reloc table)
│   ├── static/               # ELF32 static patcher public API: elf-reader.h,
│   │                         #   shlib-loader.h, logger.h
│   ├── reloc/                # ET_REL module loader public API: reloc-loader.h
│   │                         #   (load / load_set / sym; module / module_set)
│   └── overlay/              # overlay public API: overlay.h (descriptor table,
│                             #   scl_overlay_init/load/current)
│
├── src/                      # == IMPLEMENTATION ==
│   │                         # common core .cpp directly here (dynamic_info,
│   │                         #   symbol_resolver, relocator, reloc_applier,
│   │                         #   lazy_resolve, region_allocator, image_memory,
│   │                         #   link_context, log.c).  elf_image lives in
│   │                         #   ./common-elf-reader/src/elf-image.cpp (submodule)
│   ├── port/                 # Cortex-M assembly port layer (ARM-only):
│   │                         #   pic-trampoline.c (enter a lib with r10 set),
│   │                         #   dl-cross-call.c (lib->lib r10 swap veneer),
│   │                         #   lazy-resolve.c (dl_runtime_resolve stub),
│   │                         #   unwind-find-exidx.c (EHABI lookup for loaded
│   │                         #   images). VFP asm => hard-float only; owned by
│   │                         #   the scl-dynamic-nolib target.
│   ├── dynamic-nolib/        # dl.cpp — the dlopen_source pipeline
│   ├── dynamic-newlib/       # dynamic-linker.cpp, patch-journal.cpp,
│   │                         #   image-memory.cpp, link-context.cpp
│   ├── static/               # elf-reader.cpp, shlib-loader.cpp, logger.cpp
│   │                         #   (the ELF32 static patcher / scl::static)
│   ├── reloc/                # reloc-loader.cpp (ET_REL loader / scl::reloc)
│   └── overlay/              # overlay.c + ldscripts/overlay-fragment.ld
│                             #   (documented linker-script TEMPLATE — the SCL
│                             #   never owns real linker scripts)
│
└── test/                     # == TESTS: a SEPARATE CMake project ==
    ├── CMakeLists.txt        # cross-compiled firmware test apps (see §4.3)
    ├── cmake/
    │   ├── scl-test-app.cmake        # helper: one firmware app per testN
    │   └── toolchains/               # xpack-style toolchain registry
    ├── platforms/
    │   ├── board-hosted/           # HOSTED platform (CMSIS/HAL/newlib) for the
    │   │   └── vendor/             #   static loader — CMSIS/HAL/newlib + DYN
    │   │                           #   suite + fixture-lib sources, VENDORED here
    │   ├── board/                  # FREESTANDING unified board platform:
    │   │   ├── cmake/platform-library.cmake  # INTERFACE target platform::board
    │   │   ├── src/                # startup.c, board.c, semihost.c, libc-shim.c,
    │   │   │                       #   printf-shim.c (nucleo/weact variants)
    │   │   ├── include/            # board.h, semihost.h, semihost-file.hpp,
    │   │   │                       #   dl-semihost.hpp
    │   │   ├── ldscripts/          # mem.ld, sections.ld, mem-overlay.ld,
    │   │   │                       #   sections-overlay.ld, mem-flatfs.ld
    │   │   ├── openocd-nucleo.cfg  # Nucleo-F411RE ST-LINK config
    │   │   └── openocd-weact.cfg   # WeAct-F411CE CMSIS-DAP config
    │   ├── qemu-f411re/            # QEMU freestanding platform:
    │   │   ├── cmake/platform-library.cmake  # INTERFACE target platform::qemu-f411re
    │   │   ├── src/                # QEMU board.c (no RCC polling, LED over
    │   │   │                       #   semihosting), startup.c (0xFF fill for
    │   │   │                       #   flat_fs), semihost.c, libc-shim.c, printf-shim.c
    │   │   ├── include/            # board.h, semihost.h, semihost-file.hpp,
    │   │   │                       #   dl-semihost.hpp
    │   │   └── ldscripts/          # mem.ld, sections.ld, mem-overlay.ld,
    │   │                           #   sections-overlay.ld, mem-flatfs.ld (CCM SRAM-backed)
    │   └── qemu-f411re-hosted/     # QEMU hosted platform:
    │       ├── cmake/platform-library.cmake  # INTERFACE target platform::qemu-f411re-hosted
    │       └── src/                # led.cpp (semihosting trace), initialize-hardware.c
    │                               #   (no HSE/PLL polling for QEMU)
    ├── tools/                # flash-run.sh + flash-run-sf.sh / flash-run-hf.sh
    │                         #   (flash by app NAME, resolved to its build tree)
    │                         #   + qemu-run.sh, qemu-gdb.sh (QEMU runner + GDB)
    │                         #   + run-with-tmp-fixtures.sh (static2 /tmp staging)
    ├── common/               # shared test support headers/sources
    │   │                     #   (Memorybyte_source, loaded_image), test-main.cpp
    ├── fixtures/             # committed PREBUILT ARM .so fixtures (mylib1/2,
    │                         #   libcppsh, libcppeh, libprov, libcons, c1, c2,
    │                         #   cpp3, ehx_catch, libsh1/2)
    ├── fixtures-src/         # the fixture library SOURCES (+ their pic ldscripts);
    │                         #   the CMake fixture targets build .so files from here
    │   ├── dynamic1..5/      #   core suites (src/, include/, CMakeLists.txt)
    │   └── overlay1/         #   overlay window-manager suite
    ├── test-dynamic-nolib/         # ON-TARGET firmware apps (§4.3)
    │   ├── test1/            #   demo (dyn1) — src/, include/, CMakeLists.txt
    │   ├── test2/            #   16-test battery (dyn2)
    │   └── test3/            #   flat-fs battery (flatfs) — backend in src/+include/
    ├── test-overlay/
    │   └── test1/            #   overlays-vs-dynamic comparison (ovl1)
    ├── test-dynamic-newlib/          # hosted apps (own build tree test/build-dynamic-newlib[-hf]; §4.3)
    │   ├── test1/            #   SCL static battery (journal-revert proof)
    │   └── test2/            #   full static-loader DYN suite (sources in vendor/)
    ├── test-static/          # scl::static patcher demo (self-contained app:
    │                         #   main.cpp, system/, ldscripts/, libsh1/2, own
    │                         #   CMakeLists + Makefile; §4.3)
    └── test-misc/            # reserved for apps fitting no loader category

test-relocatable/                   # == SEPARATE ET_REL test project (scl::reloc) ==
├── CMakeLists.txt            #   self-contained NON-PIC firmware (own reloc.ld)
├── src/main.cpp              #   the 9-check load_set battery
├── platform/reloc.ld         #   plain non-PIC linker script (no dynsym/GOT/_DYNAMIC)
├── fixtures-reloc/           #   ET_REL fixtures: Makefile + relprov/relmod + reuse c1/c2/cpp3
└── tools/flash_reloc_run.sh  #   build (app + fixtures) + flash + run
```

### Self-contained: where every test app's sources live

Everything a test app compiles lives inside this repo — no external folder is
needed:

| Test app | Its sources | Platform |
|---|---|---|
| `scl-fw-dyn1` / `dyn2` | `test-dynamic/test-dynamic-nolib/test{1,2}/src/` | `board` (freestanding) |
| `scl-fw-flatfs` | `test-dynamic/test-dynamic-nolib/test3/{src,include}/` (flat-fs backend included) | `board` |
| `scl-fw-ovl1` | `test-overlay/test1/src/` (driver + overlay modules) | `board` |
| `scl-fw-dynamic-newlib1` / `dynamic-newlib2` | `test-dynamic/test-dynamic-newlib/test{1,2}/` + the vendored suite | `board-hosted` |
| `f411ce-sh` (scl::static patcher) | `test-static/test-static/` (`src/`, `system/`, `ldscripts/`, `libsh1/2`) | self-contained (µOS++ soft-float) |
| `scl-fw-xip-*` | `test-xip/test-xip-*/src/` | `board-hosted` |
| `scl-fw-xip-static-spi-flash` | `test-xip/test-xip-static/spi-flash/src/` | `board-hosted` |

The freestanding board platform (`test/platforms/board/`) owns
startup/board/semihosting/libc-shim/crt/printf-shim/`dl_semihost`/`scl_log`
bridge and all freestanding linker scripts. The hosted platform
(`board-hosted/vendor/`) vendors the CMSIS/HAL/newlib environment, the
`_reloc` linker scripts, the DYN suite and the fixture-library sources.

Fixtures used at run time: the committed `test/fixtures/*.so` (freestanding
apps, opened over semihosting) and the `.so` built by the CMake fixture targets
from `vendor/libs/` + `vendor/debug/` into `vendor/libs/build[_hf]` (static apps).

---

## 2. The ten libraries

Link with the namespaced aliases (`scl::common`, `scl::dynamic-nolib`, `scl::dynamic-newlib`,
`scl::static`, `scl::reloc`, `scl::overlay`, `scl::xip-common`, `scl::xip-dynamic-nolib`,
`scl::xip-dynamic-newlib`, `scl::xip-static`). All are **INTERFACE source libraries** (xpack
pattern): CMake never produces `.a` archives — linking a target propagates the SCL
*source files*, which the consuming application compiles itself with its own
flags. Only what an application links gets compiled, exactly once per
executable (transitively shared `scl::common` sources are de-duplicated).

The four loader flavours cover both linking models:
- **`scl::dynamic-nolib`** — freestanding `dlopen` of `-fPIC -shared` **ET_DYN**
  objects, `_DYNAMIC` parsed at boot, **lazy** GOT/PLT binding;
- **`scl::dynamic-newlib`** — hosted/newlib link-time binding with revertible GOT patching;
- **`scl::static`** — standalone ELF32 static patcher (GOT-patch + relocate a PIC `.so`);
- **`scl::reloc`** — loader for `-mlong-calls -fno-pic -r` **ET_REL** modules
  (kernel-module style): **no GOT/PLT/r10**, **eager** absolute (ABS32) relocation,
  with `load_set` for cyclic module groups. The eager counterpart of the ET_DYN loaders.

### 2.1 `scl::common` — the shared core

Everything every loader flavour needs; fully portable (builds and is unit-tested
on the host).

The rows marked † are the **reader/parser** (plus the shared bounded byte
accessors), which live in the shared [`common-elf-reader`](../common-elf-reader)
repo. It is pulled in via `add_subdirectory` as the
`common-elf-reader::elf-reader` **INTERFACE** target (same source-code-library
pattern as `scl-common`): `scl-common` links it, so its `src/elf-image.cpp` and
`include/loader/` headers propagate transitively and are compiled **from
source** into each consumer — never as a `.a` archive. The rest are this repo's
own resolving/patching code.

| Component | Header | What it does |
|---|---|---|
| `byte_source` † | `loader/byte-source.hpp` | **The storage abstraction.** Abstract random-access byte reader; the core never assumes how object bytes are stored. Backends are app-provided: semihosting file, memory-mapped flash blob, in-memory buffer, SD card, ... |
| `Error` / `result<T>` † | `loader/error.hpp` | Error codes + a minimal C++20 `expected`-like. `loader::unexpected(e)` builds the failure alternative. |
| ELF types † | `loader/elf-types.hpp` | ARM ELF32-LE structures, constants, relocation kinds (`Abs32`, `Rel32`, `Copy`, `GlobDat`, `JumpSlot`, `Relative`). |
| `elf_image` † | `loader/elf-image.hpp` | Validated view over a `.so`: header/program-header/section access, `image_size()`, `image_base()`, `find_section()`; `collect_relocations()` is `SCL_HOSTED`-only. |
| byte accessors † | `loader/detail/bytes.hpp` | Header-only, bounds-checked little-endian byte access into ELF spans: `load_word`/`load_byte`/`store_word` and `name_at` (NUL-terminated string read that never scans past the table). Used by the resolver/relocator/reloc-applier. |
| `dynamic_info` | `loader/dynamic-info.hpp` | `_DYNAMIC` segment parsing (DT_HASH/SYMTAB/STRTAB/JMPREL/INIT_ARRAY/...). |
| `symbol_resolver` | `loader/symbol-resolver.hpp` | SysV-hash (`.hash`) lookup over a (`.dynsym`, `.dynstr`) pair → `address_of(name)`. |
| `relocator` | `loader/relocator.hpp` | *Hosted-only* (`std::vector`): decodes `.rel.*` into a `relocation_plan` (used by host tests and the static loader). |
| `reloc_applier` | `loader/reloc-applier.hpp` | Allocation-free relocation application over raw spans — what freestanding firmware uses. Defines `symbol_source`, the extern-name→address callback (**the extension point for custom export tables**). |
| `LazyResolve` | `loader/lazy-resolve.hpp` | Lazy-binding fixup: map a `.got.plt` slot back to its JUMP_SLOT symbol and resolve on first call. |
| `region_allocator` | `loader/region-allocator.hpp` | Fixed-arena slot allocator for loaded images. |
| log hook | `scl/log.h` | `scl_log_puts/putc` — ALL SCL diagnostics go through this. Weak no-op defaults in `src/log.c`; the app overrides (e.g. to semihosting `trace_puts`). No override = silent loader. |

### 2.2 `scl::dynamic-nolib` — the runtime dynamic loader (`dlopen`-style)

ARM-only, **hard-float** (its port asm uses VFP push/pop). API in
`dynamic/dl.hpp`:

* `dlopen_source(byte_source&, rawname) -> result<handle*>` — **the only load
  entry.** Pipeline: parse ELF → allocate an arena slot → copy PT_LOAD segments
  → apply relocations (eager, lazy, or cross-call-routed) → register `.ARM.exidx`
  → run `.init_array`. The app wraps it with its storage front-end
  (`dlopen(path)` over semihosting, `dlopen_fs(name)` over a flash archive, ...).
* `dlsym(handle&, name)`, `dlclose(handle&)` — refcounted (a library imported
  by another loaded library refuses to close), runs `.fini_array`, frees the slot.
* `handle` — base/size, `got_base` (the `r10` value for entering the library),
  dependency edges, lazy tables, exidx info, a didactic GOT snapshot.
* Calling into a library: `pic_call_void/u32/u32_2(fn, got_base, ...)`
  (`scl/pic-trampoline.h`) set `r10` for single-pic-base libraries.
* Feature toggles (runtime): `dl_enable_cross_call(bool)` — route lib→lib
  JUMP_SLOTs through the `dl_cross_call` veneer so each callee runs with its own
  `r10`; `dl_cross_trace(int)` — 0/1/2 verbosity of `[xlat]` hop tracing.
* Diagnostics: `dl_dump_image`, `dl_dump_main_provides`,
  `dl_dump_got_walkthrough` (before/after GOT walkthrough).

**Link-time contract the application must satisfy** (see the freestanding test apps):

1. Firmware compiled `-fpic`, linked `-pie -Wl,-E` (re-export its dynamic
   symbols) and `-Wl,-Bsymbolic`; `link_context::from_self()` parses the
   firmware's own `_DYNAMIC` to resolve library imports against it.
2. `-Wl,-u,<sym>` anchors for every symbol only loaded libraries use
   (C++ EH runtime, libm floats, exported test surface) so `--gc-sections`
   keeps them.
3. The linker script reserves the image arena and defines
   `_simage_arena_start/_end` (consumed by `scl/image-memory.hpp`).
4. Loadable `.so` files are built `-fpic -msingle-pic-base -mpic-register=r10
   -nostdlib -shared` against `test/fixtures-src/ldscripts/gcc.ld`.

### 2.3 `scl::dynamic-newlib` — link-time binding + revertible GOT patching

ARM-only and **`SCL_HOSTED`-only** (uses `std::vector`/`unique_ptr` and the
newlib heap — a hosted CMSIS/HAL environment). API in
`static/dynamic-linker.hpp`:

* `dynamic_linker(const link_context&)`; `load_from_source(byte_source&) ->
  result<library*>`; `lookup(lib, name)`; `unload(lib)`.
* `patch_journal` — records every patched word (address + original value) so
  `unload()` **reverts** all patches (library GOT *and* firmware GOT).
* Bidirectional linking: `patch_main_got()` patches the *firmware's* GOT with
  symbols exported by a loaded library; a pending list re-resolves firmware
  slots when a later library provides the symbol.
* **The `_reloc` table** — how this loader finds the firmware's own symbol
  tables. The dynamic loader parses the firmware's `_DYNAMIC` segment; the
  static loader instead expects the application's linker script to emit a small
  fixed-layout table of address/size pairs:

  ```ld
  _reloc = .;
  LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
  LONG(ADDR(.dynsym));      LONG(SIZEOF(.dynsym));
  LONG(ADDR(.dynstr));      LONG(SIZEOF(.dynstr));
  LONG(ADDR(.hash));        LONG(SIZEOF(.hash));
  LONG(ADDR(.inits));       LONG(SIZEOF(.inits));
  ```

  `link_context` reads `_reloc` at boot and exposes those spans; symbol lookups
  against the firmware then use the same SysV-hash `symbol_resolver` as
  everything else. The linker scripts that produce this table (and keep
  `.dynsym`/`.rel.dyn` in the image) are vendored under
`test/platforms/board-hosted/vendor/common/ldscripts/{mem-sh,libs-sh}.ld`
   + `.../firmware/ldscripts/sections-dyn.ld`, used by the
   `board-hosted` test platform.
* Unresolved imports fall back to a **known-symbol table** inside
  `dynamic-linker.cpp` (libc entry points such as `printf`/`malloc`/`memcpy`,
  float libm, plus weak `AA`/`BB`/`s1` test globals the app may override) —
  handy in the hosted environment where newlib is linked in anyway.

### 2.4 `scl::overlay` — static linker-overlay window manager

Portable C (the `DSB/ISB` barrier compiles away off-target, so it is
host-testable). API in `overlay/overlay.h`:

* The app builds a `scl_overlay_module[]` descriptor table — window VMA, flash
  LMA, size, name — from symbols its linker script exports, then:
  `scl_overlay_init(&region, mods, n)`, `scl_overlay_load(&region, index)`
  (memcpy flash→window + `DSB;ISB`, no-op if already resident),
  `scl_overlay_current(&region)`.
* The linker-script side (an `OVERLAY : NOCROSSREFS` block aliasing one SRAM
  region, `LOADADDR/SIZEOF` exports) is the app's; copy and adapt the fully
  commented template `src/overlay/ldscripts/overlay-fragment.ld`.
* Overlay modules are **non-PIC with `-mlong-calls`** (they run in SRAM but
  call flash across >BL reach) — see `scl-fw-ovl1` (`test-overlay/test1`).

### 2.5 `scl::static` — standalone ELF32 static patcher/loader

The in-house static patcher (imported from the former `shared-static-rework`
project). It loads a PIC `.so` into a RAM arena, resolves symbols via SysV-hash
lookup, applies relocations and **GOT-patches** the image so a bare-metal
STM32F411 with no MMU/OS can call into it. Code in `src/static/`, headers in
`include/static/`:

* `elf_reader` (`elf-reader.{h,cpp}`) — a **thin adapter over the shared
  `loader::elf_image`** parser (same `common-elf-reader` submodule as
  `scl::common`); adds this loader's helper-section map, `pass_args`, and the
  SysV-hash `find_symbol_address`.
* `shlib_loader` (`shlib-loader.{h,cpp}`) — segment load, `patch_shlib` /
  `patch_main` GOT patching, `R_ARM_*` relocation fixup, weak-symbol resolution,
  `.init_array` constructors, `DT_NEEDED` dependency loading.
* `logger` (`logger.{h,cpp}`) — loader-side logging support.

INTERFACE target linking `common-elf-reader::elf-reader`; the loader + parser
compile into the consumer with its flags (no `.a`). The demo firmware app that
exercises it lives in **`test-static/test-static/`** (its own `main.cpp`, `system/`,
`ldscripts/` — the `_reloc`-table model — and the `libsh1/2` demo `.so`); it is
a standalone CMake project (and Makefile) that links `scl::static`. See
`test-static/test-static/doc.md`.

### 2.6 `scl::reloc` — ELF32 relocatable (ET_REL) module loader

A loader for **relocatable** objects — plain `.o`s built `-mlong-calls -fno-pic
-r`, the way a Linux kernel module or a NuttX loadable module is loaded, rather
than `-fPIC -shared` ET_DYN objects. Code in `src/reloc/`, header in
`include/reloc/`:

* **No GOT, no PLT, no `_DYNAMIC`, no r10.** `-fno-pic` means the module runs at
  fixed absolute addresses; every external reference is an **`R_ARM_ABS32`**
  literal (`ldr rN,=target ; blx rN`), so `-mlong-calls` removes the >BL-range
  problem and no trampoline is needed.
* **`load` / `load_set`.** `load` places one module; **`load_set`** links a group
  in two phases (place all → resolve all → run all `.init_array`) so
  **cyclically dependent** modules work — the *eager* counterpart of the ET_DYN
  loaders' lazy `dl_cross_call`. Relocations handled: `R_ARM_ABS32` / `TARGET1` /
  `REL32` / `THM_CALL`; `.symtab` resolution (defined = section base + `st_value`
  with the Thumb bit for `STT_FUNC`; undefined = a `loader::symbol_source`).
* Reuses the shared `loader::symbol_source` and byte helpers; ~2 KB `.text`.

The demo lives in its own top-level tree **`test-relocatable/`** (a self-contained
**non-PIC** firmware — its own `platform/reloc.ld`, no `-pie`/`-Wl,-E`), with a
9-check battery (relprov+relmod, and the cyclic c1+c2+cpp3 set via `load_set`)
that produces values **bit-identical to the ET_DYN `dyn2` battery**. See
`test-relocatable/` and [`build-linking-guide.md`](doc/build-linking-guide.md) §ET_REL.

---

## 3. Configuration reference

### CMake options (SCL package)

| Option | Default | Meaning |
|---|---|---|
| `COMMON_ELF_READER_DIR` | `./common-elf-reader` | Path to the ELF32 reader/parser, a **git submodule** at `./common-elf-reader` (`elf32_read_parse.git`). Brought in via `add_subdirectory` as the `common-elf-reader::elf-reader` **INTERFACE** target, which `scl-common` links — so `src/elf-image.cpp` and its `include/` propagate transitively and are compiled **from source** into each consumer (no `.a` archive). `SCL_HOSTED` is inherited. Configure fails fast if it is missing. |
| `SCL_HOSTED` | ON on host, OFF when cross-compiling | Builds the parts that need a hosted C++ library (`std::vector`): `relocator`, `elf_image::collect_relocations()` (guarded by the `SCL_HOSTED` compile definition), and the whole `scl::dynamic-newlib` target. Freestanding firmware (`-ffreestanding -nostdlib`) must keep it OFF and relocate via `reloc_applier`. Hosted newlib firmware (static-unify style) sets it ON. |
| `SCL_BUILD_TESTS` | ON when SCL is the top-level project | Enables CTest registration. Never on for cross builds. |

### Toolchain files (consumed by apps and the test project)

| File | Environment | Key flags |
|---|---|---|
| `cmake/arm-none-eabi.cmake` | freestanding dynamic/overlay firmware | Cortex-M4 **hard-float**, `-fpic`, `-ffreestanding -fno-tree-loop-distribute-patterns`, exceptions/RTTI ON (the firmware provides the C++ EH runtime to loaded libs). Exports `SCL_MCU_FLAGS`. |
| `cmake/arm-none-eabi-hosted.cmake` | hosted newlib firmware (static loader) | Cortex-M4 **soft-float** by default (`-DSCL_HF=ON` for hard), non-PIC, `-fno-exceptions -fno-rtti`, nano-spec friendly. |

Both default `SCL_ARM_TOOLCHAIN_BIN` to the xPack GCC 15.2.1 install; override
with `-DSCL_ARM_TOOLCHAIN_BIN=/path/to/bin` or have `arm-none-eabi-gcc` on PATH.

For a flag-by-flag rationale of these toolchain files, the linker scripts, and
the `.so`/firmware link recipes across all three repos (shared-static-rework,
this SCL, and common-elf-reader), see
[`build-linking-guide.md`](doc/build-linking-guide.md).

### The logging hook

Implement in the application (strong definitions override the weak no-ops):

```c
#include "scl/log.h"
void scl_log_puts(const char* s) { trace_puts(s); }
void scl_log_putc(char c)        { trace_putc(c); }
```

---

## 4. Tests: what each one is, what it needs, how to build and run

### 4.0 Building everything from scratch (full clean build)

All commands run from the **repo root** (the SCL content is the root). Fixtures
are built automatically by the CMake fixture targets — no manual `make` and no
sibling folders are needed.

```sh
# ARM toolchain on PATH (the vendored fixture Makefiles call objdump directly)
export PATH=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH

# 0) optional: full clean
./cleaner.sh   # or: rm -rf test-dynamic/build test/build-dynamic-newlib test/build-dynamic-newlib-hf \
       test/platforms/board-hosted/vendor/libs/build*

# 2) freestanding firmware apps — dyn1, dyn2, flatfs, ovl1
cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-dynamic/build -j                                     # 4 .hex/.elf/.bin

# 3) hosted static apps — static1, static2 (fixtures build automatically)
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake
cmake --build test/build-dynamic-newlib -j                                # SOFT float (build/  = 11 .so)
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib-hf -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake -DSCL_HF=ON
cmake --build test/build-dynamic-newlib-hf -j                             # HARD float (build_hf/ = 13 .so, +hf_test1/2)

# 4) run on the board (WeAct BlackPill STM32F411CE attached)
test/tools/flash-run.sh scl-fw-dyn1    # dyn2 | flatfs | ovl1
test/tools/flash-run-sf.sh scl-fw-dynamic-newlib1   # or static2 — SOFT float
test/tools/flash-run-hf.sh scl-fw-dynamic-newlib2   # or static1 — HARD float
```

The static apps come in two float-ABI builds; always flash them through the
matching wrapper (`flash-run-sf.sh` / `flash-run-hf.sh`), which re-stages the
correct fixtures into `/tmp` and verifies the ABI before flashing.


### 4.2 The two test platforms (hosted vs freestanding)

The SCL's loaders were born in two very different firmware environments, so the
test tree ships two "platform libraries" (xpack-style INTERFACE targets that
contribute sources, include paths and the link recipe to any app that links
them):

| | `platform::board` (freestanding) | `platform::board-hosted` |
|---|---|---|
| Runtime | none: ~400 lines of bare metal (`startup.c`, `board.c`, `semihost.c`, `libc-shim.c`, `crt.cpp`) | full CMSIS + ST HAL + newlib-nano (heap, `printf`, C++ library) |
| Code model | `-fpic -pie`, firmware exports symbols via `-Wl,-E`, `_DYNAMIC` parsed at boot | non-PIC, symbols discovered through the `_reloc` table |
| Float ABI | hard-float only (the dynamic port asm uses VFP) | soft by default, `-DSCL_HF=ON` for hard |
| Toolchain file | `cmake/arm-none-eabi.cmake` | `cmake/arm-none-eabi-hosted.cmake` |
| Used by | `scl-fw-dyn1/2`, `scl-fw-ovl1`, `scl-fw-flatfs` | `scl-fw-dynamic-newlib1/2` |
| Where the files live | `test/platforms/board/{src,include,ldscripts}` | vendored under `test/platforms/board-hosted/vendor/` |

Because the two environments need different *global* compiler flags, they
cannot share one CMake build tree: `test-dynamic/build` holds the freestanding apps, and
the static apps configure their own tree with the hosted toolchain
(inside `test-dynamic/build` they self-skip with a status message).

### 4.3 On-target firmware test apps (`test/test-*`) — board required

Each `testN` is a **complete Cortex-M4 firmware application** (its `src/main.*`
+ the `platform::board` interface library + the SCL), flashed and run
on a WeAct BlackPill STM32F411CE through OpenOCD with the semihosting console as output.

Build **all** freestanding apps (from the repo root):

```sh
cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-dynamic/build -j          # every app -> .elf + .hex + .bin
```

Build **one** app standalone (each testN CMakeLists bootstraps itself):

```sh
cmake -S test-dynamic/test-dynamic-nolib/test1 -B /tmp/t1 \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build /tmp/t1 -j
```

Run (board attached; `ctest` invokes `test/tools/flash-run.sh`, which flashes
the `.hex` — never the `.elf`, whose PIE header page OpenOCD cannot place —
enables semihosting **before** `reset run`, and streams the console; Ctrl-C to
stop):

```sh
ctest --test-dir test-dynamic/build -R scl-fw-dyn1        # or, by app name:
test/tools/flash-run.sh scl-fw-dyn1
test/tools/flash-run.sh scl-fw-flatfs
test/tools/flash-run.sh path/to/any.hex         # or an explicit path
```

| App | What it runs | Specific configuration it needs |
|---|---|---|
| `scl-fw-dyn1` (`test-dynamic-nolib/test1`) | the demo: dlopen/dlsym/call/dlclose `mylib1/2.so`, `libcppsh.so` (C++), `libcppeh.so` (exceptions across the boundary), reload proof | links `scl::dynamic-nolib`; `sections.ld`; fixtures served over **semihosting** from `test/fixtures/` (absolute `DL_LIB_DIR` is injected, so the OpenOCD CWD does not matter); default C++ EH anchors |
| `scl-fw-dyn2` (`test-dynamic-nolib/test2`) | the 16-test cross-module battery: c1/c2/cpp3, cross-library calls (`r10` routing), lazy vs eager binding, cross-module exceptions, GOT/PLT dump walkthroughs | additionally `LINK_LIBM` + anchors `sinf cosf logf sqrtf` (cpp3's hard-float math resolves against main) and `trace_event trace_u32 fm_callback fm_var fm_info` (the FM-exported test surface that only loaded libs reference) |
| `scl-fw-ovl1` (`test-overlay/test1`) | overlays-vs-dynamic comparison: same three scenarios through `scl::overlay` (link-time binding, one shared SRAM window) and through `scl::dynamic-nolib` (runtime relocation), addresses printed side by side | links `scl::dynamic-nolib` **and** `scl::overlay`; `LDSCRIPT sections-overlay.ld` (the `OVERLAY : NOCROSSREFS` block + `OVL` memory region); same battery anchors + libm |
| `scl-fw-dynamic-newlib1` (`test-dynamic-newlib/test1`) | static loader (`scl::dynamic-newlib`) battery: load `mylib1.so` over semihosting, `dlsym`+call, negative lookup, `dlclose`; reload proves the patch_journal reverted every patch; missing-file error path | **separate build tree** with the HOSTED toolchain (`cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake`) — self-skips inside the freestanding `test-dynamic/build`; platform = `board-hosted` (vendored CMSIS/HAL/newlib + `_reloc` ldscripts); fixture `.so` built automatically; `-DSCL_HF=ON` for the hard-float variant |

To add an app: create `test/test-<flavour>/testN/{src,include,CMakeLists.txt}`
with

```cmake
scl_test_app(scl-fw-<name>
    LIBS scl::dynamic-nolib            # SCL targets
    LDSCRIPT sections.ld         # from the platform's ldscripts/
    ANCHORS extra_sym ...        # extra -Wl,-u anchors
    LINK_LIBM)                   # if the app's libs need float libm
```

### 4.4 What each test app verifies

flash-run.sh accepts exactly these six names (typos get the list back; a
missing hex prints the build commands).

**`scl-fw-dyn1`** — the dynamic-loader **demo walk-through**. Sequentially:
dlopen/dlsym/call/dlclose of `mylib1.so` (C, single-pic-base; its constructors
print via the exported printf shim), a reload proving the arena slot is reused,
`mylib2.so`, `libprov.so`+`libcons.so` (inter-library imports: `dlclose refused`
while a dependent is loaded, then the 5c lazy-binding proof — fixup count rises
exactly once on the first call), `libcppsh.so` (C++ objects allocated on main's
heap), and `libcppeh.so` (5d: exception thrown inside the library, caught across
the module boundary; `dynamic_cast` across images). Ends with
`f411ce-dynloader 4b: done`.

**`scl-fw-dyn2`** — the
16-test **cross-module battery** over `c1.so`/`c2.so`/`cpp3.so` (+`libcppeh`/
`ehx_catch`), with cross-library r10 routing enabled:

| # | Verifies |
|---|---|
| 1 | FM → c1 → c2 call chain (r10 swap per hop) |
| 2 | FM → c2 → c1 → FM (call-back into main) |
| 3 | FM → cpp3 hard-float math (sinf/cosf/logf/sqrtf from main's libm) |
| 4 | main reads a variable + const exported by c2 |
| 5 | c1 reads main's `fm_var` + `fm_info` struct |
| 6 | 3-hop chain cpp3 → c2 → cpp3 → FM with C++ class state |
| 7 | cpp3 method with params sourced from c2 + verbose GOT/PLT walkthrough |
| 8 | `dlclose` refused while a cross-library dependent is loaded |
| 9 | library state persists across calls, resets on reload |
| 10 | exception unwinds lib → `dl_cross_call` → other lib's catch |
| 11 | lazy PLT fixup fires exactly once, never again |
| 12 | loader error contract: FileNotFound / NotElf (`notelf.bin`) / SymbolNotFound |
| 13 | heap: new/delete + malloc/free, reuse, cross-module allocation |
| 14 | deep mutual cross-recursion cpp3 ↔ c2 (depth 6) |
| 15 | float value crosses cpp3 → c2 through the trampoline |
| 16 | two cpp3 instances keep independent .bss / r10 |

Expected tail: `== BATTERY SUMMARY: 16/16 PASS ==`.

**`scl-fw-ovl1`** — the **overlays-vs-dynamic comparison**: the same three scenarios run
twice, once via `scl::overlay` (link-time binding, one shared SRAM window at
`0x20018000`, `[ovl-load]` traces show each flash→window copy) and once via
`scl::dynamic-nolib` (runtime relocation): (1) module reads main's var/struct + calls
main, (2) main reads module-exported `.data`/`.rodata`, (3) C++ class method +
exported const, (4) the headline contrast — all overlay entry points ALIAS one
window VMA while dynamic libs get independent bases, plus (5) hard-float and
(6) heap variants. Expected tail: `== OVERLAY-COMPARISON SUMMARY: 6/6 PASS ==`.

**`scl-fw-flatfs`** — the **storage-backend
proof**: boot checks the on-chip flat archive (sectors 6–7). If absent, the
load-module provisions it (reads each `.so` once over semihosting, programs it
into flash with per-blob CRC and a commit-marker TOC write); if present
(`flat_fs already present`), nothing is written — the archive survives
code-only reflashes. Then the same 16-test battery runs with every dlopen
routed through `dlopen_fs(name)` → `Flashbyte_source` (memory-mapped flash, no
semihosting reads). Expected tail: battery `16/16 PASS` after a
`[boot] flat_fs files = 6` header.

**`scl-fw-dynamic-newlib1`** — the **static loader
(`scl::dynamic-newlib`) battery**: T1 load `mylib1.so` via `load_semihost` →
`dynamic_linker::load_from_source`, `dlsym` + call `library_function(42)`,
negative symbol lookup, `dlclose`; T2 immediately reload — passes only if the
**patch_journal reverted every GOT/memory patch** of the first cycle (identical
result proves a pristine state); T3 missing file rejected with a clean
`dlerror()`. Its constructors print through the loader's known-symbol fallback
(newlib printf). Expected tail: `RESULT: 3 passed, 0 failed -> SUCCESS`.

**`scl-fw-dynamic-newlib2`** — the full
static-loader suite through the `dl::` facade: GOT/PLT patching
step-by-step (`test_gotplt`), concurrent libraries, coverage libs (`cov1/2`),
`mylib1/2`, cross-library calls, `libcppsh`, `c_test1/2` + their cross tests,
C++ `cpp_test1` + cross-C++, a complex multi-import scenario, and (in the HARD
float build, `flash-run-hf.sh`) the hard-float pair `hf_test1/2`. The suite
dlopens `/tmp/*.so`, staged by the flash wrapper (soft or hard set to match the
firmware ABI — see §4.0). Expected tail: `ALL OK — blink`.

### 4.5 Fixtures

* `test/fixtures/` — committed prebuilt ARM `.so` files used by the host suites
  and the freestanding target apps (opened over semihosting).
* `test/fixtures-src/` — their sources (C libs, C++ libs, the pic linker
  scripts). The freestanding apps use the committed `.so`; to regenerate them,
  build the fixture libraries with the toolchain on PATH and copy into
  `test/fixtures/`.
* Static-app fixtures live under `test/platforms/board-hosted/vendor/`
  (`libs/`, `debug/test_libs`, `debug/test_hardfp`) and are built automatically
  by the CMake fixture targets into `vendor/libs/build` (soft) / `build_hf`
  (hard).

---

## 5. The application-side API, per loader type

What a `main()` actually calls, flavour by flavour — the exact call sequences
the test apps exercise. (See [`scl-guide.md`](doc/scl-guide.md) for the plain-English walkthrough.)

**Dynamic loader** (`test-dynamic-nolib/test{1,2}/src/main*.cpp` → `scl::dynamic-nolib`):

```cpp
// app-side wiring (platform provides these):
extern "C" void scl_log_puts(const char*);        // log hook -> trace_puts
fw::dlopen(path)                                  // app front-end (dl-semihost.hpp):
                                                  //   semihost_file -> fw::dlopen_source(src, path)
// SCL API (dynamic/dl.hpp):
loader::result<fw::handle*> h = fw::dlopen(DL_LIB_DIR "/mylib1.so");
loader::result<void*> fn = fw::dlsym(**h, "library_function");
uint32_t r = pic_call_u32(*fn, (void*)(*h)->got_base, 39);   // enter lib with r10 set
fw::dlclose(**h);                                 // dtors, refcount-guarded, frees slot

fw::dl_enable_cross_call(true);                   // lib->lib r10 routing (battery)
fw::dl_cross_trace(2);                            // [xlat] hop tracing 0|1|2
fw::dl_fixup_count();                             // lazy-binding proof (test 11)
fw::dl_dump_image(h); fw::dl_dump_got_walkthrough(h);   // GOT/PLT diagnostics
```

**Flat-fs storage backend** (`main-flatfs.cpp` — app-owned, over `scl::dynamic-nolib`):

```cpp
// archive management (fs-archive.hpp, app code):
fw::fs_valid();                                   // superblock magic/version ok?
fw::fs_format();                                  // erase sectors 6..7, write superblock
fw::fs_add("c1.so", data, len);                   // program blob + commit TOC slot
fw::fs_find("c1.so"); fw::fs_entry(i);            // TOC lookup / iteration
fw::fs_blob(e); fw::fs_count(); fw::fs_used();    // memory-mapped blob + stats
// loading from flash (dl-fs.hpp -> Flashbyte_source -> the same dlopen_source):
loader::result<fw::handle*> h = fw::dlopen_fs("c1.so");
g_use_flatfs = true;                              // battery hook: route dlopen -> dlopen_fs
```

**Overlay manager** (`main-overlay.cpp` → `scl::overlay`):

```cpp
scl_overlay_module mods[3] = {                    // from sections-overlay.ld symbols
    { _ovl_vma, _ov1_lma,   (uint32_t)(uintptr_t)_ov1_size,   "ov1"   }, ... };
scl_overlay_region ovl;
scl_overlay_init(&ovl, mods, 3);
scl_overlay_load(&ovl, 0);                        // memcpy flash->window + DSB/ISB
scl_overlay_current(&ovl);                        // resident index or SCL_OVERLAY_NONE
uint32_t r = ov1_use_fm();                        // window entries are long_call
```

**Static loader** (`main.cpp` DYN / `main-no-dyn.cpp` → `scl::dynamic-newlib`, hosted):

```cpp
// POSIX-style facade (dl-impl.hpp, app code over the SCL dynamic_linker):
dl::handle h = dl::dlopen("/tmp/mylib1.so");      // nullptr on error
auto fn = (unsigned(*)(unsigned))dl::dlsym(h, "library_function");
dl::dlclose(h);                                   // patch_journal reverts every patch
const char* err = dl::dlerror();                  // last error string, then cleared

// or the class API directly (static/dynamic-linker.hpp):
firmware::link_context ctx;                        // reads the _reloc table
firmware::dynamic_linker linker(ctx);
auto lib = firmware::load_semihost(linker, path); // app front-end (dl-load.hpp)
auto sym = linker.lookup(*lib, "name");           // firmware -> libs -> cross search
linker.unload(*lib);                              // journal revert + free
```

The flat-fs storage backend (`fs_archive.*`, `flash_prog.*`,
`flash-byte-source.hpp`, `dl_fs.{hpp,cpp}`) and the overlay driver are all
**application-side** code, included in `scl-fw-flatfs` / `scl-fw-ovl1` as
worked examples of the two extension points (a custom `byte_source` and the
overlay descriptor table).

---

## 6. Integrating the SCL into a new application

```cmake
add_subdirectory(path/to/cortexm-shared-loader scl)
target_link_libraries(my-fw PRIVATE scl::dynamic-nolib)   # and/or scl::overlay / scl::dynamic-newlib
```

The application must provide: board init/startup/clocks, linker scripts
(arena symbols; `-Wl,-E`/anchors for the dynamic loader; the `_reloc` table for
the static loader; the OVERLAY block for overlays), a `byte_source`-based
storage front-end, and (optionally) the `scl_log` hook. Copy the pattern from
`test/platforms/board/` — it is a complete worked example of every
application responsibility.

---

## 7. Requirements reference — constants, paths, tools, build & link per component

### 7.1 Tools and binaries (defaults; every path overridable)

| Tool | Default location | Used by | Override |
|---|---|---|---|
| `arm-none-eabi-gcc/g++/objcopy/size/gdb/nm` | `/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin` | all cross builds | `-DSCL_ARM_TOOLCHAIN_BIN=<dir>` (CMake), `ARM_TOOLCHAIN_BIN=<dir>` (Makefiles), or PATH |
| `openocd` + its `scripts/` | `/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/{bin,openocd/scripts}` | `test/tools/flash-run*.sh` | env `OPENOCD=`, `OPENOCD_SCRIPTS=` |
| host `g++` (C++20) + `cmake` ≥ 3.20 + `ctest` | system | all CMake builds | `CXX=` |
| `make` | system | invoked *by CMake* to build the static-app fixture `.so` (vendored `libs/`, `debug/test_libs`, `debug/test_hardfp`); needs the toolchain on PATH (its Makefiles call `objdump`) | — |
| board config | `test/platforms/board/openocd.cfg` (CMSIS-DAP + `connect_assert_srst`) | all on-target runs | edit / `-f` another cfg |

### 7.2 Compile-time constants (#define) — who defines what

| Constant | Defined by | Consumed by | Meaning |
|---|---|---|---|
| `DL_LIB_DIR` | each dynamic/overlay app's build (CMake `target_compile_definitions` / Makefile `-DDL_LIB_DIR='"..."'`) — always an **absolute** path | app mains (`dlopen(DL_LIB_DIR "/x.so")`) | directory of the `.so` fixtures opened over semihosting; absolute so the OpenOCD CWD is irrelevant. Falls back to `"."` if undefined |
| `SCL_HOSTED` (=1) | SCL CMake when the option is ON | `loader/elf-image.hpp` (guards `collect_relocations`), SCL CMake (compiles `relocator.cpp`, defines `scl-dynamic-newlib`) | hosted C++ library available (`std::vector`) |
| `SCL_SO_DIR` | `test-dynamic-newlib/test1` CMake | its `main.cpp` | absolute dir of the static-app fixture `.so` (`vendor/libs/build` or `build_hf`) |
| `DYN` / `NO_DYN` | `test-dynamic-newlib` CMake (`DYN`) | `main.cpp`, `dl_impl` | selects runtime-dlopen vs link-time-PLT variant |
| `HARD_FLOAT` | hosted platform when `-DSCL_HF=ON` | test selection (`tests-dyn.cpp`) | enables the hard-float test set, skips soft-only ones |
| `DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG STM32F411xE USE_HAL_DRIVER HSE_VALUE=8000000` | hosted platform / static-unify builds | CMSIS/HAL/diag-trace | standard µOS-style HAL configuration |
| `DL_LIB_DIR`-style fixture constants in the SCL apps | `scl_test_app` sets `DL_LIB_DIR="<scl>/test/fixtures"` | test app mains | one shared fixture pool |
| `kGotSnapMax=32`, handle pool =4, `deps[4]`, `name[12]` | `include/dynamic-nolib/dl.hpp` (compile-time constants) | dynamic loader | didactic GOT snapshot size, max concurrently loaded libs — future CMake options |
| `kFsMagic/kFsVersion/kFsTocCap=64/kFsNameMax=16` | `test-dynamic/test-dynamic-nolib/test3/include/fs-archive.hpp` (app code) | flat-fs backend | on-flash archive format |
| `SCL_OVERLAY_NONE` (=-1) | `overlay/overlay.h` | overlay users | "no module resident" |

Runtime (not #define) knobs of `scl::dynamic-nolib`: `dl_enable_cross_call(bool)`, `dl_cross_trace(0|1|2)`; the `scl_log_puts/putc` hook (weak no-ops unless the app overrides — every test app maps them to semihosting `trace_puts/putc`).

### 7.3 Per-library build requirements

| library | Language/flags it needs | Link-time contract of the consuming firmware |
|---|---|---|
| `scl::common` | C++20, portable; hosted parts only with `SCL_HOSTED` | none |
| `scl::dynamic-nolib` | Cortex-M4 **hard-float** (`port/` VFP asm), consumer compiled `-fpic` | `-pie -Wl,-E -Wl,-Bsymbolic --gc-sections`; `-Wl,-u,<sym>` anchors for every symbol only loaded libs use (EH runtime `__cxa_* __gxx_personality_v0 __dynamic_cast`; `printf puts` for the shim; battery: `sinf cosf logf sqrtf trace_event trace_u32 fm_callback fm_var fm_info`); linker script defines `_simage_arena_start/_end`; link `libsupc++.a libgcc.a` (+`libm.a` for float libs) resolved via `-print-file-name` |
| `scl::dynamic-newlib` | hosted (newlib heap), `SCL_HOSTED=ON`; soft or hard float | linker script emits the `_reloc` table and keeps `.dynsym/.dynstr/.hash/.rel.dyn` (`mem-sh.ld` + `libs-sh.ld` + `sections-dyn.ld`); `--specs=nano.specs -u _printf_float`; link `stdc++ m` |
| `scl::overlay` | portable C | app linker script provides the `OVERLAY : NOCROSSREFS` block + `LOADADDR/SIZEOF/ADDR` symbols (template: `src/overlay/ldscripts/overlay-fragment.ld`) and an `OVL` memory region |

### 7.4 Per-test-app requirements matrix

| | toolchain file | platform | SCL libs | ldscript(s) | fixtures (runtime) | extra anchors / libs | defines |
|---|---|---|---|---|---|---|---|
| `scl-fw-dyn1` | `arm-none-eabi.cmake` | `board` | `scl::dynamic-nolib` | `sections.ld` (+`mem.ld`) | `test/fixtures/{mylib1,mylib2,libcppsh,libprov,libcons,libcppeh}.so` | `printf puts` (+ EH set) | `DL_LIB_DIR` |
| `scl-fw-dyn2` | `arm-none-eabi.cmake` | `board` | `scl::dynamic-nolib` | `sections.ld` | `test/fixtures/{c1,c2,cpp3,libcppeh,ehx_catch}.so` + **`notelf.bin`** (TEST12) | `sinf cosf logf sqrtf trace_event trace_u32 fm_callback fm_var fm_info` + `LINK_LIBM` (+ EH set) | `DL_LIB_DIR` |
| `scl-fw-ovl1` | `arm-none-eabi.cmake` | `board` | `scl::dynamic-nolib` + `scl::overlay` | `sections-overlay.ld` (+`mem-overlay.ld`: `OVL` @ 0x20018000/16K) | `test/fixtures/{c1,c2,cpp3}.so` | battery set + `LINK_LIBM`; overlay modules compiled `-fno-pic -mlong-calls` | `DL_LIB_DIR` |
| `scl-fw-flatfs` | `arm-none-eabi.cmake` | `board` | `scl::dynamic-nolib` | `sections-flatfs.ld` | provisions from `test/fixtures/` (c1,c2,cpp3,libcppeh,ehx_catch,notelf.bin) into flash sectors 6–7 | battery set + `printf puts` + `LINK_LIBM`; backend srcs in `test3/{src,include}` | `DL_LIB_DIR` |
| `scl-fw-dynamic-newlib2` | `arm-none-eabi-hosted.cmake` (`test/build-dynamic-newlib[-hf]`) | `board-hosted` | `scl::dynamic-newlib` | vendored `mem-sh`+`libs-sh`+`sections-dyn.ld` | full set staged into **/tmp** by the flash wrapper (soft `build/` / hard `build_hf/`); built by the CMake fixture targets | nano.specs set | `DYN`, HAL set, `SCL_HOSTED` |
| `scl-fw-dynamic-newlib1` | `arm-none-eabi-hosted.cmake` (`test/build-dynamic-newlib[-hf]`) | `board-hosted` (vendored) | `scl::dynamic-newlib` | vendored `mem-sh`+`libs-sh`+`sections-dyn.ld` | `vendor/libs/build[_hf]/mylib1.so` (auto-built), read via `SCL_SO_DIR` (no /tmp) | `--specs=nano.specs -u _printf_float`, `stdc++ m` | `DYN`, `SCL_SO_DIR`, HAL set, `SCL_HOSTED` |
| `scl-fw-xip-*` | `arm-none-eabi.cmake` | `board-hosted` | `scl::xip-*` | `sections-flatfs-hosted.ld` | XIP-compatible fixtures (built with `build-xip-libs.sh`) | `DL_LIB_DIR` | `HARD_FLOAT` |
| `scl-fw-xip-static-spi-flash` | `arm-none-eabi.cmake` | `board-hosted` | `scl::xip-common` | `sections-flatfs-hosted.ld` | XIP-compatible test-drv fixtures (built with `build-xip-drv.sh`) | `DL_LIB_DIR` | `HARD_FLOAT` |

All on-target apps additionally need: the board on USB (CMSIS-DAP (0x1a86:0x8011)), the `.hex` (never the `.elf` — PIE header page), `arm semihosting enable` before `reset run`, and no `verify` (its target CRC double-faults this core). The flat-fs firmware's 256K filesystem lives in sectors 6–7 and **survives code-only reflashes**; re-provisioning happens only when the archive is absent (or after `fs_format()`).

### 7.5 Hardware validation status (WeAct BlackPill STM32F411CE)

`scl-fw-dyn1` ✔, `scl-fw-dyn2` 16/16 ✔, `scl-fw-ovl1` 6/6 ✔,
`scl-fw-flatfs` 16/16 ✔ (archive detected, loads from flash),
`scl-fw-dynamic-newlib1` 3/3 ✔ (incl. patch_journal-revert reload),
`scl-fw-dynamic-newlib2` ✔ (`ALL OK`, soft + hard float via the matching wrappers).
Host suites 6/6 ✔.

XIP tests: `scl-fw-xip-nolib1` ✔, `scl-fw-xip-newlib1` ✔, `scl-fw-xip-static-semihosting` ✔,
`scl-fw-xip-static-direct` ✔, `scl-fw-xip-interrupt-static` ✔,
`scl-fw-xip-interrupt-static-direct` ✔, `scl-fw-xip-static-spi-flash` ✔,
`scl-fw-xip-interrupt1` ✔.

### 7.6 SPI Flash (W25Q64)

The WeAct BlackPill board has an on-board **W25Q64** (8 MB) SPI NOR flash connected to SPI1:

| Signal | Pin  | Function                |
|--------|------|-------------------------|
| CS     | PA4  | GPIO output, active low |
| SCK    | PA5  | SPI1 clock (AF5)        |
| MISO   | PA6  | SPI1 data in (AF5)      |
| MOSI   | PA7  | SPI1 data out (AF5)     |

Two driver implementations are available:
- **Bitbang** (`test-dynamic/test-flatfs-bitbang-spi/`) — raw GPIO toggling, no HAL dependency
- **HAL SPI** (`test-dynamic/test-flatfs-spi/`) — STM32F4 HAL at 25 MHz (APB2/4)

The SPI flash is used by the flat-fs storage backend (`scl-fw-flatfs-spi`, `scl-fw-flatfs-bitbang-spi`) to store `.so` modules persistently. The archive format uses sectors with a TOC (table of contents) and per-blob CRC32.

See [`test/SPI.md`](test/SPI.md) for the full driver comparison, clock tree, filesystem format, and problem/solution log.

### 7.7 XIP (Execute-In-Place) Architecture

The XIP loaders allow code to execute directly from flash while only copying writable data to RAM. This saves RAM and enables loading larger modules.

#### XIP-Compatible Module Build Requirements

Modules must be built with specific flags to support XIP:

```bash
# Required flags for XIP-compatible .so modules
-fpic -msingle-pic-base -mpic-register=r10   # Single PIC base register
-T xip-split.ld                              # Split R-X | RW segments
-Wl,-E -Wl,-Bsymbolic                        # Export symbols, bind symbolically
```

The `xip-split.ld` linker script creates two separate PT_LOAD segments:
- **R-X segment** (text): stays in flash, executed in place
- **RW segment** (data): copied to RAM at load time

#### XIP Loader Behavior

The `xip::stat::load()` function supports two modes:

1. **XIP mode** (preferred): Text in flash, data in RAM
   - Requires: split segments, VA == file_offset, no relocations in R-X
   - Output: `[xip] load: using XIP mode (text in flash, data in RAM)`

2. **Copy-whole fallback**: Everything in RAM
   - Used when: segments not split, fused RWX, VA mismatch
   - Output: `[xip] load: XIP not possible, using copy-whole mode`

#### XIP Test Applications

| Test | Description | Key Features |
|------|-------------|--------------|
| `test-xip-static` | Static loader XIP test | Basic XIP with scl::Module |
| `test-xip-static-direct` | Direct API calls XIP | Uses call_sym*, rd, rd_str helpers |
| `test-xip-interrupt-static` | XIP + interrupts | TIM2 interrupt handler with XIP |
| `test-xip-interrupt-static-direct` | Direct calls + interrupts | Combined approach |
| `test-xip-static/spi-flash` | SPI flash storage + XIP | Full pipeline: semihosting → SPI → internal flash → XIP |

#### SPI Flash as Transit Storage

The `test-xip-static/spi-flash` test demonstrates using SPI flash as intermediate storage:

```
Semihosting → SPI flash (flatfs) → Internal flash (flatfs) → XIP execution
```

Key points:
- SPI flash is **only** for storage, never for code execution
- All drivers (SPI, UART, LED) are loaded as `.so` modules
- Function pointers resolved at runtime from loaded modules
- Supports both XIP and copy-whole modes

---

## 8. Git layout

This standalone repo lives in a **bare repository**; you work in a normal clone
of it:

```
~/Downloads/GIT/cortexm-shared-loader.git   # the BARE repository (branch: main)
<your checkout>/                            # a clone of it (origin -> the bare)
```

### 8.1 Cloning and working

```sh
# Clone this repo AND the shared parser beside it (default COMMON_ELF_READER_DIR).
git clone ~/Downloads/GIT/elf32_read_parse.git       common-elf-reader
git clone ~/Downloads/GIT/cortexm-shared-loader.git  cortexm-shared-loader
cd cortexm-shared-loader
# ... edit, commit as usual ...
git push origin main
```

(Or keep `common-elf-reader` elsewhere and pass
`-DCOMMON_ELF_READER_DIR=/abs/path` at configure time.)

### 8.2 Hard-learned rules

* **Never keep the bare repo in a volatile location.** An earlier setup kept a
  bare repo in `/tmp`; a `/tmp` cleanup destroyed the entire history. Keep the
  bare somewhere durable (here: `~/Downloads/GIT/`), and ideally add an external
  remote (GitHub/GitLab) and push regularly:

  ```sh
  git remote add github git@github.com:<you>/cortexm-shared-loader.git
  git push -u github main
  ```

* Build trees (`build-fw`, `build-dynamic-newlib[-hf]`) and the vendored
  fixture outputs (`vendor/libs/build*`) are git-ignored — only sources are
  tracked.


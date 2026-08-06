# SCL Analysis & Migration Plan — Cortex-M Shared-library Loaders

Companion to `scl-shared-libs.md`. This document is the detailed analysis of the
existing projects in `/home/dan/Downloads/f411re-libs/` (excluding the Rust and
Zig ports) and the phased plan for extracting a reusable **Source Code library
(SCL)** named `cortex-m-scl`.

> **STATUS (2026-07-19): MIGRATION COMPLETE.** All six phases executed; all
> four reference apps consume the SCL (firmware built by CMake only since the
> dedup pass — Makefiles retain just the fixture-library builds); every
> test app lives under `shared-libs-loaders/test/`; validated on the
> WeAct BlackPill STM32F411CE hardware (demo ✔, battery 16/16 ✔, overlay 6/6 ✔, flat-fs
> 16/16 ✔, static 3/3 ✔; host suites 6/6, static-unify suite 458 assertions).
> See `README.md` for the current authoritative documentation; this file
> remains as the original analysis + plan record.
>
> Post-plan dedup (same day): single-copy ownership established — test-app
> mains and the board platform exist once each (`test/platforms/board`
> + reference-app-owned mains, referenced in place everywhere); the duplicate
> Makefile firmware builds were removed after two stale-binary hardware faults.

Projects in scope:

| Project | Loader flavour | Build | Notes |
|---|---|---|---|
| `main` | Dynamic (dlopen/dlsym/dlclose over semihosting) | Make | Reference implementation, C++23 |
| `overlay-flat_fs` | Dynamic, libraries served from an on-chip flat flash archive | Make | `main` + FlatFS storage backend |
| `overlay-overlay` | Static linker overlays **and** the dynamic loader side by side | Make | Adds overlay window machinery |
| `static-unify_code` | "Static" loader: link-time PLT binding + runtime GOT patching (`NO_DYN`) vs runtime lookup (`DYN`) | Make | Older, diverged core; has `patch_journal` |

---

## 1. Headline finding: the codebase is one loader copied four times

`diff -rq` results (verified byte-for-byte):

* **`core/` is byte-identical across `main`, `overlay-flat_fs`, and
  `overlay-overlay`** — all 11 headers and 7 sources. So are `tests/`, `libs/`
  (fixture libraries), and all of `firmware/src/` except the deltas below.
* `static-unify_code/common/core/` is an **older, diverged fork** of the same
  core: only `byte-source.hpp`, `elf_image`, `elf_types`, `error`, `relocator`,
  `symbol_resolver` exist there (no `dynamic_info`, `region_allocator`,
  `reloc_applier`, `lazy_resolve`, `detail/bytes`), with drifted contents. It
  uniquely adds `patch_journal` (revert-on-dlclose) and a `dynamic_linker` class
  wrapper, and it patches **main's own GOT** from library exports
  (`patch_main_got`) — bidirectional linking not present in the newer tree.

Per-project deltas from `main`:

* `overlay-flat_fs` adds: `dl-fs.cpp`, `fs_archive.{hpp,cpp}`,
  `flash-byte-source.hpp`, `flash_prog.{h,c}`, `battery.hpp`,
  `main-flatfs.cpp`; and refactors `dl.cpp` to expose
  `dlopen_source(byte_source&, name)` with `dlopen()`/`dlopen_fs()` as thin
  front-ends (`dlopen_fs` is a weak stub overridden when `dl-fs.cpp` links).
  **This refactor is the seed of the SCL design and should be adopted as the
  canonical `dl.cpp`.**
* `overlay-overlay` adds: `main-overlay.cpp`, `firmware/src/overlays/`
  (`ov1.c`, `ov2.c`, `ovcpp.cpp`), and the overlay linker scripts
  (`mem-overlay.ld`, `sections-overlay.ld`). Everything else is identical.

Conclusion: the SCL core can be extracted **once** from `main`/`overlay-flat_fs`
(they are the freshest), and `static-unify_code`'s unique features
(`patch_journal`, main-GOT back-patching) folded in as optional modules.

---

## 2. Item-by-item analysis (per the numbered list in `scl-shared-libs.md`)

### 0. API/configuration used by applications today

Application-facing surface (from `main.cpp`, `main-flatfs.cpp`,
`main-overlay.cpp`, `dl-impl.cpp`):

* **Load/unload** — `fw::dlopen(path)`, `fw::dlopen_fs(name)` (flat_fs),
  `fw::dlopen_source(byte_source&, rawname)`, `fw::dlsym(handle&, name)`,
  `fw::dlclose(handle&)`. All return `loader::result<T>`
  (`std::expected`-based, `loader::Error` enum).
* **handle** — POD-ish struct exposing `base`, `got_base`, refcount/deps,
  lazy tables, exidx info, GOT snapshot (didactic), provenance name.
* **Calling into a library** — `pic_call_void/u32/u32_2(fn, got_base, ...)`
  assembly trampolines that set `r10` to the library GOT base.
* **Feature toggles** — `dl_enable_cross_call(bool)` (cross-library `r10`
  routing via the `dl_cross_call` trampoline), `dl_cross_trace(int)`.
* **Diagnostics** — `dl_dump_image`, `dl_dump_main_provides`,
  `dl_dump_got_walkthrough`.
* **Compile-time config** — `-DDL_LIB_DIR=` (fixture path), Makefile-selected
  main (`main.cpp` vs `main-tests.cpp` vs `main-flatfs.cpp` vs
  `main-overlay.cpp`); `static-unify_code` selects `DYN`/`NO_DYN` via separate
  Makefiles.
* **static-unify variant** — class-based: `firmware::dynamic_linker(ctx)`,
  `.load(path)` / `.load_from_source(src)` / `.lookup(lib,name)` /
  `.unload(lib)`; uses `std::vector`/`unique_ptr` (heap) unlike `main`'s
  fixed handle table.
* **Link-time contract with the app** — firmware must be built `-fpic` with
  `-Wl,-E` (export dynamics), keep chosen symbols alive with `-u` anchors, and
  provide `_DYNAMIC` (parsed by `link_context::from_self()`); the linker script
  reserves the RAM arena used by `image_memory`.

### 1. Functionality shared by multiple projects (→ SCL common code)

`core/` in its entirety: `elf_types` (ELF32/ARM structs & constants),
`elf_image` (header/phdr/dynamic parsing over a byte_source), `dynamic_info`,
`symbol_resolver` (SysV-hash `.dynsym` lookup), `relocator` +
`reloc_applier` (R_ARM_RELATIVE / GLOB_DAT / ABS32 / JUMP_SLOT / COPY /
REL32), `region_allocator` (RAM arena), `lazy_resolve` (lazy PLT fixup),
`byte_source` + `detail/bytes`, `error`. Plus firmware-level shared code:
`dl.cpp` orchestration, `link_context` (main's `.dynsym` surface),
`image_memory`, the three assembly trampolines, `unwind_find_exidx`
(cross-image EHABI lookup), `libc_shim`.

### 2. Static loader implementation

`static-unify_code`: link-time symbol binding (`NO_DYN` build) with runtime GOT
patching of the firmware image, `patch_journal` recording every patch so
`unload()` can revert, `patch_main_got` for firmware→library imports. Sources:
`common/firmware/{dynamic_linker,patch_journal,link_context,image_memory}.cpp`
+ its older `common/core`.

### 3. Dynamic loader implementation

`main` (freshest): `dl.cpp` pipeline — open byte_source → `elf_image` parse →
`region_allocator` arena alloc → segment copy → `relocator` plan →
`reloc_applier` (eager, lazy, or cross-call routing) → ctors (`.init_array`)
→ handle bookkeeping (refcounts, dep edges, exidx registration).

### 4. Overlay loader implementation

`overlay-overlay`: **not runtime code in `core/`** — it is linker-script
machinery (`OVERLAY` regions aliasing one SRAM window at `0x20018000`,
`NOCROSSREFS`) plus a tiny runtime: memcpy LMA→VMA using linker-provided
`__load_start_/__load_stop_` symbols, then `DSB; ISB`. Today this lives inline
in `main-overlay.cpp`/`ov*.c`. For SCL, extract an `overlay` module: an
overlay descriptor table (region VMA, LMA, size), `ov_load(id)`,
`ov_current()`, and a documented linker-script fragment the app must include.

### 5. Symbol resolution

Three mechanisms, all shared: SysV `.hash` lookup (`symbol_resolver`),
`link_context::from_self()` exposing the firmware's own dynamic symbols as a
`symbol_source`, and multi-image search order in `dl.cpp` (main → loaded libs)
with cross-library dependency tracking. `static-unify` adds reverse direction
(library exports resolved into main's GOT).

### 6. Relocation handling

`relocator.cpp` (plan building from `.rel.dyn`/`.rel.plt`) +
`reloc-applier.cpp` (application, incl. veneers/trampoline routing);
lazy binding in `core/lazy-resolve.cpp` + firmware stubs
`lazy-resolve.c`/`lazy-plt.hpp`; cross-library `dl-cross-call.c` (asm,
swaps `r10` per callee).

### 7. Export/import management

Exports: firmware `-Wl,-E` + `-u` anchors + `fm-iface.h`; library `.dynsym`.
Imports: UND symbols in `.dynsym` resolved via `symbol_source` chain. The SCL
should formalize `symbol_source` (already in `reloc-applier.hpp`) as *the*
extension point so apps can add custom export tables without linker tricks.

### 8. ELF parsing

`elf-image.cpp` + `elf-types.hpp` + `dynamic-info.cpp` — clean, byte_source-based,
already platform-independent. Take verbatim from `main`.

### 9./10. Filesystem & storage abstraction points

`loader::byte_source` is the single storage abstraction and is already
sufficient. Existing backends: `semihost_file` (semihosting),
`Flashbyte_source` (memory-mapped flash blob), `Memorybyte_source` (host tests).
`fs_archive` (FlatFS) + `flash_prog` are **application-side** per the design
requirements — they move to an example/app support area, not the SCL core.
`dlopen(path)`/`dlopen_fs(name)` convenience front-ends are also app-side;
the SCL exposes only `dlopen_source(byte_source&, name)`.

### 11./12. Platform- and board-dependent code

* Cortex-M-specific but board-independent (stays in SCL, isolated in a
  `port/` subfolder): `pic_trampoline` (asm), `dl-cross-call.c` (asm),
  `lazy-resolve.c` firmware stub, `unwind-find-exidx.c`, `DSB/ISB` barrier
  for overlay loads.
* Board/environment-specific (excluded from SCL, app-provided): `board.c`,
  `startup.c`, `semihost.{h,c}` + `semihost_file`, `flash_prog`,
  all linker scripts (SCL ships documented *fragments/templates* only),
  `static-unify`'s CMSIS/HAL tree, `led`/`timer`.
* Logging: `dl.cpp` currently calls `trace_puts` directly — must be routed
  through a config hook (`SCL_LOG(...)`, weak function, or callback).

### 13./14. Public vs internal APIs

Public: `dl.hpp` (or a new `scl/dynamic.h[pp]`), `byte-source.hpp`,
`error.hpp`, `pic-trampoline.h`, overlay API, static-loader API
(`dynamic_linker`/`patch_journal`). Internal: `elf_image`, `dynamic_info`,
`relocator`, `reloc_applier`, `region_allocator`, `symbol_resolver`,
`lazy_resolve`, `detail/bytes`, `link_context`, `image_memory` — keep out of
`include/`, or under `include/.../detail/` where tests need them.

### 15./16. Header/source reorganization (target layout)

Common code lives **directly** in `include/` and `src/`; only loader-specific
code goes into the `static/`, `dynamic/`, `overlay/` subfolders:

```
shared-libs-loaders/
├── include/
│   ├── loader/            # common: byte_source, Error/result, elf_types,
│   │                      # symbol_source, relocator/resolver/allocator headers
│   ├── scl/               # common: log hook, pic-trampoline.h, link_context,
│   │                      # image_memory, unwind_exidx
│   ├── static/            # dynamic_linker, patch_journal public API
│   ├── dynamic/           # dl API: handle, dlopen_source/dlsym/dlclose,
│   │                      # cross-call & trace toggles, lazy binding config
│   └── overlay/           # overlay descriptor table + ov_load API
├── src/                   # common: elf_image, dynamic_info, symbol_resolver,
│   │                      # relocator, reloc_applier, region_allocator,
│   │                      # link_context, image_memory, log.c
│   ├── port/              # asm: pic_trampoline, dl_cross_call, lazy stub,
│   │                      # unwind_find_exidx (Cortex-M only)
│   ├── static/            # dynamic-linker.cpp, patch-journal.cpp (modernized)
│   ├── dynamic/           # dl.cpp (dlopen_source pipeline)
│   └── overlay/           # overlay.c + ldscripts/overlay-fragment.ld (template)
├── test/                  # ALL tests: test-{static,dynamic,overlay,misc}/testN
│                          # (firmware apps; reference-app code referenced in
│                          # place), host-unit/ (doctest), platforms/{freestanding,
│                          # hosted}, fixtures/ + fixtures-src/, tools/, build-*/
├── CMakeLists.txt
└── README.md
```

CMake targets: `scl-common` (everything outside the three loader subfolders;
static/dynamic/overlay each depend on it), `scl-dynamic`, later `scl-dynamic-newlib`
and `scl-overlay`.

### 17. CMake target organization

*(Outcome note: the final implementation went further than this proposal —
all four targets ended up as INTERFACE **source** libraries per the xpack
diag-trace pattern: no `.a`/OBJECT artifacts, consumers compile the SCL
sources with their own flags. See README §2.)*

* `scl-common` (STATIC/OBJECT lib): core + port asm. Options:
  `SCL_ENABLE_CXX_EH` (exidx support), `SCL_LOG_LEVEL`, `SCL_MAX_IMAGES`,
  `SCL_GOT_SNAP_MAX` (0 disables didactic snapshots), `SCL_ARENA_*` or
  app-provided allocator hooks.
* `scl-dynamic` → links `scl-common`. Options: `SCL_DYN_LAZY_BINDING`,
  `SCL_DYN_CROSS_CALL`, `SCL_DYN_DIAGNOSTICS` (dump_* functions).
* `scl-dynamic-newlib` → links `scl-common`. Options: `SCL_STATIC_PATCH_JOURNAL`,
  `SCL_STATIC_PATCH_MAIN_GOT`. Must lose its `std::vector`/`unique_ptr`
  heap dependency (align with `main`'s fixed table) or gate it behind
  `SCL_STATIC_USE_HEAP`.
* `scl-overlay` → tiny; links nothing but the barrier helper.
* Host testability: cores compile for host (they already do — `tests/` run on
  x86 via `Memorybyte_source`); guard the few `arm` asm files with
  `CMAKE_SYSTEM_PROCESSOR` checks.
* Apps do `add_subdirectory(cortex-m-scl)` + `target_link_libraries(app
  scl-dynamic)` and provide board/startup/ldscripts themselves.

### 18. Existing duplication (to eliminate)

* 3× byte-identical `core/`, `tests/`, `libs/`, and ~90 % of `firmware/src/`
  across `main`/`overlay-flat_fs`/`overlay-overlay` (≈ two full copies of
  ~2.5 kLOC of loader + ~20 test files each).
* A 4th diverged copy of ~½ the core in `static-unify_code`.
* `dl.cpp` in `main` vs `overlay-*` differs only by the `dlopen_source`
  refactor — trivially unifiable (keep the flat_fs version).
* Fixture libraries (`mylib1/2`, `c1/c2`, `cpp3`, `libcppsh`, `libcppeh`,
  `libprov`, `libcons`) duplicated 3×; move once into `tests/fixtures/`.

### 19. Simplification opportunities

* One `dl.cpp` with `dlopen_source()` as the only entry; path/name front-ends
  become app code (kills the weak-symbol `dlopen_fs` trick).
* Replace direct `trace_puts` calls with a single log hook.
* Merge `static-unify`'s `dynamic_linker` orchestration onto the newer core
  instead of maintaining its fork (its unique value is patch_journal +
  main-GOT patching, both expressible over the new `reloc_applier`).
* Make `handle` capacity/table sizes compile-time options instead of magic
  constants (`kGotSnapMax`, `deps[4]`, `name[12]`).
* Overlay support reduces to ~100 lines + a documented ld fragment.

---

## 3. Phased migration strategy (item 20)

Each phase keeps every existing test application green (`make test` host
batteries + on-target scripts) before the next begins.

**Phase 0 — Baseline.** ✔ DONE — Record current test outcomes for all four projects
(host doctest batteries; target scripts `flash-run.sh`/`test_*.sh` where
hardware is available). These are the acceptance gates for every later phase.

**Phase 1 — Extract `scl-common` + `scl-dynamic`.** ✔ DONE — Create
`cortex-m-scl` with CMake; move `main`'s `core/` verbatim into `src/shared`,
and the *flat_fs variant* of `dl.cpp`/`dl.hpp` (the `dlopen_source` refactor)
plus `link_context`, `image_memory`, lazy/cross-call/trampoline files into
`src/dynamic` + `src/shared/port`. Introduce the log hook. Move the host test
suite (one copy) into `tests/dynamic` and make it build against the SCL on
host — this is the fastest correctness check, no hardware needed.

**Phase 2 — Port `main` to consume the SCL.** ✔ DONE — `main` keeps only `board`,
`startup`, `semihost*`, ldscripts, `main*.cpp`, and a local
`dlopen(path)` wrapper over `semihost_file` + `dlopen_source`. Its Makefile (or
a new CMake build) pulls SCL sources. Target tests must match Phase 0 output.

**Phase 3 — Port `overlay-flat_fs`.** ✔ DONE — Same as Phase 2; `fs_archive`,
`flash_prog`, `flash_byte_source`, `dlopen_fs` stay in the app as the
reference "custom storage backend" example.

**Phase 4 — Extract `scl-overlay` and port `overlay-overlay`.** ✔ DONE — Pull the
overlay copy/barrier logic out of `main-overlay.cpp` into a descriptor-driven
`ov_load()`; ship `sections-overlay.ld` as a documented template. The app
keeps its scenario/demo code.

**Phase 5 — Rebase `static-unify_code` onto the SCL core.** ✔ DONE — Largest step:
replace its diverged `common/core` with `scl-common` (API drift to reconcile in
`elf_image`/`relocator`/`symbol_resolver`), re-implement `patch_journal` and
`patch_main_got` over the new `reloc_applier` as the `scl-dynamic-newlib` module,
remove or gate its heap usage. Validate with its own `test/` battery and
`DYN`/`NO_DYN` target scripts.

**Phase 6 — Cleanup & de-duplication.** ✔ DONE — Delete the now-unused per-project
`core/`, `tests/`, duplicated fixture libs (single `tests/fixtures/` in SCL);
finalize CMake options, README, and per-module API docs. Optionally follow up
with a CMake presets/toolchain file for the STM32F411RE reference apps.

Risk notes: (a) `static-unify` reconciliation is the only phase with real code
changes rather than moves — schedule it last; (b) the exported-symbol link
contract (`-Wl,-E`, `-u` anchors, `_DYNAMIC`) must be documented early since it
is invisible in the sources; (c) keep the asm files (`pic-trampoline.c`,
`dl-cross-call.c`, `lazy-resolve.c`) byte-stable through Phase 5 — they are the
hardest pieces to re-verify.

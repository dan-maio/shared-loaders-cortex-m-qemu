# Refactor plan — folder renames + import the static patcher

> **STATUS (done on branch `shared-lib-loaders`):** Stage 1 (module renames),
> Stage 2 (newlib apps/trees/scripts), Part 2 (import `scl::static` +
> `test-static/test-static`), and Stage 3 (docs + PDFs) are complete and committed.
> This document is retained as the record of decisions.


Planning doc for two changes to **cortexm-shared-loader**. Nothing here is done
yet; this is the analysis to execute against. Decisions 1–4 must be locked first.

## Goal

1. Rename `src/dynamic → src/dynamic-nolib` and `src/static → src/dynamic-newlib`
   (+ update all files, build/execute scripts, and docs — md and pdf).
2. Copy **shared-static-rework** into a **new** `cortexm-shared-loader/src/static`,
   restructured into a self-contained module with its own `test/` (like dynamic-*).

## Current state (what "dynamic" / "static" mean today)

| Today | Code | Headers | Target | Tests | Role |
|---|---|---|---|---|---|
| `src/dynamic/dl.cpp` | `include/dynamic/` | `scl-dynamic` / `scl::dynamic` | `test/test-dynamic` | freestanding `dlopen` loader (nolib) |
| `src/static/*.cpp` (dynamic-linker, patch-journal, image-memory, link-context) | `include/static/` | `scl-static` / `scl::static` | `test-static/test-static` | hosted link-time loader (newlib) |

Proposed: `src/dynamic → src/dynamic-nolib`, `src/static → src/dynamic-newlib`,
then a **new** `src/static` = the shared-static-rework static patcher.

## ⚠️ Core hazard: "dynamic"/"static" are massively overloaded

Naive find/replace blast radius:
- **CMake:** ~74 refs to `scl-dynamic`/`scl-static`/`scl::*`; only **8** are real path refs.
- **Docs (.md):** 185 (`dynamic-vs-static-linker-explained.md`) + 104 (`README.md`)
  + 41 + 39 + 28 + 25 … across analysis docs, `command_test.md`, `build-linking-guide.md`.
- **Scripts:** `flash-run-sf/hf.sh`, `SCL_STATIC_VARIANT`, app names `scl-fw-static1/2`,
  build trees `build-static`/`build-static-hf`, `libs/build` — all currently mean
  "the hosted static loader" (→ becoming `dynamic-newlib`).

**Must NOT be touched** (false positives):
- `src/dynamic-info.cpp` + `include/loader/dynamic-info.hpp` — these are **scl-common**, unrelated to the dynamic loader.
- `scl::overlay`, `scl-fw-dyn1/2`, `test-overlay`.

A blind replace **breaks the build and mislabels overlay/common code**. Scope
every rename to whole-path / whole-target-name matches.

## Decision 1 — how far do the renames propagate?

- **A. Minimal** — rename only `src/dynamic`, `src/static`; fix the 8 CMake path
  lines + any `#include "static/…"`. Targets stay `scl-dynamic`/`scl-static`.
  Lowest risk, but folder names no longer match target names.
- **B. Code-consistent (RECOMMENDED)** — also rename `include/dynamic→include/dynamic-nolib`,
  `include/static→include/dynamic-newlib`, and targets `scl-dynamic→scl-dynamic-nolib`,
  `scl-static→scl-dynamic-newlib` (+ `scl::` aliases). Update CMake, includes, docs.
  Leave the test-dir/app/script *names* (`test-static`, `scl-fw-static1`,
  `SCL_STATIC_VARIANT`, `build-static`) as-is — user-facing, high-churn.
- **C. Full** — rename everything incl. test dirs, app targets, build trees, script
  vars. 400+ edits, and `scl-fw-static*` would clash with the new static patcher's apps.

Recommendation: **B**, and explicitly **do not** rename the `scl-fw-static*` /
`build-static` test surface (it collides with part 2).

> **Decision 1 answer: C — FULL code rename.** No clash: the hosted-static apps
> are *derived from* `dynamic-newlib`, so they are renamed too. The name
> `static` / `scl-fw-static*` is then free for the NEW static patcher (part 2).
>
> Rename map (everything that today means the hosted link-time loader → `dynamic-newlib`;
> everything that means the freestanding dlopen loader → `dynamic-nolib`):
>
> | Today (hosted static loader) | New |
> |---|---|
> | `src/static/` | `src/dynamic-newlib/` |
> | `include/static/` | `include/dynamic-newlib/` |
> | `scl-static` / `scl::static` | `scl-dynamic-newlib` / `scl::dynamic-newlib` |
> | `test-static/test-static/` | `test-dynamic/test-dynamic-newlib/` |
> | `test/build-static` / `build-static-hf` | `test/build-dynamic-newlib` / `-hf` |
> | `scl-fw-static1` / `static2` | `scl-fw-dynamic-newlib1` / `dynamic-newlib2` |
> | `SCL_STATIC_VARIANT` | `SCL_DYNAMIC_NEWLIB_VARIANT` |
> | `libs/build` / `libs/build_hf` (fixture trees for the hosted apps) | keep? (decide) |
> | `flash-run-sf.sh` / `flash-run-hf.sh` (comments/paths) | update prose + tree paths |
>
> | Today (freestanding dlopen loader) | New |
> |---|---|
> | `src/dynamic/` | `src/dynamic-nolib/` |
> | `include/dynamic/` | `include/dynamic-nolib/` |
> | `scl-dynamic` / `scl::dynamic` | `scl-dynamic-nolib` / `scl::dynamic-nolib` |
> | `test/test-dynamic/` | `test-dynamic/test-dynamic-nolib/` (`scl-fw-dyn1/2/flatfs`, `test-overlay` — decide if these follow) |
>
> STILL untouched (false positives): `src/dynamic-info.cpp`,
> `include/loader/dynamic-info.hpp` (scl-common), `scl::overlay`.

## Part 2 — new `src/static` = shared-static-rework, restructured

Self-contained module (loader code split from the demo app, mirroring dynamic-*):

**Layout = FLAT, like the siblings** (overlay / dynamic-*), NOT self-contained
nested. Code, headers and the app live in the repo's top-level `src/`,
`include/`, `test/` — exactly how `dynamic-nolib` / `dynamic-newlib` / `overlay`
are organized. `test-static/test-static` is freed by the D1 rename (old static loader's
tests → `test-dynamic/test-dynamic-newlib`).

```
src/static/                 ← INTERFACE target scl-static / scl::static (Decision 4)
    elf-reader.cpp  shlib-loader.cpp  logger.cpp     (resolving/patching + loader support)
include/static/
    elf-reader.h    shlib-loader.h    logger.h
test-static/test-static/           ← the ONE firmware app (Decision 2)
    main.cpp
    led.* timer.* initialize-hardware.c write.c stm32f4xx-hal-msp.c   (app support)
    system/         (startup, HAL, CMSIS, newlib)
    ldscripts/      (mem-sh.ld, libs-sh.ld, sections-sh.ld)
    libsh1/ libsh2/ (demo .so + their ldscripts; "shlib1/2")
    CMakeLists.txt  build-and-run*.sh
    (build/ out-of-source, gitignored — do NOT copy stale artifacts)

Builds with common-elf-reader (Decision 3), INTERFACE style (Decision 4):
  scl-static (INTERFACE: src/static + include/static) → links common-elf-reader::elf-reader
  test-static/test-static app → links scl::static + a platform library (no .a; sources compile in)
```

### Decision 2 — resolving/patching (→ src+include) vs app (→ test)

- **Loader (→ `src/static/src` + `include`):** `elf-reader.cpp/.h`, `shlib-loader.cpp/.h`.
- **App (→ `src/static/test`):** `main.cpp`, `led.*`, `timer.*`, `initialize-hardware.c`,
  `write.c`, `stm32f4xx-hal-msp.c`, `system/`, `ldscripts/`, `libsh1/`, `libsh2/`,
  build files, run scripts.
- **`logger.cpp/.h`:** goes in `src/static` + `include/static` as loader support
  (option a). No switch to `scl/log.h`.

> **Decision 2 answer:** loader side = `src/static` (`elf-reader.cpp`,
> `shlib-loader.cpp`, `logger.cpp`) + `include/static` (their `.h`).
> `test-static/test-static/` holds **one** firmware app only: `main.cpp` + the two demo
> shared libs (`libsh1/`, `libsh2/` — user's "shlib1/2"; rename to `shlib1/2`?
> decide), plus `system/` + `ldscripts/` and the `led/timer/
> initialize-hardware/write/stm32f4xx-hal-msp` app support.
> **Layout is FLAT (like siblings), not nested** — see Part 2 below.

### Decision 3 — integrate with common-elf-reader?

shared-static-rework's `elf-reader.cpp` is an adapter over `loader::elf_image`.
On the move, keep that dependency → `src/static` also links
`common-elf-reader::elf-reader` (RECOMMENDED, consistent with the rest of the SCL),
vs. dropping it and vendoring a standalone parser (reverses the shared-parser work —
not recommended).

> **Decision 3 answer:** KEEP the dependency — `src/static` builds with
> `common-elf-reader` (its `elf-reader.cpp` stays an adapter over
> `loader::elf_image`).

### Decision 4 — new target + join the CMake build now?

Does `src/static` get its own SCL target (e.g. `scl-static` / `scl::static`, freed
by renaming the old one to `dynamic-newlib`) and a `test-static/test-static-patcher`
wiring, or stay a self-contained copy with its own Makefile/CMake not yet in the
top-level SCL build? The nested `src/static/test/build/` suggests self-contained
for now.

> **Decision 4 answer:** Wire it into the SCL with the **same INTERFACE
> source-code-library CMake style** as `scl-common` / `scl-dynamic-*` /
> `common-elf-reader::elf-reader`. So `src/static/{src,include}` become an
> INTERFACE target (`scl-static` / `scl::static` — the name freed by Decision 1),
> which links `common-elf-reader::elf-reader`; the `test/` app links that target
> plus a platform library. No `.a` — sources compile into each consumer.

## Risks / gotchas

1. **Overloaded grep** — scope every replace to whole paths/targets; protect
   `dynamic-info`, `overlay`, `scl-fw-dyn`.
2. **Name collision** — RESOLVED by Decision 1=C: old hosted-static apps become
   `scl-fw-dynamic-newlib*`, freeing `static`/`scl-fw-static*` for the new patcher.
   Open sub-questions this raises: (a) do the *freestanding* app names
   `scl-fw-dyn1/2/flatfs` and `test-overlay` also get renamed, or only the
   loader dirs/targets on that side? (b) do the `libs/build` / `libs/build_hf`
   fixture trees get renamed too? Both are high-churn in scripts + docs.
3. **`build/` in the copy** — don't copy stale artifacts; `.gitignore` covers
   `src/static/test/build/`.
4. **PDFs** — 4 exist (`dynamic-vs-static-linker-explained`, `README`,
   `scl-codebase-analysis`, `scl-guide`); regenerate after md edits.
5. **`dynamic-vs-static-linker-explained.md`** (185 refs) — its whole thesis is
   "dynamic vs static"; renaming to `dynamic-nolib`/`dynamic-newlib` changes its
   vocabulary — likely prose rework, not pure substitution.
6. **Cross-repo** — shared-static-rework stays put (a *copy* goes into the SCL);
   its own remote `shared-static-rework-cortexm.git` keeps living.

## Suggested execution order

1. Lock Decisions 1–4.
2. Part 1 renames: `git mv` folders → fix CMake paths → fix `#include`s → build +
   ctest green **before** touching docs.
3. Rename targets/includes per Decision 1; rebuild.
4. Copy shared-static-rework → `src/static/`; restructure into src/include/test;
   strip `build/`; fix internal include paths + `COMMON_ELF_READER_DIR` relative path.
5. Build the new module standalone; wire into SCL if Decision 4 says so.
6. Update docs (README tree, build-linking-guide, command_test, dynamic-vs-static
   doc) → regenerate all 4 PDFs.
7. Commit in logical chunks; push.

## Reference — the 8 CMake path lines to update (Part 1)

```
CMakeLists.txt:82   src/dynamic/dl.cpp
CMakeLists.txt:90   include/dynamic
CMakeLists.txt:98   src/static/dynamic-linker.cpp
CMakeLists.txt:99   src/static/patch-journal.cpp
CMakeLists.txt:100  src/static/image-memory.cpp
CMakeLists.txt:101  src/static/link-context.cpp
CMakeLists.txt:103  include/static
```
(plus `scl-dynamic`/`scl-static` target/alias lines if Decision 1 = B or C.)

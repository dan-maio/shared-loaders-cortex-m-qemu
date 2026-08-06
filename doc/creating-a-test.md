# Creating a New Test — a methodical walkthrough

A test in this workspace is Cortex-M4 firmware that loads one or more modules and
proves something about how they were loaded. Building one means making six
decisions, **in order** — each constrains the ones after it. Take them out of
order and you will find yourself rebuilding fixtures because the loader you
picked cannot consume them.

| # | Decision | Constrains |
|---|---|---|
| 1 | Board startup — freestanding or hosted | what libc the app and modules may use |
| 2 | Delivery — how modules reach the board | what the app does before loading |
| 3 | Loader — how symbols get bound | the fixture format, entirely |
| 4 | Fixture construction | follows from 3 and 5; no freedom left |
| 5 | Execution — XIP or RAM | the fixture's link script and PIC model |
| 6 | Call style — direct or resolved | what main's source looks like |

Read `doc/test-taxonomy.md` first for where the directory goes and what to call
it. This document is about what goes *in* it.

---

## Decision 1 — Board startup

Two platform families under `test/platforms/`, selected by `SCL_PLATFORM`
(default `board`):

| Platform | Model | Link flags | What you get |
|---|---|---|---|
| `board` | **freestanding / nolib** | `-pie -nostdlib -nostartfiles` | own `startup.c`, `semihost.c`, `libc-shim.c`, `printf-shim.c` |
| `board-hosted` | **hosted / newlib** | `-nostartfiles --specs=nano.specs` | vendored µOS++ system tree: newlib `syscalls.c`, `sbrk.c`, `cxx.cpp`, full HAL |

`board` and `board-hosted` are the same two for the other board.
`test/platforms/common/` holds the board-independent glue (`crt.cpp`,
`semihost-file.cpp`, `dl-semihost.cpp`, `scl-log-semihost.cpp`) that both share.

**Choose freestanding** unless you need a heap. There is no `malloc`; loaded
images come from `fw::image_arena()`, a fixed `region_allocator`. `libc-shim.c`
supplies only what the compiler itself emits — `memcpy`, `memset` and friends.

**Choose hosted** when the app or its modules need newlib: real `malloc`/`free`,
`printf` with float, C++ exceptions and RTTI. `scl::dynamic-newlib` requires it;
so does `test-xip-static/spi-flash`, whose modules borrow `malloc`, `free` and
`snprintf`.

This choice is nearly irreversible later — it decides which symbols main can
export, which decides what your modules may import.

---

## Decision 2 — Delivery

How the module bytes get onto the board, before any loading happens.

### Semihosting

The debugger hands the file over at run time. Simplest, and the default.

```cpp
firmware::semihost_file src(path);          // path from DL_LIB_DIR
auto sz = src.size();
src.read_at(0, std::span<std::byte>{buf, sz});
```

The `.so` paths are passed on the OpenOCD command line by the runner
(`arm semihosting_cmdline`), and `DL_LIB_DIR` is compiled in as the directory to
look in. Nothing persists across a reset.

### Internal flash flat-fs

Modules are written into a small filesystem in internal flash at
`_fs_base = ORIGIN(FLASH_FS)` = `0x08040000`, then read back memory-mapped.
This is what XIP requires — code cannot execute in place from a RAM buffer.

```cpp
fw::fs_add(name, buf, sz);                  // provision (writes flash)
const FsEntry* e = fw::fs_find(name);
const std::byte* blob = fw::fs_blob(*e);    // memory-mapped, executable
```

Provisioning is still done over semihosting once at startup; the flat-fs is
where the bytes live afterwards.

### SPI flash

An external W25Q64 as a staging store. Only `test-xip-static/spi-flash` does
this, and it bootstraps: `spi.so` is loaded first, then *used* to reach the SPI
flash holding the other modules, which are copied back into the internal flat-fs
before executing. Storage is never where code runs.

---

## Decision 3 — Loader

| Loader | Consumes | Binding | Notes |
|---|---|---|---|
| `scl::static` | `ET_DYN` | resolve once at load, patch the GOT | no runtime service |
| `scl::dynamic-nolib` | `ET_DYN` | `dlopen`/`dlsym`, optional lazy PLT | freestanding, arena slots |
| `scl::dynamic-newlib` | `ET_DYN` | full relocation + patch journal | hosted; `dlclose` reverts byte-exact |
| `scl::reloc` | `ET_REL` | `R_ARM_ABS32` literals | no GOT, no PLT, no `r10` |
| `scl::overlay` | link-time bound | `memcpy` LMA→VMA | one module resident per window |

Two behavioural differences that catch people out:

**Destructors.** `scl::dynamic-nolib` records `.fini_array` and runs it on
`dlclose` (`src/dynamic-nolib/dl.cpp:387,404`). `xip::stat` runs `.init_array`
only — it has no `fini` path. If your test asserts on destructor side effects,
the static XIP loader will not satisfy it.

**Unload semantics.** `scl::dynamic-newlib` keeps a `patch_journal` and reverts
every patch on unload, so reloading proves the revert was exact. The others just
release the memory.

---

## Decision 5 (take it before 4) — XIP or RAM

This decides how the fixture must be linked, so settle it before building
anything.

**RAM** — the whole image is copied into RAM and relocated there. Standard
`-fPIC`, PC-relative GOT. Works with every loader.

**XIP** — the R-X segment executes in place from flash; only the RW segment
(GOT, `.data`, `.bss`) is copied to RAM. Requires two things of the fixture:

- **`r10`-based GOT addressing**, not PC-relative. Text in flash and GOT in RAM
  are ~`0x18000000` apart, so any PC-relative reach from one to the other is
  broken by construction.
- **Split segments** with `VA == file_offset`, from `xip-split.ld`.

`test-xip/fixtures-src/build-fixtures.sh` states it plainly:

> `-fPIC` C/C++ libraries use PC-relative GOT and cannot XIP; they load
> copy-whole from `test/fixtures` via the stock loader.

The XIP loader checks this itself: `classify_segments()` reports `split=false`
for a fused RWX blob, and `text_is_immutable()` rejects any relocation targeting
the R-X range. Both are in `src/xip/`.

**The PLT trap.** A PLT stub reaches its GOT slot with `add ip, pc`, so from
flash it reads the *flash* image of the GOT and never the RAM copy. No load-time
fixup can repair it and no compiler flag suppresses PLT generation on ARM. Such
imports must be bound before the image is written to flash —
`xip::stat::bind_host_imports()`, called at provisioning. Full derivation in
`doc/xip-plt-prelink.md`.

---

## Decision 4 — Fixture construction

Now mechanical. Pick the row matching decisions 3 and 5.

### `ET_DYN`, RAM

Committed prebuilt in `test/fixtures/*.so` (12 of them, force-added past the
`*.so` line in `.gitignore`), sources in `test/fixtures-src/`. Standard
`-fpic -shared`. Reuse these unless you need something new.

### `ET_DYN`, XIP

```sh
MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
OPT="-O0 -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,-E -Wl,-Bsymbolic"
$CC $MCU $PIC $OPT -shared -nostartfiles $LDFLAGS -T xip-split.ld -o out.so in.c
```

`-Bsymbolic` binds intra-library calls directly so only genuine external calls
route through the GOT. Built by `test-xip/fixtures-src/build-xip-libs.sh` and
`build-fixtures.sh` into `test-xip/fixtures/`.

### `ET_REL`

```sh
RELOC := -mlong-calls -fno-pic          # every call: ldr rN,=target ; blx rN
# link with: ld -r                      # partial link, stays ET_REL
```

`-mlong-calls` is what removes the ±16 MB `BL` range problem; `-fno-pic` means
fixed absolute addresses and no GOT at all. See
`test-relocatable/fixtures-reloc/Makefile`.

### Soft-float caveat

`test-xip-static/spi-flash` is the one test whose toolchain file is
`-mfloat-abi=soft` with no `-mfpu`. Anything it compiles must avoid VFP
instructions, or be guarded — see `#if defined(__ARM_FP)` in
`src/xip/xip-cross-call.c`.

---

## Decision 6 — Call style, constructors, variables

### Resolved calls (the default)

Look the symbol up at run time and call through the pointer. Entering a module
requires `r10` = its GOT base, which the `pic_call_*` trampolines set:

```cpp
void* fn = xip::stat::resolve(m, "library_function");
uint32_t r = pic_call_u32(fn, (void*)m.got_base, 7);
```

`scl::Module` plus the `call_sym0/1/2` helpers in each test's
`test-helpers.hpp` wrap this. **Using them does not make the test "direct"** —
you are still naming the function in a string.

### Direct calls

The app calls by name, as ordinary symbols:

```cpp
extern struct SInfo s_info;                 // module global
z = library_function(7);                    // module function
```

Two ways to make that link, and the taxonomy does not distinguish them:

- **link time** — put the `.so` on main's link line (`-l:slib.so …`). Used by
  `test-xip-static-direct`.
- **load time** — the loader patches main's own GOT: `shlib_loader::patch_main()`
  or `xip::stat::load(..., patch_host_got=true)`. Used by `test-static-direct`
  and `test-drv`.

### Constructors

The loader runs `.init_array` at the end of `load()`, entering each with
`r10 = got_base`. They run **during load**, before your first call — so a fault
in a global constructor looks like a load failure, not a call failure. That is
exactly how the `cpp_features` PLT bug presented.

### Variables

A module's globals live in the RAM copy of the RW segment. `resolve()` returns
the RAM address for data and the flash address for code, using the two-base
model — `sym_addr()` in `src/xip/adapter-base.hpp` picks the base by which
segment the symbol's `st_value` falls in.

### What main must export

Modules import from main, resolved against main's own `.dynsym` via
`link_context::from_self()`. But `-Wl,-E` only exports symbols that were
*actually linked in*, and libc functions come from archives — pulled only when
referenced. So a module importing `printf` fails unless main references `printf`
itself.

Include `test-xip/common/include/link-anchor.hpp` from exactly one translation
unit. It anchors the usual borrowed set (`printf`, `puts`, `operator new`, the EH
runtime, libm). If your modules borrow something else, add an array beside it.
Getting this wrong surfaces as a HardFault inside the module, not a link error.

---

## Wiring it up

1. `CMakeLists.txt` — copy the closest sibling. Reach shared infrastructure
   through `test/`, never by walking up into another tree:
   ```cmake
   get_filename_component(_repo ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
   set(_su ${_repo}/test/platforms/board-hosted/vendor)
   include(${_repo}/test/cmake/scl-test-app.cmake)
   ```
   Pass linker scripts as `-T${DIR}/script.ld` — one argument, no space. The
   `"SHELL:-T ${DIR}/script.ld"` form word-splits and breaks on paths containing
   spaces.
2. `add_subdirectory` in the tree's `CMakeLists.txt`, unless it is a standalone
   hosted app.
3. A row in the tree's `build-and-run.sh` `TARGETS` table:
   `[option]="cmake-target:subdirectory"`.
4. A line in `cleaner.sh` so its `build/` is cleaned.

---

## Verifying

In this order — each step catches a class the previous one cannot.

```sh
# 1. builds standalone (the aggregate supplies includes that this does not)
cd <new test> && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=... && cmake --build build -j

# 2. builds from its tree
cmake -S <tree> -B <tree>/build -DCMAKE_TOOLCHAIN_FILE=... && cmake --build <tree>/build -j

# 3. the runner works up to the flash step
OPENOCD=/bin/false <tree>/build-and-run.sh <target>

# 4. the fixture scripts land in the right directories
<its fixtures-src>/build*.sh && ls <expected output dir>

# 5. on the board
<tree>/build-and-run.sh <target>
```

Steps 1, 3 and 4 exist because each has hidden a real bug. A clean cmake build
proves nothing about relative paths inside runner scripts, and nothing at all
about where a fixture script writes its output.

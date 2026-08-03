# SCL vs Test App — Responsibility Split After Loading & Parsing

The previous document (`TEST-MODULE-LOADING.md`) covered the **reading** side:
how the test app's `byte_source` subclasses deliver bytes. This document covers
what happens **after** those bytes are loaded and parsed: for every loading
case, **who is responsible for what** — the **SCL library** (the loader engine
under `src/`, `include/`, and the `common-elf-reader` parser) versus the
**test application** (the `test-*` trees, the platform layer, and each `main`).

The rule that holds everywhere:

- **SCL** owns the *loader engine*: parsing, image placement, relocation,
  constructor execution, symbol lookup, handle lifetime — it is **storage- and
  board-agnostic**.
- **The test app** owns everything *around* the engine: where the bytes come
  from (the reader), how they were provisioned, the board, the console, the
  fixtures, and the test logic that calls the engine and reports results.

File references are given as `path:line`. All line numbers are for this
workspace.

---

## 1. Case: `scl::dynamic-nolib` (freestanding `dlopen`)

**Apps:** `scl-fw-dyn1`, `scl-fw-dyn2`, `scl-fw-gotplt-demo`, `scl-fw-flatfs`,
`scl-fw-flatfs-spi`, `scl-fw-flatfs-bitbang-spi`, `scl-fw-ovl1`.

### What the SCL does (the whole engine)

| Step | Where | What it does |
|---|---|---|
| Parse the ELF | `common-elf-reader/src/elf-image.cpp:26` `elf_image::parse(byte_source&)` | reads Ehdr/Phdr/Shdr via the byte_source; checks magic/class/arch |
| Reserve image memory | `src/image-memory.cpp:12` `image_arena()`; `src/region-allocator.cpp:22` `region_allocator::alloc()` | hands out a slot in the linker-reserved arena |
| Copy segments + relocate + resolve + constructors | `src/dynamic-nolib/dl.cpp:190` `fw::dlopen_source()` | copies `PT_LOAD` into the slot, patches GOT/PLT, resolves against main's `.dynsym`, runs `.init_array`, snapshots `.got`, records exidx, refcounts deps |
| Lazy / cross-lib / EH support | `src/port/lazy-resolve.c`, `src/port/dl-cross-call.c`, `src/port/pic-trampoline.c`, `src/port/unwind-find-exidx.c` | r10 trampolines, lazy PLT fixup, C++ EH unwinding |
| Handle lifetime | `src/dynamic-nolib/dl.cpp:392` `dlsym`, `:398` `dlclose` | resolves a symbol, runs fini, frees the slot, decrements refcounts (refuses if still in use) |
| C-API facade | `src/dynamic-nolib/dl.cpp:795` `dl::dlopen`, `dl::dlsym`, `dl::dlclose`, `dl::dlerror` | thin wrapper that routes to `fw::dlopen` or `fw::dlopen_fs` |
| Storage decision stubs | `src/dynamic-nolib/dl.cpp:767` weak `fw::dlopen`, `:768` weak `fw::dlopen_fs`, `:769` `g_use_flatfs` | declares the hooks the app overrides |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Get the bytes (reader) | `test/platforms/common/src/dl-semihost.cpp:7` `fw::dlopen` | builds `semihost_file` and calls `dlopen_source` |
| On-chip flash loader | `test-dynamic/.../test3/src/dl-fs.cpp:17` `fw::dlopen_fs` | builds `Flashbyte_source` over a flash blob |
| SPI flash loader | `test-dynamic/test-flatfs-spi/src/dl-fs-spi.cpp:8` `fw::dlopen_fs` | builds `SpiByteSource` at the W25Q64 offset |
| Storage routing | app's `main` sets `g_use_flatfs` (e.g. `test3/src/main-flatfs.cpp`) | `scl_impl_dlopen` (`dl.cpp:775`) then picks `dlopen_fs` by basename |
| Arena size | `test/platforms/board/ldscripts/mem.ld:12` | reserves `_simage_arena_size = 64K`; `sections.ld:84` places `.image_arena` |
| Test logic + reporting | each `test*/src/main*.cpp` → `battery_run()` in `test3/src/main-tests.cpp:165` | `open_sel`/`open_lib`, `check`, prints `PASS/FAIL`, LED blink |

**Boundary example (`scl-fw-flatfs`):** the app provisioned the fixtures into
on-chip flash and set `g_use_flatfs`; when the battery calls `fw::dlopen_fs("c2.so")`,
the app builds the flash reader and the **SCL** parses, places, relocates and
constructs the library — the app never sees a relocation, the SCL never sees a
flash offset.

---

## 2. Case: `scl::dynamic-newlib` (hosted static loader)

**Apps:** `scl-fw-dynamic-newlib1`, `scl-fw-dynamic-newlib2` (sf + hf).

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Parse + load + relocate | `src/dynamic-newlib/dynamic-linker.cpp:27` `dynamic_linker::load_from_source(byte_source&)` | parses via `common-elf-reader`, allocates on the heap, copies segments |
| Plan + apply relocation | `src/relocator.cpp` (SCL_HOSTED) | builds a `std::vector` relocation plan, applies it |
| Reversible patching | `include/dynamic-newlib/patch-journal.hpp:16` `class patch_journal`; `src/dynamic-newlib/patch-journal.cpp` | records every word written into main's memory/GOT (`record`, `record_bytes`) so `dlclose` can `revert()` cleanly |
| Library object + lifetime | `include/dynamic-newlib/dynamic-linker.hpp:26` `struct library` | holds image, journal, `init_array` pointer; init/fini handling |
| Symbol source | `include/dynamic-newlib/dynamic-linker.hpp` | parses main's `_DYNAMIC` (DT_HASH/SYMTAB/STRTAB) itself |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Hosted platform | `test/platforms/board-hosted/vendor/...` | CMSIS + HAL + newlib; `semihost_file`, `dl-load`, heap (`sbrk`), startup |
| Symbol table for the loader | app firmware exports `-Wl,-E`; `scl_embed_symtab()` (`board-hosted/cmake/platform-library.cmake:83`) | embeds a link-time symbol table into flash via `gen-symtab` |
| Fixtures | `vendor/libs/{c,cpp}` Makefiles (HF flag), `debug/test_libs`, `debug/test_hardfp` | build the `.so` set; `flash-run.sh` stages them to `/tmp` and ABI-checks |
| Battery | `test-dynamic/test-dynamic-newlib/test1|2/src/main.cpp` | calls `dl::dlopen`/`dlsym`/`dlclose`, runs the full static-unify DYN suite, reports |

**Boundary:** the **SCL** links and patches with a journal so it can revert;
the **app** must make its symbols visible (`-Wl,-E`, the embedded symtab), feed
the fixtures, and run the suite. If a symbol is missing, it is the app's export
list that is wrong — not the SCL.

---

## 3. Case: `scl::static` (standalone static patcher)

**Apps:** `f411ce-sh`, `scl-fw-static-cpp`, `scl-fw-static-direct`, `scl-fw-drv`.

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Parse | `src/static/elf-reader.cpp` (`elf_reader` adapter over `common-elf-reader`) | finds `.dynsym`/`.dynstr`, reads section headers |
| Load + patch **both** GOTs | `src/static/shlib-loader.cpp` (`shlib_loader::load_bin`, `populate_helper_sections`, `patch_shlib`, `patch_main`) | patches the library's GOT **and** main's GOT so direct calls work both ways |
| Constructors | `src/static/module.cpp` `Module::build` (line ~18 path) | runs `.init_array` after patching |
| Symbol lookup | `src/static/module.cpp` `Module::sym` | scans `.dynsym` for a name → returns runtime address |
| Front-end | `src/static/module.cpp:11` `Module::load_file`, `:18` `Module::load_buf` | from a path (semihosting) or a RAM buffer |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Firmware `.dynsym` | `test-static/test-static/CMakeLists.txt` links `libsh1/libsh2.so` with `-l:…` | so the firmware ELF carries `.dynsym`/`.rel.dyn`/`.plt`/`.got` for the loader to patch against |
| Reader | app opens `libsh1.so` over semihosting, or passes a buffer | supplies the bytes |
| Driver fixtures | `test-static/test-drv/CMakeLists.txt` `add_shlib(spi/uart/led)` | builds `libspi/uart/led.so` (WEACT pins) |
| Test logic | `test-static/*/src/main.cpp` | calls `m.sym(...)`, casts to function pointers, verifies results |

**Boundary:** the **SCL** makes a `.so` callable and patches both sides; the
**app** makes sure the `.so` is linked into the firmware image and that the
firmware exports the symbols the libraries import.

---

## 4. Case: `scl::reloc` (ET_REL module loader)

**Apps:** `scl-fw-reloc`, `scl-fw-classic-reloc`, `scl-fw-classic-spi-reloc`,
`scl-fw-reloc-interrupt`.

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Place + link + relocate + construct | `src/reloc/reloc-loader.cpp:248` `reloc::load_set(...)` | **two phases**: (1) place every module's `SHF_ALLOC` sections contiguously in the app's arena; (2) resolve each module's `R_ARM_ABS32`/`TARGET1`/`REL32`/`THM_CALL` relocations against the whole set first, then the app's resolver, then run all ctors |
| Cyclic group | `include/reloc/reloc-loader.hpp` `struct module_set` (up to `MAX_SET = 4` modules) | resolves mutual/cyclic imports between modules |
| Symbol lookup | `src/reloc/reloc-loader.cpp:286` `reloc::sym` | searches exports across all modules |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Get the `.rel.o` bytes | `test-relocatable/src/classic/main.cpp` reads over semihosting; `classic-spi/main.cpp` reads from SPI flash | into `g_img[]` staging buffers |
| Provide the arena | `test-relocatable/src/classic/main.cpp` `alignas(8) std::byte g_arena[8*1024]` | the RAM the modules are linked into |
| Symbol resolver callback | `test-relocatable/src/main.cpp` `main_resolve` | maps an undefined symbol to a runtime address (the app's own resolver over main + modules) |
| Interrupt / vector patch | `test-relocatable/src/main-interrupt.cpp:464` | the app patches the RAM vector table for TIM2 and installs a handler that calls into a loaded module |
| Battery + reporting | `test-relocatable/src/main.cpp` | 9-check battery, `PASS/FAIL` |

**Boundary:** the **SCL** links ET_REL objects into a working group in the
app's arena; the **app** supplies the object bytes, the arena, the resolver
callback, and (for the interrupt test) the vector-table patch that calls the
loaded code.

---

## 5. Case: XIP loaders (`scl::xip-*`)

**Apps:** `scl-fw-xip-nolib1`, `scl-fw-xip-newlib1`, `scl-fw-xip-static*`,
`scl-fw-xip-interrupt*`, `scl-fw-xip-static-spi-flash`.

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Classify R-X \| RW | `include/xip/segment-map.hpp` `struct SegmentMap`; `src/xip/segment-map.cpp` | decides which segment stays in flash (R-X) and which is copied to RAM (RW); `split == false` ⇒ copy-whole |
| Relocate for two bases | `src/xip/relocate.cpp` | resolves addends against text_base (flash) vs data_base (RAM) |
| Compact/derive data-base | `src/xip/compact.cpp` | derives the RAM data base / got base |
| Pre-bind host imports | `src/xip/static/adapter.cpp:127` `xip::stat::bind_host_imports` | pre-links function JUMP_SLOTs to the cross-call trampoline |
| Load (text in flash, data in RAM) | `src/static/module.cpp` `Module::load_buf` + `shlib_loader` | patches the RW copy's GOT; leaves the R-X text untouched in flash |
| Cross-library trampoline | `src/xip/xip-cross-call.c` (`xip_cross_call`), `src/xip/static/adapter.cpp` (`xip_cross_lookup`) | r10 swap so a flash PLT stub can call another module |
| Symbol lookup | `Module::sym` | returns either a **flash** (code) or **RAM** (data) address |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Provisioning pipeline | `test-xip/test-xip-static/spi-flash/src/main.cpp` | semihosting → **SPI** flash flatfs (`fw_spi::fs_add`) → copy to **internal** flash (`fw_int::fs_add`) |
| Internal-flash flatfs | `test-xip/test-xip-static/spi-flash/src/fs-archive-internal.cpp` | `fs_find`/`fs_blob` return the memory-mapped blob |
| Call helpers | `test-xip/*/include/test-helpers.hpp` `call_sym0..3` | resolve + `pic_call` into the module |
| Interrupt vector patch | `test-xip/test-xip-interrupt*/src/main.cpp` | app patches the vector table (SysTick/TIM2) to the loaded handler |
| XIP fixtures | `test-xip/test-xip-static/semihosting/fixtures-src/build.sh`, `test-xip/fixtures-src/build-xip-libs.sh` | build `-msingle-pic-base -mpic-register=r10` split `R-X|RW` `.so` |
| Walkthroughs + reporting | `test-xip/*/src/main.cpp` | execution proofs (flash vs RAM addresses), `PASS/FAIL` |

**Boundary:** the **SCL** decides *what stays in flash and what moves to RAM*
and makes cross-module calls work; the **app** decides *what to provision
where* (semihosting → SPI → internal), patches the interrupt vectors, and runs
the tests.

---

## 6. What is shared / cross-cutting

Some responsibilities are not "SCL" or "app" alone but a **contract**:

- **The symbol contract.** The SCL resolves imports against the app's exports.
  The app must export them (`-Wl,-E`, `.dynsym`, `gen-symtab` flash table,
  `-l:libsh.so`). If the app exports nothing, every load fails with
  unresolved symbols — the loader did its job; the export list is the app's.
- **The reader contract.** `loader::byte_source` (`common-elf-reader/
  include/loader/byte-source.hpp`) is a pure virtual interface (no work).
  The **library** consumes it; the **app** implements it.
- **The arena contract.** The SCL places images in memory the app reserved
  (linker `_simage_arena` for nolib, newlib heap for newlib, `g_arena` for
  reloc, caller `buf` for static/XIP).

---

## 7. Summary — the responsibility boundary after loading & parsing

| Concern | Owned by SCL | Owned by the test app |
|---|---|---|
| Parse ELF / ET_REL / ET_DYN | ✔ `elf_image::parse`, `elf_reader` | |
| Image placement (arena slot / heap / provided arena) | ✔ (uses what the app supplies) | supplies the arena / heap / buf |
| Relocation / GOT / PLT patching | ✔ `reloc_applier`/`relocator`/`shlib_loader`/`reloc::load_set` | |
| Running constructors / destructors | ✔ | |
| Symbol lookup / resolution | ✔ `dlsym`/`Module::sym`/`reloc::sym`/resolver | must **export** the symbols |
| Reversible patching (hosted) | ✔ `patch_journal` | |
| Cross-library routing / EH / lazy resolve | ✔ trampolines + port layer | enables them |
| Getting the bytes (reader) | | ✔ `byte_source` subclasses |
| Choosing the storage / provisioning | | ✔ `dlopen` vs `dlopen_fs`, flatfs, SPI→internal pipeline |
| Board init, clock, LED, console | | ✔ platform + `board.c` |
| Linker script / arena size / stack | | ✔ platform `ldscripts` |
| Fixture `.so` / `.rel.o` building | | ✔ CMake / Makefiles / scripts |
| Test battery, checks, reporting | | ✔ each `main.cpp` |

**Takeaway:** after the bytes are read, the **SCL does all the *loader* work**
— parse, place, relocate, construct, resolve — against a storage/board-neutral
contract; the **test app does all the *situational* work** — where the bytes
came from, what got provisioned, how the board is set up, and what the test
asserts. Neither side can do the other's job without breaking the boundary:
the SCL has no idea what a WEACT/NUCLEO is, and the app never re-implements a
single relocation or constructor walk.

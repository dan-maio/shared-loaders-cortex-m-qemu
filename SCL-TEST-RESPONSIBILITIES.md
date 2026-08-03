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

---

## 1. Case: `scl::dynamic-nolib` (freestanding `dlopen`)

**Apps:** `scl-fw-dyn1`, `scl-fw-dyn2`, `scl-fw-gotplt-demo`, `scl-fw-flatfs`,
`scl-fw-flatfs-spi`, `scl-fw-flatfs-bitbang-spi`, `scl-fw-ovl1`.

| Phase after load/parse | SCL (`src/dynamic-nolib`, `src/`, `common-elf-reader`) | Test app (platform + `test-*` `main`) |
|---|---|---|
| Get the bytes | — (no storage knowledge) | builds the reader: `fw::dlopen`/`fw::dlopen_fs` → `semihost_file`/`Flashbyte_source`/`SpiByteSource` |
| Reserve image memory | `image_arena()` + `region_allocator` hand out slots in `_simage_arena` | declares the arena size in the linker script (`_simage_arena_size = 64K`, `mem.ld`) |
| Parse ELF, copy segments, relocate, resolve, run `.init_array` | `fw::dlopen_source` (`dl.cpp`) — the whole engine | — (calls it) |
| Choose the storage | weak `fw::dlopen` / `fw::dlopen_fs` stubs | strong definitions per test: `dl-semihost.cpp`, `dl-fs.cpp`, `dl-fs-spi.cpp`; `g_use_flatfs` routing |
| C-API facade | `dl::dlopen`/`dlsym`/`dlclose`/`dlerror` (`dl.cpp`) | calls it; interprets handles |
| Cross-lib routing / lazy resolve / EH | `dl_cross_call`, `lazy-resolve`, `exidx` (SCL) | enables/selects them (`dl_enable_cross_call`, `dl_cross_trace`) |
| Test logic + reporting | — | the battery (`battery_run`, `open_lib`, `check`), `DL_LIB_DIR`, LED, `PASS/FAIL` |
| Console / board | — | semihosting `trace_*`, `board_init`, LED |

**Boundary:** the app decides *which* storage and *whether* to use flatfs; the
SCL decides *how* an image is turned into a callable library. The app never
touches relocation; the SCL never touches a file/probe/flash.

---

## 2. Case: `scl::dynamic-newlib` (hosted static loader)

**Apps:** `scl-fw-dynamic-newlib1`, `scl-fw-dynamic-newlib2` (sf + hf).

| Phase after load/parse | SCL (`src/dynamic-newlib`, `src/`) | Test app (hosted platform + `main`) |
|---|---|---|
| Get the bytes | — | `semihost_file` over the hosted platform; `/tmp` fixture staging |
| Parse + load + relocate | `dynamic_linker::load_from_source`, `relocator` (std::vector plan), `patch-journal` | — |
| Reversible GOT patching | `patch-journal` records/reverts patches on `dlclose` | — |
| Constructors / library lifetime | `library` struct, init/fini | — |
| Symbol source for the loader | reads it from the firmware's `_DYNAMIC` | app must **export** those symbols (`-Wl,-E`, anchors, `scl_embed_symtab` flash table) |
| Heap | — (uses it) | hosted platform provides newlib heap (`sbrk`) |
| Fixture `.so` files | — | built by `vendor/libs/{c,cpp}` Makefiles, staged to `/tmp`, ABI-verified |
| Battery + reporting | — | `main.cpp` DYN suite, `dl::` facade, `PASS/FAIL` |

**Boundary:** the SCL links/patches/reverts; the app supplies the exported
symbols, the fixtures, the heap, and the test narrative.

---

## 3. Case: `scl::static` (standalone static patcher)

**Apps:** `f411ce-sh`, `scl-fw-static-cpp`, `scl-fw-static-direct`, `scl-fw-drv`.

| Phase after load/parse | SCL (`src/static`) | Test app (`test-static/*`) |
|---|---|---|
| Get the bytes | — | `Module::load_file`/`load_buf` over semihosting / a RAM buffer |
| Parse | `elf_reader` (adapter over `common-elf-reader`) | — |
| Load + patch both GOTs | `shlib_loader` patches the **library's** and **main's** GOT slots | — |
| Constructors | `Module::build` runs `.init_array` | — |
| Symbol lookup | `Module::sym` | calls it, then calls the function pointers |
| `.dynsym` in the firmware ELF | — (needs it) | links the `.so` files into the firmware (`target_link_libraries`) so `.dynsym`/`.rel.dyn`/`.plt`/`.got` exist; `gen-symtab` table |
| Driver fixtures (`libspi/uart/led`) | — | `add_shlib` CMake in `test-drv`; board pins (WEACT-only) |
| Test logic + reporting | — | `main.cpp`, `call`/verify, LED, `PASS/FAIL` |

**Boundary:** the SCL turns a `.so` into a patched, callable image; the app
makes sure the `.so` and the firmware's symbol table exist and drives the test.

---

## 4. Case: `scl::reloc` (ET_REL module loader)

**Apps:** `scl-fw-reloc`, `scl-fw-classic-reloc`, `scl-fw-classic-spi-reloc`,
`scl-fw-reloc-interrupt`.

| Phase after load/parse | SCL (`src/reloc`, `src/`) | Test app (`test-relocatable/*`) |
|---|---|---|
| Get the bytes | — | raw `.rel.o` spans read over semihosting / SPI into `g_img[]` |
| Place modules + link cycles + apply relocs + constructors | `reloc::load_set` into the supplied arena | — |
| Where modules live | — (needs it) | supplies `g_arena`/`g_arena2` (8 KB each) |
| Symbol source | `loader::symbol_source` resolver callback | supplies `main_resolve` (the app's resolver over main's symbols) |
| Symbol lookup | `reloc::sym` | calls it, then calls function pointers |
| Interrupt / vector patching | — | app patches the RAM vector table (TIM2) and installs a handler |
| Test logic + reporting | — | the 9-check battery, `PASS/FAIL` |

**Boundary:** the SCL links relocatable objects into a working module group in
the app-provided arena; the app feeds the object bytes, the arena, the symbol
resolver, and the test logic.

---

## 5. Case: XIP loaders (`scl::xip-*`)

**Apps:** `scl-fw-xip-nolib1`, `scl-fw-xip-newlib1`, `scl-fw-xip-static*`,
`scl-fw-xip-interrupt*`, `scl-fw-xip-static-spi-flash`.

| Phase after load/parse | SCL (`src/xip`, `src/static`, `src/`) | Test app (`test-xip/*`) |
|---|---|---|
| Provisioning | — | semihosting → **SPI** flash → **internal** flash flatfs (`fs_*`) |
| Classify R-X \| RW | `xip::SegmentMap` | — |
| Bind host imports / pre-link | `xip::stat::bind_host_imports` | — |
| Load (text in flash, data in RAM) | `scl::Module::load_buf` + `shlib_loader` | supplies `flash_blob` from internal flash + a RAM buffer |
| Cross-library trampoline | `xip_cross_call` / `xip_cross_lookup` | — |
| Call into modules | — (helper) | `call_sym0..N` in the app's `test-helpers.hpp` |
| Interrupt (TIM2/SysTick) dynamic vector | — (helper: `xip_cross_call`) | app patches the vector table and installs the handler |
| Fixtures (XIP split) | — | `fixtures-src/build-xip-drv.sh` builds `-msingle-pic-base` `slib/mylib/c1/…` |
| Test logic + reporting | — | walkthroughs, execution proofs, `PASS/FAIL` |

**Boundary:** the SCL decides *what stays in flash* and *what moves to RAM* and
makes cross-module calls work; the app decides *what to provision where* and
drives the whole pipeline and the tests.

---

## 6. Summary — the responsibility boundary after loading & parsing

| Concern | Owned by SCL | Owned by the test app |
|---|---|---|
| Parse ELF / ET_REL / ET_DYN | ✔ (`common-elf-reader`, `src/`) | |
| Image placement (RAM arena slot / heap / provided arena) | ✔ (uses what the app supplies) | supplies the arena / heap |
| Relocation / GOT / PLT patching | ✔ | |
| Running constructors / destructors | ✔ | |
| Symbol lookup / resolution source | ✔ resolver + `sym`/`dlsym` | app must *export* the symbols (`-Wl,-E`, `.dynsym`, symtab table) |
| Reversible patching (hosted) | ✔ `patch-journal` | |
| Cross-library routing / EH / lazy resolve | ✔ | enables them |
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

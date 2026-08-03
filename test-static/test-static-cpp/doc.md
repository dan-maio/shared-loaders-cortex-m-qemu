# shared-static-rework

Bare-metal STM32F411CE (Cortex-M4F) firmware with an in-house ELF32 shared library loader.
Loads PIC `.so` files at runtime from RAM, performs GOT patching + relocation fixup,
resolves weak symbols, and runs constructors — all without an MMU or OS.

> **Part of the SCL — this is the `scl::static` demo app.** This firmware lives
> at `test-static/test-static/` inside the `shared-lib-loaders` repo. The loader itself
> (`elf_reader` adapter + `shlib_loader` + `logger`) is the SCL module
> **`scl::static`** (`src/static` + `include/static`); this folder is just the
> demo application (`main.cpp`, `system/`, `ldscripts/`, `libsh1/2`).
>
> **ELF reading/parsing is shared, not in-house.** The ELF32 reader/parser
> (`elf_image`, `byte_source`, ELF32 types, `error`/`result`) lives in
> **[`common-elf-reader`](../../common-elf-reader)** (`elf32_read_parse.git`), a
> **git submodule** at the repo root (`../../common-elf-reader` from here),
> shared with the other loaders. `elf_reader` is a thin adapter over it; the GOT
> patching, relocation fixup and symbol resolution below are the loader's own.
> The **CMake** build links `scl::static` (which pulls the parser transitively);
> the **Makefile** compiles the loader from `../../src/static` and the parser
> from `../../common-elf-reader` directly. See
> [`integration-common-elf-reader.md`](integration-common-elf-reader.md).

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  f411ce-sh.elf                  │
│  (STM32F411 firmware: HAL + loader + main)      │
│                                                  │
│  ┌──────────────┐   ┌──────────────────────────┐│
│  │  elf_reader  │   │     shlib_loader          ││
│  │  .cpp/.h     │   │     .cpp/.h               ││
│  │              │   │                          ││
│  │ - Parses     │   │ - load_bin() segments    ││
│  │   ELF headers│   │ - patch_shlib() GOT      ││
│  │ - Reads      │   │ - patch_main() refs      ││
│  │   .dynsym /  │   │ - execute_constructors() ││
│  │   .rel.dyn   │   │ - resolve_weak_function()││
│  │ - Hash-based │   └──────────────────────────┘│
│  │   sym lookup │                                │
│  └──────────────┘                                │
└─────────────────────────────────────────────────┘
         ▲                    ▲
         │ load + patch       │ load + patch
    ┌────┴─────┐         ┌────┴─────┐
    │libsh1.so │         │libsh2.so │
    │(ET_DYN)  │         │(ET_DYN)  │
    │PIC .so   │         │PIC .so   │
    │fPIC      │         │fPIC      │
    │RAM@0     │         │RAM@0     │
    └──────────┘         └──────────┘
```

### Components

**Shared Libraries** (`libsh1/`, `libsh2/`)
- Compiled as `ET_DYN` with `-fPIC -shared`
- Linked at RAM address 0x00000000 (position-independent)
- Export functions, variables, constructors, and weak symbols
- `libsh2` can call into `libsh1` or the main app

**ELF Reader** (`elf-reader.cpp/h`) — adapter over the shared parser
- Thin adapter over `loader::elf_image` + `loader::fd_byte_source` from
  [`common-elf-reader`](../../common-elf-reader); the shared parser validates
  magic bytes, class, endianness, type (`ET_DYN`) and machine (`EM_ARM`) and
  provides header/segment/section access via a `byte_source` (no raw fd here)
- Keeps this project's own resolving/patching helpers: hash-based symbol
  lookup (`find_symbol_address`), relocation/dynamic-symbol reading, and the
  cross-library `symbol_table`
- The `Elf32_*` (`<elf.h>`) public API and the `loader::elf::*` parser types
  are layout-identical and bridged by `memcpy`
- String-table reads go through `loader::detail::name_at`
  (`loader/detail/bytes.hpp`, in `common-elf-reader`) — a shared,
  bounds-checked reader that never scans past the table on a malformed
  `st_name`. The local `name_at` is a thin `(ptr, size)`→span wrapper over it,
  so both projects share one copy of the byte-access helpers
  (`load_word` / `load_byte` / `store_word` / `name_at`)

**Shared Library Loader** (`shlib-loader.cpp/h`)
- Loads ELF PT_LOAD segments into RAM
- Populates helper sections (`.rel.dyn`, `.dynsym`, `.dynstr`, `.hash`, `.inits`)
- Patches library GOT (`R_ARM_RELATIVE`, `STB_GLOBAL`, `STB_WEAK`)
- Patches main executable references to library symbols (`R_ARM_COPY`, global/weak)
- Executes constructors (`.init_array`)
- Resolves weak function overrides

**Main Application** (`main.cpp`)
- Loads `libsh1.so` then `libsh2.so`
- Calls library functions, accesses exported variables, uses structs
- Demonstrates weak symbols, function pointers, math library linkage
- Blinks LEDs via µOS++ framework

---

## ELF32 Specification Compliance

| Check | Status | Detail |
|-------|--------|--------|
| Magic (`EI_MAG0..3`) | ✅ | `ELFMAG0..3` checked in `verify_elf()` |
| Class (32-bit) | ✅ | `Elf32_Ehdr`, `Elf32_Phdr`, `Elf32_Shdr`, `Elf32_Sym`, `Elf32_Rel` |
| Endianness | ✅ | Little-endian (ARM default) |
| Type (`ET_DYN`) | ✅ | `e_type == ET_DYN (3)` enforced |
| Machine (`EM_ARM`) | ✅ | `e_machine == EM_ARM (0x28)` enforced |
| Program headers (`PT_LOAD`) | ✅ | Segments loaded respecting `p_filesz` / `p_memsz` (zero-fills BSS) |
| Section string table | ✅ | Via `e_shstrndx` |
| `.dynsym` / `.dynstr` / `.hash` | ✅ | Parsed; `elf_hash()` matches ELF System V ABI |
| Relocations (`Elf32_Rel`) | ✅ | `R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`, `R_ARM_COPY` |
| Symbol binding | ✅ | `STB_LOCAL` / `STB_GLOBAL` / `STB_WEAK` via `ELF32_ST_BIND` |
| `DT_INIT_ARRAY` | ✅ | `execute_constructors()` runs `.init_array` |
| Segment permissions (`p_flags`) | ✅ | `load_segment` struct + `flags_to_string()` prints `R/W/X` per segment |
| `R_ARM_COPY` | ✅ | Handled in `patch_main()` for data symbols |

---

## RWX Segment Handling

Cortex-M4 has **no MMU** — memory permission attributes at the ELF level are metadata only.
The firmware accepts RWX where necessary and suppresses spurious linker warnings.

### Main executable (`f411ce-sh.elf`)

| Segment | Virtual addr | Permissions | Notes |
|---------|-------------|-------------|-------|
| LOAD #0 | `0x08000000` (FLASH) | `RWE` | Code + dynamic sections. W is harmless: flash hardware is read-only. |
| LOAD #1 | `0x20000000` (RAM) | `RW` | `.data`, `.dynamic`, `.got` |
| LOAD #2 | `0x20000304` (RAM) | `RW` | `.bss`, `.noinit`, stack |

- `ldscripts/mem-sh.ld`: RAM regions declared `(rw)` — no executable sections in RAM
- `-Wl,--no-dynamic-linker`: suppresses `.interp` and `PT_PHDR`
- Dynamic sections (`.dynsym`, `.dynstr`, `.hash`, `.rel.dyn`) explicitly placed in FLASH
- `/DISCARD/ { *(.interp) }` in linker script

### Shared libraries (`libsh1.so`, `libsh2.so`)

| Segment | Virtual addr | Permissions | Notes |
|---------|-------------|-------------|-------|
| LOAD #0 | `0x00000000` | `RWX` | Code + GOT + data coexist at base-relative addresses |

- RWX is required because code, writable GOT, and data share the same address space
- No MMU means page-level permission separation is impossible
- `-Wl,--no-warn-rwx-segments` suppresses the false-positive linker warning

### Loader changes (`elf-reader.h`, `elf-reader.cpp`)

```cpp
struct load_segment {
    uint32_t type;
    uint32_t vaddr;
    uint32_t memsz;
    uint32_t filesz;
    uint32_t offset;
    uint32_t flags;   // PF_R, PF_W, PF_X
    uint32_t align;
};

uint32_t get_program_header_count();
void get_segment_info(uint32_t nbr, load_segment& seg);
static const char* flags_to_string(uint32_t flags);  // "R--", "RW-", "R-X", "RWX"
```

`shlib_loader::load_bin()` now prints each segment with its permissions at load time:
```
  [00] LOAD vaddr=0x0000 filesz=0x254c memsz=0x2558 perm=RWX
```

---

## Build

### Toolchain

```
arm-none-eabi-gcc 15.2.1 (xPack) @ /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
```

### Dependency

The shared ELF32 reader/parser must sit beside this repo at
`../../common-elf-reader` (clone `elf32_read_parse.git`). The Makefile references
it via `COMMON_ELF_READER = ../../common-elf-reader` — it adds
`-I$(COMMON_ELF_READER)/include` and compiles
`$(COMMON_ELF_READER)/src/elf-image.cpp`. Point it elsewhere by overriding that
variable: `make COMMON_ELF_READER=/abs/path`.

### Build with Make

```console
$ make clean && make
```

| Target | text | data | bss | dec | hex |
|--------|------|------|-----|-----|-----|
| `libsh1.so` | 5337 | 296 | 12 | 5645 | 160d |
| `libsh2.so` | 1829 | 216 | 4 | 2049 | 801 |
| `f411ce-sh.elf` | 84100 | 808 | 2188 | 87096 | 15438 |

Output files (in the repo root): `f411ce-sh.elf`, `f411ce-sh.bin`,
`libsh1/libsh1.so`, `libsh2/libsh2.so`.

Override the toolchain or the shared-parser location on the command line:

```console
$ make TOOLCHAIN_PATH=/path/to/arm-none-eabi/bin COMMON_ELF_READER=/path/to/common-elf-reader
```

### Build with CMake

An equivalent CMake build is provided (`CMakeLists.txt` +
`cmake/arm-none-eabi.cmake`). It builds the same two `.so` libraries and the
firmware, out-of-source under `build/`:

```console
$ cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
$ cmake --build build
```

Recommended invocation (from the repo root, its own build tree):

```console
$ cmake -S test-static/test-static -B test/build-static \
        -DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake
$ cmake --build test/build-static
```

- Firmware ELF: `f411ce-sh.elf` in the build dir (+ `.bin` and `.elf.lst`); the
  `.so` libraries are written next to their sources (`libsh1/`, `libsh2/`).
- The app links **`scl::static`** — a standalone configure pulls the SCL root
  (`add_subdirectory`) to provide it. `scl::static` is an **INTERFACE** target,
  so the loader (`elf_reader`/`shlib_loader`/`logger`) **and** the
  `common-elf-reader` parser compile **from source** into `f411ce-sh` with the
  app's flags — no `.a` archive. The Makefile does the equivalent by listing the
  loader sources (`../../src/static/*.cpp`) and `elf-image.cpp` directly.
- Override the toolchain with `-DTOOLCHAIN_PATH=/path/to/arm-none-eabi/bin`.
- `COMMON_ELF_READER_DIR` defaults to the repo-root submodule
  (`../../common-elf-reader`); configuration fails fast if it is missing (run
  `git submodule update --init`).

The Make and CMake outputs are functionally identical; the CMake ELF is a few
hundred bytes larger in `.text` because CMake adds its implicit C++ link
libraries. The hardware run scripts below drive the **Make** build.

### Run on hardware

```
build-and-run.sh               # make  + openocd + gdb (semihosting)
build-and-run-cmake.sh         # cmake + openocd + gdb (flashes build/f411ce-sh.elf)
build-and-run-openocd-only.sh  # make  + openocd flash + resume
```

`build-and-run.sh` and `build-and-run-openocd-only.sh` drive the **Make** build
(ELF in the repo root); `build-and-run-cmake.sh` drives the **CMake** build and
flashes `build/f411ce-sh.elf`. All three pass `libsh1/libsh1.so libsh2/libsh2.so`
via the semihosting command line (paths relative to the repo root).

---

## Known issues

All previously-tracked warnings have been resolved; the build is clean apart from
the intentional board-assumption `#warning` reminders in `main.cpp` /
`initialize-hardware.c`.

Fixed:
- `log::hex()` / `hex16()` were non-reentrant (single static buffer), so multiple
  hex arguments in one log call all printed the last value. They now rotate
  through a ring of `HEX_RING` static buffers.
- `elf_reader` now checks every `read()` / `lseek()` for short reads and reports
  a `loader_error` instead of parsing garbage on a truncated/corrupt file.
- `find_section_by_name()` returns an unambiguous `0xFFFFFFFF` sentinel instead of
  overloading index `0` (the null section) as "not found".
- Removed dead `ww` / `ww2` declarations/definitions in the demo libraries.
- `timer.cpp` no longer uses deprecated `--` on a `volatile`.
- Demo `str_lib` initializers use writable buffers instead of string literals
  assigned to `char*`.

### Resolving / patching hardening

- `patch_shlib` no longer writes a GOT slot for registration-only entries
  (dynamic-symbol-table additions carry type `R_ARM_NONE` / `got_address 0`).
  These previously wrote to `buffer[0]`, clobbering offset 0 of the loaded
  image; they now only publish the symbol for interposition — matching the
  guard `patch_main` already had.
- Every patch write is bounds-checked: shlib relocation offsets against the
  loaded image size (`r_offset` comes from the untrusted library), and main
  slots for null / word-alignment.
- `R_ARM_ABS32` is dispatched by relocation type for **any** binding, so a
  local `ABS32` is applied rather than silently dropped.
- `resolve_dependencies` keeps each auto-loaded `DT_NEEDED` dependency's loader
  alive in `loaded_deps_`; a local temporary previously dropped the parsed
  image and section maps at end of iteration.
- `R_ARM_COPY` is documented as a deliberate *address-injection* (store the
  library symbol's runtime address into main's slot), **not** standard
  data-copy semantics — the demo depends on it and `st_size` is unused.
- Symbol-name reads (in relocation/dynsym decoding and hash lookup) go through
  the shared `loader::detail::name_at` (`common-elf-reader`), which is bounded
  by the `.dynstr` size so a crafted `st_name` past the table yields an empty
  view instead of an over-read.

# XIP Dynamic-Newlib Test (`scl-fw-xip-newlib1`)

## What This Test Does

Loads 10 shared libraries (`.so`) with **Execute-In-Place (XIP)** using the hosted
`dynamic-newlib` loader. Library code executes directly from flash (R-X segment
stays in flash); only the RW data segment is copied to RAM. This saves ~15 KB
of RAM compared to loading everything into RAM.

**Fixtures loaded:** mylib1, mylib2, c1, c2, cpp3, libprov, libcons, libcppeh,
libcppsh, ehx_catch — all XIP-verified (split R-X|RW segments, immutable text).

## Why a Separate Build

This test **cannot** be built from the aggregate `test-xip/` tree because:

1. The aggregate uses `arm-none-eabi.cmake` (freestanding toolchain)
2. This test needs `SCL_HOSTED=ON` and the hosted toolchain (`arm-none-eabi-hosted.cmake`)
3. CMake does not support mixing two toolchain files in one tree

The other 7 XIP tests use the freestanding toolchain and build from the aggregate.
This one requires its own standalone build.

## Build Steps

### Step 1: Build the `gen-symtab` Host Tool

The XIP newlib loader needs a **symbol table embedded in flash** so it can resolve
library imports at runtime without semihosting. The `gen-symtab` C++20 host tool
extracts symbols from the linked firmware ELF and writes a binary blob into the
`.scl_symtab` section.

```bash
# From the repo root — compile the host tool
tools/build.sh
# Produces: tools/gen-symtab (x86-64 Linux binary)
```

Or let CMake build it automatically (the `gen-symtab-host` custom target handles
this — see Step 2).

### Step 2: Build the Firmware

```bash
cd test-xip/test-xip-dynamic-newlib/test1

cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=../../../../cmake/arm-none-eabi-hosted.cmake \
    -DSCL_HF=ON

cmake --build build -j
```

**What happens during the build:**

1. CMake compiles `gen-symtab.cpp` + `elf-image.cpp` into `tools/gen-symtab`
   (the `gen-symtab-host` custom target)
2. The firmware is compiled and linked → `scl-fw-xip-newlib1.elf`
3. **POST_BUILD step:** `gen-symtab` parses the linked ELF, extracts 299
   global/weak symbols, writes `symtab.bin`
4. `objcopy --update-section .scl_symtab=symtab.bin` embeds the table
5. `objcopy -O ihex` produces the `.hex` for flashing

**Expected output:**
```
[100%] Built target scl-fw-xip-newlib1
Embedding loader symbol table into scl-fw-xip-newlib1
gen-symtab: 299 symbols, 7548 bytes used of 12288 at 0x803bc24
```

### Step 3: Flash and Run

```bash
# From the repo root
source tools-config.sh
test/tools/flash-run-hf.sh test-xip/test-xip-dynamic-newlib/test1/build/scl-fw-xip-newlib1.hex
```

**Expected output:**
```
scl-fw-xip-newlib1: full-surface XIP + cross-lib C/C++ test
[symtab] 299 symbols from flash
  loaded: XIP;  library_function @ 0x08040d69 (FLASH)
  loaded: XIP;  lf @ 0x08042809 (FLASH)
  c2 loaded (XIP); got_base=0x200053a4
  c1 loaded (XIP); got_base=0x200054bc
  cpp3 loaded (XIP); got_base=0x200055cc
  ...
scl-fw-xip-newlib1: done
```

All libraries should show `loaded: XIP` (code executing from flash, not copy-whole).

## How the Symbol Table Works

```
Build time:
  Link firmware → gen-symtab parses ELF → symtab.bin → objcopy embeds in .scl_symtab

Boot time:
  Firmware starts → xip::newlib::set_symtab() reads .scl_symtab blob
  → parses (magic 0x534C4353, count, (name_addr, value)[] entries)
  → populates link_context's symbol table

Load time:
  xip::newlib::dlopen("c1.so") → parse ELF → classify segments
  → resolve "printf" via symbol table → patch GOT → run constructors
```

## Symbol Pruning

Linking newlib and libstdc++ pulls in thousands of symbols. Without filtering,
the table would be ~87 KB — too large for STM32F411 flash. The tool filters:

- **Keeps:** Global/Weak symbols with non-zero value
- **Removes:** Symbols starting with `_` unless whitelisted:
  `_impure_ptr`, `_Znw` (new), `_Zdl` (delete), `__cxa` (exceptions),
  `__errno`, `__exidx`, `__gnu_Unwind`, `__register/deregister_frame_info`

**Result:** 299 symbols, ~7.5 KB (fits in 12 KB `.scl_symtab` section).

## Key Files

| File | Purpose |
|------|---------|
| `tools/gen-symtab.cpp` | C++20 host tool source |
| `tools/build.sh` | Manual compilation script |
| `CMakeLists.txt:141-150` | POST_BUILD integration |
| `src/sections-flatfs-hosted.ld:96-98` | `.scl_symtab` section reservation |
| `src/main.cpp:33-47` | `scl_symtab_blob[]` parsing at boot |

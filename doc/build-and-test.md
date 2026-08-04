# Build & Test — all modules

The one build/test recipe for the whole SCL. Run everything from the **repo
root**; the SCL content *is* the root, with the `common-elf-reader` parser
**vendored in place** at `./common-elf-reader` (no submodule, no links to
external projects — everything is local). Three targets are supported:
**WEACT-F411CE**, **NUCLEO-F411RE**, and **QEMU** (emulated STM32F411RE). Only
board boot/init differs (`board=weact|nucleo|qemu`).

## The modules and where their tests live

| Module (target) | Code | Tested by | Hardware? |
|---|---|---|---|
| `scl::common` (core parser/resolver) | `src/`, `include/loader/` (+ submodule) | every firmware suite | yes |
| `scl::dynamic-nolib` (freestanding `dlopen`) | `src/dynamic-nolib/` | `test-dynamic/test-dynamic-nolib` → `dyn1/dyn2/flatfs` (§2, §5) | board |
| `scl::dynamic-newlib` (hosted static loader) | `src/dynamic-newlib/` | `test-dynamic/test-dynamic-newlib` → `newlib1/newlib2`, SF+HF (§3, §5) | board |
| `scl::static` (ELF32 static patcher) | `src/static/` | `test-static/test-static` → `f411ce-sh` (§4) | board |
| `scl::reloc` (ET_REL module loader) | `src/reloc/` | `test-relocatable/` → `scl-fw-reloc` (§6) | board |
| `scl::overlay` (overlay window manager) | `src/overlay/` | `ovl1` firmware (§2, §5) | board |
| `scl-xip-common` (XIP shared core) | `src/xip/` | XIP tests (§7) | board |
| `scl-xip-dynamic-nolib` | `src/xip/dynamic-nolib/` | `test-xip/` → `nolib1` (§7) | board |
| `scl-xip-dynamic-newlib` | `src/xip/dynamic-newlib/` | `test-xip/` → `newlib1` (§7) | board |
| `scl-xip-static` | `src/xip/static/` | `test-xip/` → `static/*` (§7) | board |

**What needs hardware:** every suite is a firmware pass/fail
gate. §2–§4 are **compile checks** without a board; the actual on-target runs are
§5 (and `test-static/test-static`'s own run scripts in §4). §2–§3 exercise the shared
parser on both soft- and hard-float ABIs.

## 0. Setup

```sh
# ARM toolchain on PATH (the vendored fixture Makefiles call objdump directly)
export PATH=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH

# optional: full clean
./cleaner.sh   # or: rm -rf test-dynamic/build test/build-dynamic-newlib test/build-dynamic-newlib-hf \
       test/build-static test/platforms/board-hosted/vendor/libs/build*
```

> **`CMake Warning: … CMAKE_TOOLCHAIN_FILE … not used`?** The toolchain file is
> read once, on the **first** configure of a build dir, and baked into its cache.
> Re-running `cmake … -B <existing-tree>` ignores the flag (that warning); if the
> tree was first configured without it, the cache is locked to the **host**
> compiler (`/usr/bin/c++`) — the ARM app then builds with the wrong compiler, or
> hosted apps self-skip and produce nothing ("firmware .hex not found" later).
> Fix: **delete that `-B` directory** and configure again. Verify with
> `grep CMAKE_CXX_COMPILER: <tree>/CMakeCache.txt` → expect `arm-none-eabi-g++`.


## 2. Freestanding firmware apps (`scl::dynamic-nolib` + `scl::overlay`) — compile check

Builds 4 apps. The output ELF is the **extension-less** target file; `.hex`/`.bin`
sit beside it.

```sh
cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-dynamic/build -j
```

| App (target / ctest) | Output dir | What it runs on-target (§5) |
|---|---|---|---|
| `scl-fw-dyn1`   | `test-dynamic/build/test-dynamic-nolib/test1/` | dlopen/dlsym/call/dlclose demo (`mylib1/2`, C++, exceptions), reload proof |
| `scl-fw-dyn2`   | `test-dynamic/build/test-dynamic-nolib/test2/` | 16-test cross-module battery (r10 routing, lazy vs eager, GOT/PLT walkthrough) |
| `scl-fw-gotplt-demo` | `test-dynamic/build/test-dynamic-nolib/test4/` | GOT[2]/PLT0 standard ARM ABI demonstration, GOT walkthrough |
| `scl-fw-flatfs` | `test-dynamic/build/test-dynamic-nolib/test3/` | same battery, fixtures from an **on-chip flat filesystem** |
| `scl-fw-ovl1`   | `test-overlay/build//test1/`       | overlays-vs-dynamic comparison (`scl::overlay` + `scl::dynamic-nolib`) |

## 3. Hosted newlib static apps (`scl::dynamic-newlib`) — compile check, both ABIs

Builds `scl-fw-dynamic-newlib1/2` **twice**, one tree per float ABI (same target
names — the *build tree* and the staged fixture set differ). Fixtures build
automatically.

```sh
# SOFT float  -> tree test/build-dynamic-newlib,    fixtures libs/build    (11 .so)
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib    -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake
cmake --build test/build-dynamic-newlib -j

# HARD float  -> tree test/build-dynamic-newlib-hf, fixtures libs/build_hf (13 .so, +hf_test1/2)
cmake -S test-dynamic/test-dynamic-newlib -B test/build-dynamic-newlib-hf -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake -DSCL_HF=ON
cmake --build test/build-dynamic-newlib-hf -j
```

| App | Soft-float output | Hard-float output |
|---|---|---|
| `scl-fw-dynamic-newlib1` | `test/build-dynamic-newlib/test1/`    | `test/build-dynamic-newlib-hf/test1/` |
| `scl-fw-dynamic-newlib2` | `test/build-dynamic-newlib/test2/`    | `test/build-dynamic-newlib-hf/test2/` |

Sanity-check a tree: `grep CMAKE_CXX_COMPILER: test/build-dynamic-newlib/CMakeCache.txt`
→ `arm-none-eabi-g++`.

## 4. The static patcher (`scl::static`) — `test-static/test-static`

A **self-contained** firmware app (its own µOS++ `system/`, soft-float
`ldscripts/`, and `libsh1/2` demo `.so`) that links `scl::static` — the loader +
the `common-elf-reader` submodule parser compile straight into `f411ce-sh.elf`.
It is **not** part of the `test/` CMake tree; configure it on its own.

**Build (CMake, INTERFACE style):**
```sh
cmake -S test-static/test-static -B test/build-static \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/test-static/test-static/cmake/arm-none-eabi.cmake
cmake --build test/build-static -j          # -> test/build-static/f411ce-sh.elf (+ libsh1/2.so)
```

**Build with CMake + run on the board — one shot** (WeAct BlackPill STM32F411CE attached).
Run **from the app dir** (`test-static/test-static`), since the script uses `-S .`,
`openocd.cfg`, and relative `libsh1/2.so` paths:
```sh
cd test-static/test-static
./build-and-run-cmake.sh
```
`build-and-run-cmake.sh` does the whole cycle:
1. `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` then
   `cmake --build build` → `build/f411ce-sh.elf`;
2. starts OpenOCD (`openocd.cfg`), and on exit tears it down;
3. runs GDB `-batch`: `load`s the ELF and `continue`s with semihosting enabled
   and the command line `myprogram libsh1/libsh1.so libsh2/libsh2.so`, so the
   patcher opens both demo `.so` over semihosting and the run streams back to the
   console.

`./build-and-run-openocd-only.sh` just starts OpenOCD for a manual GDB session.

Single soft-float image (no `SCL_HOSTED`/ABI switch). Loads `libsh1/libsh1.so` +
`libsh2/libsh2.so` over semihosting, patches their GOTs, runs constructors, and
calls across the boundary. Details in `test-static/test-static/doc.md`.

## 5. Run the test apps on the board — NUCLEO-F411RE or WEACT-F411CE

Both boards share this one codebase; only board boot/init and the OpenOCD flash
config differ (NUCLEO = ST-LINK, WEACT = CMSIS-DAP). Pick the board with
`board=` (default `weact`), pick the app with `test=`:

```sh
# one-shot: configure + build + flash + run
./build-and-run.sh board=weact  test=dyn2
./build-and-run.sh board=nucleo test=ovl1
./build-and-run.sh board=nucleo test=newlib2-sf abi=sf
```

`build-and-run.sh` prints the full test-name list with no arguments. The SPI
apps (`flatfs-spi`, `flatfs-bitbang-spi`, `drv`, `classic-spi`,
`xip-static/spi-flash`) are WEACT-only (the NUCLEO has no SPI wiring) and the
script refuses `board=nucleo` for them.

Run one at a time; output comes back over the semihosting console.

**A. Flash wrappers** (resolve an app NAME to its build tree; set `SCL_BOARD`
to pick the OpenOCD flash config — ST-LINK for `nucleo`, CMSIS-DAP for `weact`;
the `-sf`/`-hf` wrappers also re-stage the matching fixtures and verify the ABI
first):

**A. Flash wrappers** (resolve an app NAME to its build tree; the `-sf`/`-hf`
wrappers also re-stage the matching fixtures and verify the ABI first):

```sh
# freestanding apps (any one)
test/tools/flash-run.sh    scl-fw-dyn1
test/tools/flash-run.sh    scl-fw-dyn2
test/tools/flash-run.sh    scl-fw-gotplt-demo
test/tools/flash-run.sh    scl-fw-flatfs
test/tools/flash-run.sh    scl-fw-ovl1

# hosted newlib static apps — pick the wrapper matching the ABI you built
test/tools/flash-run-sf.sh scl-fw-dynamic-newlib1   # or newlib2  — SOFT float
test/tools/flash-run-hf.sh scl-fw-dynamic-newlib2   # or newlib1  — HARD float
```

`flash-run.sh` takes a valid app NAME (newest matching `.hex`) or an explicit
`.hex` path; a typo prints the valid names, a missing `.hex` prints the build
command. `flash-run-sf.sh`/`flash-run-hf.sh` just set
`SCL_DYNAMIC_NEWLIB_VARIANT=sf|hf` (→ `build-dynamic-newlib`+`libs/build` vs
`build-dynamic-newlib-hf`+`libs/build_hf`) and call `flash-run.sh`.

**B. Via CTest** (each app registers a flash-and-run test in its tree — **board
required**):

```sh
ctest --test-dir test-dynamic/build               --output-on-failure   # 5: dyn1/dyn2/gotplt-demo/flatfs/ovl1
ctest --test-dir test/build-dynamic-newlib   --output-on-failure   # 2: newlib1/newlib2 (SOFT)
ctest --test-dir test/build-dynamic-newlib-hf --output-on-failure  # 2: newlib1/newlib2 (HARD)
```

### Soft vs hard float — always use the matching wrapper

The `newlib2` suite dlopens its `.so` from `/tmp`, and a fixture's float ABI MUST
match the firmware: a hard-float `.so` (VFP) run by a soft-float firmware (FPU
off) HardFaults (NOCP) on the first float call. `flash-run-sf.sh`/`flash-run-hf.sh`
re-stage the matching fixtures into `/tmp` every run and verify the ABI before
flashing. Soft firmware → `-sf`, hard firmware → `-hf`; never cross them.

### Expected on-target output

The apps report over semihosting and end in a `PASS` summary. Verified on a
WeAct BlackPill STM32F411CE:

- **`scl-fw-flatfs`** — formats the on-chip flat_fs, saves the 6 fixtures
  (`c1.so c2.so cpp3.so libcppeh.so ehx_catch.so notelf.bin`), runs the
  cross-module battery, ending:

  ```
  == BATTERY SUMMARY: 16/16 PASS ==
  battery: done
  ```

  Any `FAIL`, or a summary below `16/16`, is a regression.
- **`scl-fw-gotplt-demo`** — demonstrates the standard ARM GOT[2]/PLT0 layout:
  sets GOT[2] to `dl_runtime_resolve`, dumps PLT0 instructions, runs the GOT
  walkthrough (before/after), calls `cpp3_method(2,100)` twice. Ends with a PASS
  when the result is finite and consistent.
- **`scl-fw-dyn1` / `dyn2` / `scl-fw-ovl1`** — per-scenario `PASS` lines + a final
  summary; good when no `FAIL` appears.
- **`f411ce-sh`** (`scl::static`) — segment dumps, constructor prints, a GOT
  walkthrough, then the demo library calls (see `test-static/test-static/doc.md`).

## 6. The ET_REL module loader (`scl::reloc`) — `test-relocatable/`

A **separate**, self-contained non-PIC firmware project (its own `platform/reloc.ld`,
no `-pie`/GOT/`.dynsym`). It loads `-mlong-calls -fno-pic -r` **ET_REL** modules —
no GOT/PLT/r10 — and links cyclic module groups with `reloc::load_set`.

```sh
# build (app + ET_REL fixtures) and flash + run — one shot, from the repo root
test-relocatable/tools/flash_reloc_run.sh
```
Or manually:
```sh
cmake -S test-relocatable -B test-relocatable/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-relocatable/build -j          # -> test-relocatable/build/scl-fw-reloc.hex (+ fixtures)
ctest --test-dir test-relocatable/build          # flash + run on the board
```

**Expected on-target output** — a 9-check battery; verified on a WeAct BlackPill STM32F411CE:
```
== TEST 1: relprov + relmod (cross-module ABS32) ==       -> 3 PASS
== TEST 2: c1 + c2 + cpp3 (cyclic set, load_set) ==       -> 6 PASS
   c1_call_c2(5)=17467  c1_use_fm()=611  cpp3_recurse(6)=21  cpp3_method(2,100)=1432
==== BATTERY SUMMARY: 9 passed, 0 failed ====   RESULT: ALL PASS
```
Those values are **bit-identical** to the ET_DYN `dyn2` battery — eager ET_REL
linking behaves exactly like lazy GOT/PLT. See [build-linking-guide.md](build-linking-guide.md) §3.5 and
[dynamic-vs-static-linker-explained.md](dynamic-vs-static-linker-explained.md) §12.

## Tips

- After the first configure, just rerun the matching `cmake --build … -j`.
- Build one app instead of all: `cmake --build test-dynamic/build --target scl-fw-dyn1`.
- Everything (except the parser submodule) is self-contained; no sibling folder
  is needed.

This is the exact sequence that produces **6/6** host, **5/5** freestanding,
**2/2 × SF/HF** hosted newlib, the `scl::static` `f411ce-sh.elf`, the
`scl::reloc` **9/9** battery, and **8 XIP** test targets. Each module and app is
explained in `README.md` (§2, §4) and [scl-guide.md](scl-guide.md).

## 7. XIP (Execute-In-Place) tests — `test-xip/`

The XIP test tree exercises all three XIP adapter variants. Code executes
directly from flash; only RW data is copied to RAM.

```sh
# build all XIP targets (individual targets via --target)
cmake -S test-xip -B test-xip/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-xip/build --target scl-fw-xip-nolib1 -j
```

Or use the runner script:
```sh
./test-xip/build-and-run.sh nolib1             # freestanding XIP
./test-xip/build-and-run.sh newlib1            # hosted XIP
./test-xip/build-and-run.sh static             # static XIP (semihosting)
./test-xip/build-and-run.sh static-direct      # direct API calls XIP
./test-xip/build-and-run.sh interrupt          # XIP + SysTick interrupt
./test-xip/build-and-run.sh interrupt-static   # XIP + TIM2 interrupt + static
./test-xip/build-and-run.sh interrupt-static-direct  # direct calls + interrupts
./test-xip/build-and-run.sh static2            # SPI flash storage + XIP
```

| Target | App | Loader | Description |
|--------|-----|--------|-------------|
| `nolib1` | `scl-fw-xip-nolib1` | `scl-xip-dynamic-nolib` | Freestanding dlopen + XIP |
| `newlib1` | `scl-fw-xip-newlib1` | `scl-xip-dynamic-newlib` | Hosted dlopen + XIP |
| `static` | `scl-fw-xip-static-semihosting` | `scl-xip-static` | Static loader + XIP |
| `static-direct` | `scl-fw-xip-static-direct` | `scl-xip-common` | Direct API calls + XIP |
| `interrupt` | `scl-fw-xip-interrupt1` | `scl-xip-dynamic-nolib` | XIP + SysTick interrupt trampoline |
| `interrupt-static` | `scl-fw-xip-interrupt-static` | `scl-xip-static` | XIP + TIM2 interrupt + static loader |
| `interrupt-static-direct` | `scl-fw-xip-interrupt-static-direct` | `scl-xip-common` | XIP + direct calls + interrupts |
| `static2` | `scl-fw-xip-static-spi-flash` | `scl-xip-static` | SPI flash → internal flash → XIP |

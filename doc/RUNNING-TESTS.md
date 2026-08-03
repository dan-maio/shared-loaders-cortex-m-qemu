# Building and Running the Tests

Every test in this workspace is Cortex-M4 **firmware**: it is built with the ARM
cross toolchain, flashed to a **WeAct BlackPill STM32F411CE** over CMSIS-DAP, and
reports back over the semihosting console.

Run everything from the repo root unless a section says otherwise.

---

## 1. Prerequisites

```sh
export PATH="/path/to/arm-none-eabi/bin:$PATH"     # or set ARM_TOOLCHAIN_BIN
```

Paths to the toolchain and OpenOCD live in `tools-config.sh`, which every script
sources. It prefers whatever is on `PATH` and falls back to a hard-coded xPack
location, so overriding `PATH`, `ARM_TOOLCHAIN_BIN`, `OPENOCD` or
`OPENOCD_SCRIPTS` is enough — no file needs editing.

**The `common-elf-reader` submodule must be initialised**, or nothing builds:

```sh
git -c protocol.file.allow=always submodule update --init
```

The `-c protocol.file.allow=always` is required because the submodule URL is
relative and the remote is a local path; git blocks `file://` submodule transport
by default. Cloning fresh, do it in one step:

```sh
git -c protocol.file.allow=always clone --recurse-submodules <url> <dir>
```

OpenOCD 0.12.0 or newer, board attached over CMSIS-DAP.

---

## 2. The five test trees

Tests are grouped by **loading technique**, one top-level tree each. `test/` is
not a test tree — it holds the shared infrastructure (board platforms, built
fixtures, flash tooling, cmake helpers). See `doc/test-taxonomy.md` for the
naming rules.

| Tree | Technique |
|---|---|
| `test-static/` | resolve once at load, patch the GOT (`scl::static`) |
| `test-dynamic/` | `dlopen`/`dlsym`, optional lazy PLT (`scl::dynamic-nolib`, `-newlib`) |
| `test-xip/` | execute in place from flash, only RW copied to RAM |
| `test-overlay/` | time-multiplex one RAM window (`scl::overlay`) |
| `test-relocatable/` | `ET_REL` `.rel.o` modules (`scl::reloc`) |

---

## 3. Running a test

Each tree has a `build-and-run.sh` that configures, builds, flashes and runs one
app. **Target names are the subdirectory names with the `test-` prefix dropped,
so the option is the path to the test.** Run any of them with no arguments to
list its targets and the directory each maps to.

```sh
test-xip/build-and-run.sh          static/spi-flash
test-dynamic/build-and-run.sh      dynamic-nolib/test1
test-static/build-and-run.sh       drv
test-overlay/build-and-run.sh      test1
test-relocatable/build-and-run.sh  reloc
```

### `test-xip/` — 8 targets

| Target | Directory |
|---|---|
| `static/semihosting` | `test-xip-static/semihosting` |
| `static/spi-flash` | `test-xip-static/spi-flash` |
| `static-direct` | `test-xip-static-direct` |
| `interrupt-static` | `test-xip-interrupt-static` |
| `interrupt-static-direct` | `test-xip-interrupt-static-direct` |
| `dynamic-nolib` | `test-xip-dynamic-nolib/test1` |
| `dynamic-newlib` | `test-xip-dynamic-newlib/test1` |
| `interrupt` | `test-xip-interrupt/test1` |

`static/spi-flash` builds standalone (it is a hosted app with its own platform
flags) — the runner delegates to `test-xip/test-xip-static/spi-flash/build-and-run.sh`.
`dynamic-newlib` switches to the hosted toolchain automatically.

### `test-dynamic/` — 8 targets

| Target | Directory |
|---|---|
| `dynamic-nolib/test1` … `test4` | `test-dynamic-nolib/testN` |
| `dynamic-newlib/test1`, `test2` | `test-dynamic-newlib/testN` |
| `flatfs-spi` | `test-flatfs-spi` |
| `flatfs-bitbang-spi` | `test-flatfs-bitbang-spi` |

The newlib apps build once per float ABI: `SCL_ABI=sf` (default) or `SCL_ABI=hf`.

### `test-static/` — 4 targets

`static`, `static-cpp`, `static-direct`, `drv`. Each carries its own platform
tree and toolchain file, so the runner forwards to the project's own script.

### `test-overlay/` — `test1`

### `test-relocatable/` — 4 targets

`reloc`, `classic`, `classic-spi`, `interrupt`. This tree is a single project
with several apps and no test subdirectories, so its options name the apps.

---

## 4. Building without flashing

Useful for a compile check, or when no board is attached.

```sh
# whole tree
cmake -S test-dynamic -B test-dynamic/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-dynamic/build -j

# one test, standalone
cd test-xip/test-xip-static/spi-flash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j
```

Toolchain files, both in `cmake/`:

- `arm-none-eabi.cmake` — freestanding, hard-float
- `arm-none-eabi-hosted.cmake` — hosted newlib; add `-DSCL_HF=ON` for hard-float

Two caveats worth knowing:

**`cmake --build` on the whole `test-xip` tree fails.** `test-xip-dynamic-newlib`
needs the hosted toolchain while its siblings need the freestanding one, and the
aggregate lists all of them. Build individual targets with `--target`, or use the
runner, which selects the toolchain per target.

**A green build proves less than it looks.** It does not exercise the runner
scripts, the fixture scripts, or anything path-related at run time. Several bugs
have hidden exactly there. To check a runner without a board:

```sh
OPENOCD=/bin/false test-xip/build-and-run.sh static/spi-flash
```

That runs everything up to the flash step.

---

## 5. Fixtures

The `.so` and `.rel.o` modules the tests load are built by their own scripts, not
by cmake. The runners call them, so normally you never do — but if you change a
fixture source:

| Script | Builds | Into |
|---|---|---|
| `test-xip/fixtures-src/build-fixtures.sh` | baseline PIC fixtures | `test-xip/fixtures/` |
| `test-xip/fixtures-src/build-xip-libs.sh` | same, XIP split segments | `test-xip/fixtures/` |
| `test-xip/test-xip-static/semihosting/fixtures-src/build.sh` | slib/mylib1/mylib2 | `test-xip/fixtures/` |
| `test-xip/test-xip-static/spi-flash/fixtures-src/build-xip-drv.sh` | spi/uart/led/cpp_features | `fixtures/` (repo root) |
| `test-relocatable/fixtures-reloc/Makefile` | `.rel.o` modules | `test-relocatable/fixtures-reloc/build/` |

---

## 6. Cleaning

```sh
./cleaner.sh
```

Removes every build tree: the shared cmake trees, the per-test `build/`
directories, and the fixture builds.

Run it if cmake reports *"the source does not match the source used to generate
cache"* — that means a stale `CMakeCache.txt` from a different copy of the tree,
and no amount of rebuilding fixes it.

---

## 7. Adding a new test

This is the checklist. For the reasoning behind each choice — board startup,
delivery, loader, fixture flags, XIP vs RAM, call style, constructors and
variables — see **`doc/creating-a-test.md`**.

1. **Pick the technique.** That fixes the top-level tree, from the five in §2. If
   none fits, you are adding a loading technique, not a test.
2. **Name the directory** after what it exercises, with the `test-` prefix:
   `test-static/test-static-interrupt`. Level-2 names are relaxed — the
   peripheral, the storage, the module set all qualify. See
   `doc/test-taxonomy.md`.
3. **If it matches an existing test on everything that matters**, it is a
   *scenario*, not a test: nest it beside that one and name it after what varies
   (`semihosting`, `spi-flash`), not by number.
4. **Add it to the tree's `CMakeLists.txt`** with `add_subdirectory`, unless it
   is a standalone hosted app like `spi-flash`.
5. **Add a row to the tree's `build-and-run.sh` `TARGETS` table**:
   `[option]="cmake-target:subdirectory"`. The option is the subdirectory path
   without the `test-` prefix. The script checks the directory exists before
   using it, and `--help` prints the mapping.
6. **Add it to `cleaner.sh`** so its `build/` is cleaned.
7. **Verify standalone**, not just from the aggregate — the aggregate supplies
   includes that a standalone configure does not:
   ```sh
   cd <new test> && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=... && cmake --build build -j
   ```

### Paths in a new CMakeLists

Getting these wrong is the single most common breakage, and builds do not always
catch it. Reach the shared infrastructure through `test/`, never by walking up
into a sibling tree:

```cmake
get_filename_component(_repo ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)   # adjust depth
set(_su ${_repo}/test/platforms/weact-f411ce-hosted/vendor)
include(${_repo}/test/cmake/scl-test-app.cmake)
```

Pass linker scripts as `-T${DIR}/script.ld` — **one argument, no space**. The
`"SHELL:-T ${DIR}/script.ld"` form word-splits and breaks if the repo path
contains a space.

---

## 8. Script reference

| Script | Does |
|---|---|
| `<tree>/build-and-run.sh` | configure + build + flash + run one target |
| `test/tools/flash-run.sh` | flash and run an already-built app by name; searches the repo for the newest matching `.hex` |
| `test/tools/flash-run-sf.sh` / `-hf.sh` | set the newlib ABI variant, stage fixtures, delegate to `flash-run.sh` |
| `test-relocatable/tools/flash_reloc_run.sh` | one-step build + flash + run for the ET_REL apps |
| `cleaner.sh` | remove all build trees |

`flash-run.sh` runs until Ctrl-C. The `build-and-run.sh` runners are
non-interactive: OpenOCD runs the firmware for `RUN_MS` (default 5 min; the
`spi-flash` scenario uses 20 min because provisioning over semihosting and the
SPI writes are slow), then shuts down. Override it:

```sh
RUN_MS=120000 test-xip/build-and-run.sh static/spi-flash
```

Their console output is teed to `run.log` in the build tree.

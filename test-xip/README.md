# test-xip — XIP firmware test tree

Firmware apps that link the **`scl::xip-*`** loaders: a module's R-X segment
executes in place from flash and only its RW segment is copied to RAM (two-base
model). See `doc/xip-flash-execution-analysis.md` (§Implementation) for the full
causes-and-fixes writeup.

## Status

`test-dynamic-nolib/test1` is **implemented and hardware-verified** (WeAct BlackPill STM32F411CE).
It provisions the on-chip flat-fs from semihosting, then XIP-loads **all ten fixtures**
(`mylib1 mylib2 c1 c2 cpp3 libprov libcons libcppeh libcppsh ehx_catch`) and exercises
their surface **from flash** — every function, global, constructor effect, string
literal, struct member, function-pointer path, weak symbol, cross-library call
(`c1→c2`, `cpp3↔c2`), C++ RTTI/`dynamic_cast`, virtual objects, and calls back into
`main` (including `printf`). Every library reports `loaded (XIP)` with code at
`0x0804xxxx`; only the RW segment lands in RAM. `test-dynamic-newlib` and `test-static`
are also implemented (see `build_and_run.sh newlib1` / `static`). See `comparison.md`
for per-library sizes and RAM savings (~15 KB).

## How it runs

Run the main test script with one of the available target options (`nolib1`, `interrupt`, `newlib1`, `static`, `static-direct`, `interrupt-static`, `interrupt-static-direct`, or `static2`):
```bash
./test-xip/build_and_run.sh <target-variant>
```

`build_and_run.sh` runs `fixtures-src/build-fixtures.sh` (builds `mylib1/2`), configures
CMake, then runs `fixtures-src/build-xip-libs.sh` **after** configure so its
single-pic-base rebuilds of the other eight fixtures overwrite the baseline `-fPIC`
copies CMake staged; it then builds the hosted firmware, flashes it, and tees the
semihosting console to `test-xip/build/run.log`. (The fixtures are build output,
git-ignored.) Give it a longer window for the full surface: `RUN_MS=20000
test-xip/build_and_run.sh nolib1` — the default 10 s can truncate before the last group.

## Key pieces

| path | role |
| --- | --- |
| `common/src/test_c1_c2_cpp3.cpp` | **Shared** cross-lib C/C++ group test (parameterized by `XIP_NS_ALIAS`). Included by both `dynamic-nolib` and `dynamic-newlib` test wrappers. |
| `common/src/test_ehx_cons_ppeh_ppsh_prov.cpp` | **Shared** prov→cons + EH/RTTI test (also parameterized). |
| `common/src/walkthrough.cpp` | **Shared** XIP didactic walkthrough (same pattern). |
| `common/src/walkthrough_complex.cpp` | **Shared** complex GOT/PLT walkthrough (same pattern). |
| `test-dynamic-nolib/test1/src/main.cpp` | nolib variant: provisions all ten fixtures; runs `mylib1/2` full-surface |
| `test-xip-interrupt/test1/src/main.cpp` | interrupt variant: same full-surface dynamic tests but installs a SysTick timer exception trampoline to blink PA5 using `mylib1.so`'s handler callback |
| `test-dynamic-nolib/test1/src/test_c1_c2_cpp3.cpp` | **Wrapper** — includes `../../common/src/test_c1_c2_cpp3.cpp` with `xip_ns = xip::nolib` |
| `test-dynamic-newlib/test1/src/main.cpp` | newlib variant (same structure, links `scl::xip-dynamic-newlib`) |
| `test-dynamic-newlib/test1/src/test_c1_c2_cpp3.cpp` | **Wrapper** — includes shared test with `xip_ns = xip::newlib` |
| All variants `include/test-helpers.hpp` | shared `xopen`/`C0..C2`/`RD`/`CSTR` call+read shims (enter each lib with r10 = got_base) |
| All variants `CMakeLists.txt` | builds on the **newlib-hosted** platform (`../test/platforms/board-hosted`, board = `-DSCL_BOARD=nucleo|weact`): runs `main`'s constructors, real heap, libc/libm/libstdc++, `-pie -Wl,-E`, register-dumping fault handlers |
| All variants `sections-flatfs-hosted.ld` | reviewed linker script: heap placed after the 64 KB image arena; flat-fs in flash sectors 6–7 |
| All variants `semihost-file.cpp` | direct-BKPT semihosting file reader (newlib stdio is unusable under `-pie`) |
| `fixtures-src/mylib1.c`, `mylib2.c` | XIP variants of the shared C fixtures (constructor `printf` removed — a PLT call in a constructor runs before r10 routing is set) |
| `fixtures-src/ldscripts/xip-split.ld` | split `R-X | RW` linker script with **VA == file offset** |
| `fixtures-src/build-fixtures.sh` | builds `mylib1/2` split: `-msingle-pic-base -mno-pic-data-is-text-relative -Wl,-Bsymbolic` |
| `fixtures-src/build-xip-libs.sh` | rebuilds the eight shared C/C++ fixtures single-pic-base + split (run after CMake configure) so they XIP too |
| `tools/flash-run.sh` | flash + run one already-built app |

## What XIPs, and what does not

A library XIPs only if it uses a **single-pic-base** (r10-relative) GOT. The shared
C/C++ fixtures ship `-fPIC` (PC-relative GOT), so `fixtures-src/build-xip-libs.sh`
**rebuilds** them single-pic-base + split (`build-fixtures.sh` builds `mylib1/2`); with
that, all ten XIP. Any genuinely fused-`RWX` or VA-mismatched image still loads
**copy-whole** through the stock loader — the adapter falls back automatically
(`segment-map` requires two PT_LOAD segments with `VA == file_offset`).

**One limitation:** a C++ exception whose unwind must pass *through* the cross-call
trampoline cannot be caught — the trampoline's `.ARM.exidx` is CANTUNWIND (model 0, no
`.personality`). RTTI/`dynamic_cast` and non-crossing EH work; the cross-boundary catch
case (`ehx_catch`) is skipped. See `comparison.md`.

The heavy shared dirs (`cmake/`, `common/`, `platforms/`) are reused from `../test`;
the flat-fs backend (`fs-archive.cpp`, `flash-prog.c`) is reused in place from
`../test/.../test3`.

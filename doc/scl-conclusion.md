# SCL — Analysis and Conclusion

An assessment of the Shared Code Loader codebase: what it is, what its
architecture gets right, where its structure works against it, and what to do
next. Complements the descriptive docs (`scl-codebase-analysis.md`,
`manual-scl-loaders.md`, `Full-Manual-SCL.md`) — this one draws conclusions
rather than describing mechanics.

**Scope.** `src/` (4,962 lines), `include/` (1,714), `common-elf-reader`
(654), across 19 on-target test projects. Target is a
WeAct BlackPill STM32F411CE (Cortex-M4, no MMU).

---

## 1. What this codebase is

SCL is not one loader. It is **five different answers** to "run code that
wasn't linked into the firmware," sharing one ELF-reading core:

| Technique | Consumes | Where code runs | Key mechanism |
|---|---|---|---|
| **static** (`src/static/`) | `ET_DYN` `-fPIC` | RAM | link-time patching against main's `.dynsym` |
| **dynamic-nolib** (`src/dynamic-nolib/`) | `ET_DYN` | RAM | `dlopen`/`dlsym`, lazy PLT, no heap |
| **dynamic-newlib** (`src/dynamic-newlib/`) | `ET_DYN` | RAM | full relocation + patch journal, newlib heap |
| **reloc** (`src/reloc/`) | `ET_REL` `-mlong-calls -fno-pic` | RAM | `R_ARM_ABS32` literals, no GOT/PLT |
| **overlay** (`src/overlay/`) | link-time bound | fixed RAM window | `memcpy` LMA→VMA + DSB/ISB |

**XIP** (`src/xip/`) is orthogonal — a modifier that keeps the R-X segment in
flash and copies only RW to RAM. It is applied *per loader* through adapters.

This breadth is the codebase's real value. Most embedded projects pick one
strategy; this one implements five against identical fixtures on identical
hardware, which makes the trade-offs directly observable rather than theoretical.

---

## 2. What the architecture gets right

**The `byte_source` seam is the best decision in the codebase.**
`common-elf-reader/include/loader/byte-source.hpp` reduces all I/O to
`read_at(offset, span)`. Semihosting, a plain memory buffer, SPI flash and
FlatFS in `test-drv` — all behind one two-method interface. The core
never assumes the object is resident, which is what makes loading directly from
SPI flash possible without a special case.

**Pure computation is genuinely separated from mutation.** `relocator` computes
a plan and writes nothing; `reloc_applier` applies without allocating;
`classify_segments` and `text_is_immutable` decide without touching memory. This
keeps the tricky arithmetic in functions that can be reasoned about in
isolation, independent of any load actually being in flight.

**Freestanding discipline is real, not aspirational.** `symbol_source` is a
function pointer plus context rather than `std::function`. `region_allocator` is
a fixed 16-block free list with no dynamic allocation. `loader::result` is a
hand-rolled C++20 stand-in for `std::expected` so C++23 isn't required. The
constraints were chosen deliberately and held.

**Documentation quality is unusually high.** Nearly every header explains *why*,
not just *what*. `segment-map.hpp` documents the subtle case where the linker
packs both segments into one page and RW VAs land numerically inside the text
range — with the reasoning for why `in_data` must win. That comment is worth more
than the code it describes.

---

## 3. Structural findings

### 3.1 The static XIP loader — RESOLVED

**Resolution:** `src/xip/static/adapter.cpp` is now a **complete 407-line implementation** providing `bind_host_imports()`, `load()`, `resolve()`, `unload()`, and `xip_cross_lookup()`. It is compiled as part of the `scl-xip-static` library target. The recommendation below has been carried out.

*Historical note:* When this analysis was first written, `src/xip/static/adapter.cpp` was an 8-line TODO stub and the implementation existed only as five diverged test-local copies. The library implementation was subsequently promoted and the copies removed.

For comparison, the sibling adapters are: `dynamic-nolib/adapter.cpp` is 391 lines,
`dynamic-newlib/adapter.cpp` is 436 lines.

The forks exist for a real reason: the two-stage SPI→internal-flash path in
`static2` genuinely differs from the others. But that argues for
parameterising one implementation, not for copying it.

### 3.2 Two build systems, silently drifting

**32 directories carry both a `CMakeLists.txt` and a `Makefile`** describing the
same target. Nothing checks that they agree, and they don't.

`test-xip-interrupt-static-direct` is the demonstration. Its Makefile was a
verbatim copy of the non-direct test's, never adapted:

- missing the `.so` files on the link line, so `s_info`/`s_lib` were undefined —
  the "direct" variant names library globals rather than resolving them, so the
  `.so` must be linked;
- missing `--unresolved-symbols=ignore-in-shared-libs`, which `CMakeLists.txt:130`
  has;
- emitting `scl-fw-xip-interrupt-static.hex` while `build-and-run-make.sh:52`
  looks for `...-direct.hex` — so the make path could not have worked even once
  it linked.

The CMake path built correctly the whole time. Nobody noticed because nobody ran
the make path, which is the point: an unexercised second build system is not
redundancy, it is a second thing to be wrong.

Notably, the two newest tests (`test-xip-static-direct`,
`test-xip-static/spi-flash`) had *only* CMakeLists — the drift was already being
resolved by attrition rather than decision.

**Resolved.** The 24 duplicate Makefiles have been deleted, along with the five
`build-and-run-make.sh` runners and the two make-based `build-and-run.sh`
variants that drove them. The five remaining Makefiles are not duplicates: they
are the sole build for fixture and vendor-library trees, and they stay. A third
Makefile turned out to be dead on arrival — `test-xip-interrupt/test1` failed on
a missing `test-helpers.hpp`, before any change in this branch.

### 3.3 Verification is on-hardware only, by decision

The project previously carried a `doctest`-based host unit suite over the pure
layer. It has been removed deliberately: the tests that matter here are the ones
that run the firmware on the board, and a second suite that passed while the
firmware was broken was worth less than the maintenance it cost.

Two observations from that suite are worth recording, because they justify the
removal rather than arguing against it.

`test-xip/host-unit/xip1` was **never built by anything**. No `CMakeLists.txt`
referenced it. Its single test file was a TODO stub whose `main()` returned 0
without asserting anything, so XIP had no host coverage at all — only the
appearance of it. A test that cannot fail is worse than a missing test, because
it is counted.

The remaining suites did cover real ground (`byte_source`, `elf_image`,
`symbol_resolver`, `relocator`, `reloc_applier`, `region_allocator`, overlay),
but none of the defects found in practice were of a kind they could catch. The
PLT fault, the missing prelink call, the unlinked `.so`, the absent exports —
every one of them lived in the wiring between components, not inside a component.

The consequence to accept honestly: **`xip::apply_rel_xip` is now verified only by
running firmware.** It decides, per relocation, whether an addend biases against
`text_base` or `data_base` — the entire two-base model in one switch statement,
and the place split-base bugs concentrate. Nothing checks it below the level of a
flashed board, so changes there deserve proportionate care on hardware.

### 3.4 The host-export contract is implicit

Modules import symbols from main, resolved against main's `.dynsym` via
`link_context::from_self()`. But `-Wl,-E` only exports symbols that were
*actually linked in*, and libc functions come from archives — pulled only when
referenced. So a module importing `printf` fails unless main happens to
reference `printf` itself.

Nothing enforces or checks this. It is handled by convention: a
`g_link_anchor[]` array of `used, retain` function addresses, documented in
`test-xip-dynamic-nolib/test1/src/main.cpp:49` as replacing "the old pile of `-u`
flags." The convention is good. That it is a convention, discovered per test at
link or run time, is the weakness — `static2` was missing `printf` and
`puts` and only surfaced it as a HardFault in a constructor.

---

## 4. The deep technical conclusion: XIP's real constraint

The most valuable finding is a general rule the codebase now demonstrates
concretely:

> Under XIP, **any PC-relative reference that crosses from the flash-resident R-X
> segment into the RAM-resident RW segment is broken by construction**, because
> the loader gives the two segments independent bases.

The PLT is the instance the compiler generates unprompted. A PLT stub reaches its
GOT slot with `add ip, pc`; executing from flash, that lands on the stale flash
image of the GOT rather than the relocated RAM copy. No load-time fixup can
repair it, because the stub lives in the immutable R-X segment — and on ARM no
compiler or linker flag suppresses PLT generation (`-fno-plt` is silently
ignored, `--no-plt` unsupported).

The resolution generalises well: bind anything of that shape **before the image
is committed to flash**. This is legal because the relocation kinds differ in
nature — `JUMP_SLOT`/`GLOB_DAT` targets are host addresses, fixed at main's link
time and independent of where the module lands, so they can be baked into flash;
`R_ARM_RELATIVE` depends on the runtime bases and must stay in the RAM copy. The
existing `text_is_immutable()` scan already encodes half this insight by
rejecting relocations that target R-X. The other half — that PC-relative *reads*
across the boundary are equally fatal, and are invisible to a relocation scan —
is what this session established.

Full derivation with register-level evidence: `doc/xip-plt-prelink.md`.

---

## 5. Recommendations, in priority order

**1. Promote `xip::stat` into the library.** Implement
`src/xip/static/adapter.cpp` from the most advanced fork (variant C, which has
the prelink verification), parameterise the two-stage provisioning difference,
and delete the five test-local copies. This closes the defect class that produced
both of this session's bugs and is the single highest-value change available.

**2. Make the host-export contract explicit.** A single shared header declaring
the anchor set, included by each test's main, replaces per-test rediscovery.
Better still, a build-time check that every `JUMP_SLOT` import across the fixture
set appears in main's `.dynsym` — the same check the loader now performs at
runtime, moved to link time where it costs nothing.

**3. Deduplicate the shared test sources.** *Done.* Fifteen byte-identical pairs
remained; there are now none. The XIP battery files (`walkthrough.cpp`,
`walkthrough_complex.cpp`, `test_c1_c2_cpp3.cpp`,
`test_ehx_cons_ppeh_ppsh_prov.cpp`) are compiled by the interrupt variants from
the non-interrupt projects. The four board-independent platform files
(`crt.cpp`, `dl-semihost.cpp`, `scl-log-semihost.cpp`, `semihost-file.cpp`) moved
to `test/platforms/common/src/`, which both board platforms glob — the `.c` files
stay per-board because they genuinely differ. `led.cpp`, `timer.cpp` and
`cxx.cpp` are shared from `test-static` to `test-static-cpp`.

Note that `weact-f411ce` looked dead — nothing referenced it but its own
`platform-library.cmake` — yet it is a selectable board via `SCL_PLATFORM`.
Deleting it would have removed board support rather than removing duplication.

---

## 6. Verdict

The **core is genuinely good**. The `byte_source` abstraction, the
computation/mutation split, the freestanding discipline, and the documentation
quality are all above what this class of project usually achieves. The five-way
comparison of loading strategies on one target is unusual and valuable, and the
XIP work has produced a real, transferable result about PC-relative references
across a split address space.

The **weakness is entirely structural, not algorithmic**. Nothing here is wrong
because the ELF handling is wrong; things are wrong because the same logic exists
in three to five places and the copies drift. Every defect found in this session —
the ineffective linker-script edit, the missing prelink call, the broken
Makefile, the missing exports — traces to duplication rather than to a
misunderstanding of the problem.

That is the good kind of problem to have, because it is fixable by consolidation
rather than redesign. Recommendation 1 alone would eliminate most of it.

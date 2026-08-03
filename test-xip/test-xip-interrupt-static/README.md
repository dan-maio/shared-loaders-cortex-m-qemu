# test-xip/test-static — XIP with the *static* loader model

Mirrors `test-dynamic-nolib/test1` (full-surface, print-only, R-X from flash),
but with **no dynamic loader** — there is no `dlopen`/`dlsym` and no call-time
binding. The library is resolved once, at load, and its GOT is patched in place
(the classic static-linker model). Only the two-base XIP split is shared with the
nolib test: the R-X segment executes in place from flash, only the RW segment
(GOT/.data/.bss) is copied to RAM (r10 = got_base).

## Status — implemented and hardware-verified (WeAct BlackPill STM32F411CE)

`slib.so` loads XIP (`s_add @ 0x0804xxxx`, FLASH) and the whole surface runs from
flash: `.data`/`.bss` globals, the constructor effect, pure functions, intra-lib
calls (`-Bsymbolic`), string literals (`R_ARM_RELATIVE`), a struct global with
string members, a function-pointer path, a **data import** from main (`HOST_K`)
and **function imports** from main (`host_f`, `printf`).

## How it runs

```
test-xip/test-static/build_and_run.sh      # build fixture + firmware, flash, capture console
```

## How the static model differs from the nolib (dynamic) one

| step | this static test |
| --- | --- |
| resolution | once, at load — imports are resolved against **main's own `.dynsym`** via `fw::link_context::from_self()` (main is `-pie -Wl,-E`). No `dlopen`/`dlsym` service, **no hand-maintained host table, and no `-u` link anchors** |
| data import (`HOST_K`, GLOB_DAT) | written into the **RAM GOT** at load (`apply_rel_xip`, resolver = main's `.dynsym`) |
| function import (`host_f`/`printf`, JUMP_SLOT) | libraries import only from **main**, which is r10-free, so the flash `.got.plt` slots are bound **directly to main's resolved addresses at provision** (`bind_host_imports`) — **no cross-call trampoline** |
| calling in | `pic_call_*` sets r10 = got_base before branching to the flash code |

The only residual coupling is a `[[gnu::used, gnu::retain]]` **linkage anchor** in
`main.cpp`: symbols the libraries borrow but main never calls itself (`printf`,
`operator new`, the C++ EH runtime, `sinf/…`) come from `libc.a`/`libsupc++.a` and
must be *referenced* to be linked in before `-E` can export them. The anchor is not
a resolver — the loader resolves purely via `.dynsym`. See `../../doc/xip-memory-analysis.md` §10.4.

Because everything is resolved at load, this is closer to static linking than the
nolib loader's per-call trampoline. (A library that imported from *another* loaded
library would need the r10-swapping trampoline; `slib` does not.)

## Key pieces (all under `test-static/`, per the loader-tree rule)

| path | role |
| --- | --- |
| `src/xip-static-loader.{hpp,cpp}` | the static XIP loader: `bind_host_imports` (provision), `load` (R-X flash + RW→RAM + split relocs + ctors), `resolve`, `unload`. Built on `scl::xip-common` (classify/relocate) + the common-elf-reader parser — **no dynamic loader** |
| `src/main.cpp` | full-surface print-only test; defines `HOST_K`/`host_f`/`AA`/… (exported via `-E`) plus the borrowed-symbol linkage anchor |
| `fixtures-src/slib.c` + `build.sh` | the single-pic-base split fixture and its build (`-msingle-pic-base -mno-pic-data-is-text-relative -Wl,-Bsymbolic`, `xip-split.ld`) |
| `src/sections-flatfs-hosted.ld`, `src/semihost-file.cpp`, `include/abort_fix.h` | reused hosted-platform pieces (flat-fs in flash sectors 6–7, semihosting file reader) |

`pic-trampoline.c` (r10 setup), `image-memory.cpp` (arena) and the flat-fs backend
(`fs-archive.cpp`/`flash-prog.c`) are compiled in directly from the repo — the
dynamic loader is never linked.

# XIP and the PLT — why `cpp_features.so` HardFaulted, and how it was fixed

**Test:** `test-xip/test-xip-static/spi-flash`
**Commits:** `9119755`, `6900470`

## Overview

`test-xip-static/spi-flash` composes two earlier tests: the SPI-flash + FlatFS
provisioning from `test-drv`, and true XIP execution from internal flash from
`test-xip-static-direct`. Semihosting brings the `.so` files in, they are staged
to SPI flash, copied into a FlatFS on internal flash, and then executed in place
from there.

`uart.so` and `led.so` ran correctly. `cpp_features.so` HardFaulted every time,
before any of its exported functions were ever called.

---

## Symptom

From `build/run.log`:

```
[xip] ==== C++ features test ====
  [xip] load_buf: raw=0x8041300 sz=2024
  [xip] load_buf: OK text_base=0x8041374 data_base=0x20009470 got_base=0x200094e8
  [xip] load_buf: raw=0x80422b4 sz=8528
[HardFault]
 R2  = 08042DFD
 R3  = 080431AC
 R12 = 08043428
 LR  = 08042E0D
 PC  = 00000FD6
 CFSR = 00000400
```

Note that `load_buf: OK` is never printed for the second module. The fault
happened *inside* `xip::stat::load()` — specifically in the `.init_array`
constructor the loader runs at the end of loading.

---

## Root cause

The registers identify the failure exactly.

`LR = 0x08042E0D`. The blob is at `0x080422b4`, and for this link `text_off ==
text_vaddr`, so a runtime address maps back to a link-time VA by simple
subtraction: `0x08042E0C - 0x080422B4 = 0xB58`. Disassembly:

```
00000b48 <_Z8cpp_ctorv>:
 b4c:  ldr    r3, [pc, #24]
 b4e:  ldr.w  r3, [sl, r3]     ; r10-based GOT load — this part is fine
 b52:  mov    r0, r3
 b54:  bl     ff8 <.plt+0x30>  ; puts        <-- returns to 0xb58
 b60:  bl     1018 <.plt+0x50> ; uart_puts
```

So the global constructor called `puts` through `.plt+0x30`. That stub is:

```
 ff8:  movw   ip, #0x170
 ffc:  movt   ip, #0
1000:  add    ip, pc           ; ip = pc + 0x170
1002:  ldr.w  pc, [ip]
```

`add ip, pc` — the stub reaches its GOT slot **PC-relatively**. With
`text_base = 0x8042328`, the stub sits at `0x080432AC`, so `ip` becomes
`0x080432B8 + 0x170 = 0x08043428`, which is exactly `R12` in the fault frame.

And `0x08043428 = blob + 0x1174`, where `0x1174` is the file offset of the GOT
slot for `puts`.

**That is the flash image of the GOT, not the RAM copy.** The loader relocates
the RW segment into RAM at `0x200094e8` and patches the GOT there correctly, but
a PLT stub executing from flash can never reach it. The flash word still held
the linker's lazy-binding prefill:

```
$ od -A x -t x4 -j 0x116c -N 0x18 fixtures/libcpp_features.so
00116c 00000fc9 00000fc9 00000fc9 00000fc9
00117c 00000fc9 00000fc9
```

`0x00000fc9` is `.plt+0` with the Thumb bit. On the STM32F411 address
`0x00000000` aliases to flash `0x08000000` in the default boot configuration, so
`ldr pc, [ip]` branched into the middle of *main's own* firmware and executed
garbage until it faulted at `PC = 0x00000FD6`.

Only `cpp_features.so` is affected — it is the only fixture with a `.plt`:

| module              | `.plt`   | JUMP_SLOT imports                                |
|---------------------|----------|--------------------------------------------------|
| `libcpp_features.so`| 0x70     | printf, snprintf, puts, malloc, uart_puts, free  |
| `libuart.so`        | none     | —                                                |
| `libled.so`         | none     | —                                                |
| `libspi.so`         | none     | —                                                |

---

## Two hypotheses that were wrong

**Moving `.plt` into the RW segment** (commit `a406593`) cannot work. The `bl`
into the PLT encodes a link-time relative offset, so at runtime it still targets
`blob + plt_vaddr` in flash and lands on the same stub. Even if the branch were
somehow redirected, flash and RAM are `0x18000000` apart on this part — far
beyond the ±16 MB range of a Thumb `BL`.

That commit also edited `test-xip/fixtures-src/ldscripts/xip-split.ld`, which
this test does not use. It builds from its own copy under
`test-xip-static/spi-flash/fixtures-src/ldscripts/`. The change was a no-op.

**Suppressing the PLT at build time** is not possible on ARM. `-fno-plt` was
already present in `build-xip-drv.sh` and is silently ignored; `-Bsymbolic`,
`-z now`, and `--no-plt` (unsupported, removed in `9521a91`) all leave the `.plt`
in place. Verified empirically against arm-none-eabi-gcc 15.2.1.

`-fno-plt` has since been dropped from `build-xip-drv.sh`, as it only implied a
guarantee it never provided. Rebuilding without it produces byte-identical
fixtures.

---

## Solution: prelink the host imports into flash

The key observation is that the two relocation kinds have different natures:

- `R_ARM_JUMP_SLOT` and `R_ARM_GLOB_DAT` resolve to **host** addresses. Main is
  statically linked, so these are fixed at main's link time and independent of
  where the module later lands. They can be baked into flash.
- `R_ARM_RELATIVE` depends on the runtime `text_base` / `data_base`, which differ
  per load. These must stay in the RAM GOT.

A PLT stub only ever reads the first kind. So binding those slots before the blob
is programmed makes the flash-resident GOT correct, and the RAM GOT continues to
be relocated per-load exactly as before. The two copies agree.

**This keeps the module fully XIP.** The text still executes in place from flash
and is never copied to RAM. Only six words in the module's GOT changed.

`xip::stat::bind_host_imports()` already implemented precisely this pass, and
every sibling test calls it (`test-xip-static/semihosting/src/main.cpp:109`,
`test-xip-static-direct/src/main.cpp:70`, and both interrupt variants). This test
defined the function and never called it. The fix is one call in
`copy_spi_to_internal()`, while the blob is still staged in `g_img`:

```cpp
spi_flash_drv_read(e->off, g_img, (uint16_t)e->size);
int bound = xip::stat::bind_host_imports({(std::byte*)g_img, e->size});
trace_printf("  [copy] %s: prelinked %d host imports\n", name, bound);
fw_int::fs_add(name, g_img, e->size);
```

`fs_add()` computes the CRC over the buffer it is handed, so the patch is covered
by the stored checksum.

### Why write time and not load time

The flash words are already programmed to `0x00000fc9`. NOR flash can only clear
bits without an erase, and retargeting them to `0x0800xxxx` sets bits (`0x08000000`
has bit 27 set, the prefill does not). That would require erasing the sector — and
the FlatFS occupies only two sectors holding all four modules, so a load-time fixup
would mean erasing and rewriting the whole filesystem on every boot. At provisioning
the blob is still in RAM, so the patch is free.

---

## A second gap: missing exports

Prelinking initially bound only 4 of the 6 imports. `printf` and `puts` were
absent from main's `.dynsym`:

```
printf       *** NOT IN .dynsym ***
snprintf     08013a65 FUNC
puts         *** NOT IN .dynsym ***
malloc       08013335 FUNC
free         08013345 FUNC
uart_puts    0800059d FUNC
```

Main's exports come solely from `LINKER:-E` (`--export-dynamic`), which only
exports symbols that were actually linked in. Nothing in main referenced `printf`
or `puts`, so newlib never pulled those objects in and there was nothing to
export. `puts` is exactly what the crashing constructor called.

The fix is the same `g_link_anchor` pattern the other tests already use (see
`test-xip-dynamic-nolib/test1/src/main.cpp:62`) — an array of function addresses
marked `used, retain`, which forces those archive members into the link so `-E`
has something to export:

```cpp
__attribute__((used, retain)) static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(static_cast<int(*)(const char*, ...)>(&::printf)),
    reinterpret_cast<const void*>(static_cast<int(*)(const char*)>(&::puts)),
    ...
};
```

It anchors the whole borrowed set — `printf`, `puts`, `snprintf`, `malloc`,
`free` — not just the two that were missing, since the other three were only
being pulled in incidentally by something else. The anchor is not a resolver; the
loader still resolves via `.dynsym`.

`--undefined=` linker flags would also work, but this repo deliberately moved away
from "the old pile of `-u` flags" in favour of keeping the list next to the code
that explains it. All six now resolve.

---

## Diagnostic guard

`xip::stat::load()` now verifies the prelink happened instead of trusting it. For
each `JUMP_SLOT` relocation it reads the slot from the flash blob and checks
whether it still points into `.plt` — the signature of an unbound import:

```
[xip] unbound PLT import 'printf' - prelink missing
```

The module is rejected with a named symbol rather than HardFaulting. This replaces
the blanket "reject any module with a `.plt`" guard from `9119755`, which was based
on the mistaken conclusion that such modules could not be XIP'd at all.

---

## Files changed

| File | Change |
|------|--------|
| `test-xip/test-xip-static/spi-flash/src/main.cpp` | call `bind_host_imports()` in `copy_spi_to_internal()` |
| `test-xip/test-xip-static/spi-flash/src/xip-static-loader.cpp` | verify prelink, name unbound imports |
| `test-xip/test-xip-static/spi-flash/CMakeLists.txt` | force-link and export `printf`, `puts` |
| `test-xip/fixtures-src/ldscripts/xip-split.ld` | reverted the ineffective `.plt` move |

## Takeaway

Under XIP, any PC-relative reference that crosses from the flash-resident R-X
segment into the RAM-resident RW segment is broken by construction, because the
loader gives the two segments independent bases. The PLT is the main instance of
this in compiler-generated code. Anything of that shape must be resolved before
the image is committed to flash, since the R-X segment is immutable at load time
by definition.

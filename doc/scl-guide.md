# The SCL Libraries, Explained Simply

A plain-English companion to [README.md](../README.md). It answers three questions for each
of the six SCL libraries: **why does it exist**, **how does it work**, and
**how does a real `main()` use it** — with the exact lifecycle of every test
application broken into *start → configure/activate → use → end/close*.

---

## 1. The problem all six libraries solve

A normal microcontroller firmware is **one frozen block**: every function it
will ever call is glued in at compile time. If you want to run code that is
*not* inside that block — a plug-in, a downloaded module, an alternative
algorithm — you need machinery that answers three questions at run time:

1. **Where do the bytes of the extra code come from?** (a file on the host, a
   region of flash, somewhere in the firmware image itself)
2. **Where does that code run from?** (a piece of RAM chosen at run time, or a
   fixed address chosen by the linker)
3. **How do names get connected?** The extra code calls `printf` or reads
   `fm_var` from the firmware — someone must patch the right addresses in, and
   the firmware must be able to find `library_function` inside the extra code.

The six SCL libraries are different answers to those questions,
with different trade-offs. `scl::common` is the toolbox they all share.

---

## 2. `scl::common` — the shared toolbox

**Why.** Most of the loaders need the same low-level chores: read an ELF
file, find a symbol by name, apply relocations. Writing that once is the whole
point of the SCL (the original repo had four diverging copies).

**What is inside, in plain words:**

| Piece | Plain-English job |
|---|---|
| `byte_source` | "give me N bytes at offset X" — hides WHERE a library's bytes live (host file, flash, RAM buffer) |
| `elf_image` | understands the `.so` file format: headers, sections, sizes |
| `symbol_resolver` | phone book: name → address, using the ELF hash table |
| `reloc_applier` / `relocator` | the patcher: walks the relocation list and writes correct addresses into the loaded image |
| `region_allocator` | hands out RAM slots for loaded libraries from a reserved arena |
| `LazyResolve` | figure out, from a patched call slot, *which* symbol it stands for (used for on-first-call binding) |
| `result<T>` / `Error` | C++20 "value or error code" return type used everywhere |
| `scl_log` hook | all diagnostics go through two tiny functions the app may override; silent by default |

**Example.** Every loader begins the same way:

```cpp
Somebyte_source src(...);                    // app decides where bytes come from
auto img = loader::elf_image::parse(src);    // common: is this a valid ARM .so?
// -> from here the loaders diverge.
```

---

## 3. `scl::dynamic-nolib` — the real `dlopen`, like Linux, but bare-metal

**Reason.** You want what a PC gives you: pick a `.so` file *at run time*,
load it *anywhere* in RAM, call it by name, unload it, load another. Several
libraries at once, libraries calling each other, even C++ exceptions crossing
between them.

**Implementation, step by step.** `dlopen_source()` copies the file's load
segments into a slot of the RAM arena, then patches the library's **GOT** — a
table of addresses the library reads instead of hard-coding them. Because the
library was compiled *position-independent* with register **r10** pointing at
its GOT, the same bytes work at ANY RAM address: entering the library is just
"set r10 to its table, jump" (`pic_call_*` trampolines do this). Imports the
library needs (`printf`, `fm_var`...) are looked up in the firmware's own
symbol table (`_DYNAMIC`, exported with `-Wl,-E`) and written into the GOT —
eagerly, or **lazily** on first call, or routed through a small veneer
(`dl_cross_call`) when one library calls another so each runs with its own r10.

**Trade-off.** Most flexible; costs RAM for every loaded library and needs
the strictest build contract (PIC firmware, exported symbols, reserved arena).

**Fits `main.cpp` of `scl-fw-dyn1` / `scl-fw-dyn2`:**

```cpp
// ---------- START -------------------------------------------------
board_init();                             // clocks, LED, SysTick (platform)
trace_puts("starting\n");                 // semihosting console up

// ---------- CONFIG / ACTIVATION -----------------------------------
// dyn1: nothing to configure — defaults are fine.
// dyn2 (battery) turns on cross-library routing and its tracing:
fw::dl_enable_cross_call(true);
fw::dl_cross_trace(1);                    // print [xlat] hops

// ---------- USAGE -------------------------------------------------
auto h = fw::dlopen(DL_LIB_DIR "/mylib1.so");     // app wrapper: semihost file
if (h) {                                          //   -> dlopen_source(...)
    auto fn = fw::dlsym(**h, "library_function"); // name -> address
    uint32_t r = pic_call_u32(*fn,                // call with r10 = lib's GOT
                  (void*)(*h)->got_base, 39);
    // dyn2 additionally: loads c1+c2+cpp3 together, lets them call each
    // other, throws exceptions across, checks dl_fixup_count() == "lazy
    // binding happened exactly once", dumps GOT walkthroughs...
}

// ---------- END / CLOSE -------------------------------------------
fw::dlclose(**h);      // run destructors, free the RAM slot.
                       // Refused (with a message) while another loaded
                       // library still imports from this one.
```

---

## 4. The flat-fs backend — same loader, different shelf

*(Not a separate SCL library — the reference example of swapping the
`byte_source`. App-owned code in `overlay-flat_fs`, exercised by `scl-fw-flatfs`.)*

**Reason.** Semihosting needs a debugger attached. A shipped device must load
libraries from somewhere it owns — here: 256 KB of on-chip flash (sectors
6–7) organised as a tiny append-only archive (name → bytes, with CRC).

**Implementation.** `fs_add()` programs a blob into the erased area and
commits its directory entry last (so a torn write is simply invisible).
Reading is free: internal flash is memory-mapped, so `Flashbyte_source` is a
`memcpy`. `dlopen_fs("c1.so")` finds the blob and feeds it to the SAME
`dlopen_source()` as before — the loader never notices the difference. The
flash *programming* routines run from RAM (`.ramfunc`), because on this chip
code cannot execute from flash while flash is being written.

**Fits `main-flatfs.cpp` of `scl-fw-flatfs`:**

```cpp
// ---------- START -------------------------------------------------
trace_puts("flat_fs: on-chip filesystem build\n");

// ---------- CONFIG / ACTIVATION -----------------------------------
load_module();                 // reads the OpenOCD command line:
                               //   (none) -> provision if archive absent:
                               //             fs_format(); fs_add("c1.so",...) ...
                               //   -c/-d/-l -> format / delete-all / list
trace_u32("[boot] flat_fs files = ", fw::fs_count());
g_use_flatfs = true;           // THE switch: battery's dlopen goes to flash

// ---------- USAGE -------------------------------------------------
battery_run();                 // the same 16 tests as dyn2 — but every
                               // dlopen is dlopen_fs(name) -> Flashbyte_source
                               // (no debugger file I/O at load time)

// ---------- END / CLOSE -------------------------------------------
// dlclose per test, as in dyn2. The archive itself stays in flash across
// reboots and code reflashes; only fs_format() erases it.
```

---

## 5. `scl::overlay` — the linker did the thinking, we just copy

**Reason.** Sometimes you don't need run-time flexibility at all — you need
three features that never run at the same time to SHARE one small RAM window.
Old-school "overlays": maximum speed and simplicity, zero run-time linking.

**Implementation.** The **linker** places all modules at the SAME run address
(the window) and stores their bytes back-to-back in flash; it also refuses to
link if one overlay references another (`NOCROSSREFS`). At run time the SCL
manager is ~50 lines: `memcpy` the chosen module flash→window, run `DSB; ISB`
(so the CPU doesn't execute stale bytes), remember which one is resident.
All addresses were fixed at link time — no ELF parsing, no symbol lookup, no
GOT. The price: modules are part of the firmware image, only one per window
is alive, and calls into the window need `long_call` (it is far from flash).

**Fits `main-overlay.cpp` of `scl-fw-ovl1`:**

```cpp
// ---------- START -------------------------------------------------
board_init();
ov_init();                       // build the descriptor table from linker
                                 // symbols: {window VMA, flash LMA, size}
                                 // per module; scl_overlay_init(&ovl, mods, 3)

// ---------- CONFIG / ACTIVATION -----------------------------------
// none — everything was decided at link time (that's the whole point).

// ---------- USAGE -------------------------------------------------
scl_overlay_load(&ovl, OV1);     // memcpy + barriers; no-op if resident
uint32_t r = ov1_use_fm();       // direct call, address fixed at link time
scl_overlay_load(&ovl, OVCPP);   // OV1 is now gone — window re-used
ovcpp_method(2, 100);
// the app then runs the SAME scenarios through scl::dynamic-nolib and prints the
// addresses side by side: overlays alias ONE window, dlopen'd libs each get
// their own base.

// ---------- END / CLOSE -------------------------------------------
// nothing to close: loading the next module IS the cleanup.
```

---

## 6. `scl::dynamic-newlib` — loads like dynamic, binds like static, UNDOES itself

**Reason (in plain words).** This is the middle-road loader; it came from the
`static-unify_code` project. Three ideas together:

* **Still load modules on the fly.** Like `dlopen`, you can bring in an extra
  `.so` while the program is running — it is *not* baked in at compile time.
* **On a "comfortable" firmware.** *Hosted* means this firmware already carries
  a standard C library and STMicro's driver code, so it has a working heap
  (`malloc`/`new` — a pool it grabs memory from on demand) and ready-made HAL
  drivers. The bare-metal `scl::dynamic-nolib` firmware deliberately has none of
  that; this one uses them.
* **A perfect undo.** Loading a module means writing new addresses into memory
  (patching). This loader writes down every change it makes, so `unload` can
  put every byte back exactly as it was — leaving the system bit-for-bit
  identical to before the module was loaded, as if it had never been there.

**Why the perfect undo is worth it.** Because each load starts from a
*guaranteed-clean slate*, you can safely load one module, use it, unload it,
then load a **different** module into the same freed space — over and over —
with no risk that leftovers from the previous one corrupt the next. Concretely
that buys you:

* **A/B and hot-swap.** Run algorithm A, unload, run algorithm B in the same
  spot, compare — with confidence the two runs can't contaminate each other.
* **Recovery from a bad module.** If a loaded module misbehaves, `unload`
  fully rewinds the damage instead of leaving the firmware in a half-patched,
  unknown state.
* **Long-running reuse.** A device that cycles through many plug-ins over days
  never accumulates stale patches; every load looks like the first.
* **Bidirectional links stay honest.** It also patches the firmware's *own*
  call table with a module's exports; the journal makes sure those are undone
  too, so unloading truly severs every connection in both directions.

> **Where the "clean spot" really lives.** The module's *own* bytes are
> trivially clean each time — each load gets a **fresh heap buffer** (`new[]`)
> and the old one is freed, so a new module never lands on the previous one's
> remains. The journal's real job is the harder half: undoing the patches this
> loader stamped **outside** the module — into the firmware's own call table
> and into any *other* libraries still loaded. Without the journal those
> outside edits would pile up; with it, the whole system — not just the freed
> buffer — returns to a known-clean state on every unload.

**Implementation.** `dynamic_linker` loads the image into heap memory
(`new[]`, not an arena) and patches it — but every single word it writes,
in the library AND in the firmware's own GOT, is first recorded in the
**patch_journal** (address + original value). `unload()` replays the journal
backwards: a perfect undo. It also links in BOTH directions: the firmware's
GOT can be patched with symbols a loaded library exports (with a pending list
re-resolved when later libraries arrive). Instead of parsing `_DYNAMIC`, its
`link_context` reads a small `_reloc` table the linker script emits. Unresolved
libc names fall back to a built-in table (newlib `printf`, `malloc`, ...).

**Fits `main.cpp` of `scl-fw-dynamic-newlib1` / `scl-fw-dynamic-newlib2`:**

```cpp
// ---------- START -------------------------------------------------
timer tim; tim.start();          // HAL clocks (hosted platform)
led board_led(0, 5, true); board_led.power_up();

// ---------- CONFIG / ACTIVATION -----------------------------------
// dl-impl.cpp creates the machinery once, at static-init time:
//   firmware::link_context  g_ctx;        // reads the _reloc table
//   firmware::dynamic_linker g_linker(g_ctx);
// nothing else to switch on.

// ---------- USAGE -------------------------------------------------
dl::handle h = dl::dlopen("/tmp/mylib1.so");   // POSIX-style facade:
if (!h) trace_printf("%s\n", dl::dlerror());   //   load_semihost -> journal
auto fn = (unsigned(*)(unsigned))dl::dlsym(h, "library_function");
unsigned r = fn(42);                           // plain call — non-PIC firmware
// static1 proves the undo: dlclose, dlopen AGAIN, same result == the journal
// really restored every patched word.

// ---------- END / CLOSE -------------------------------------------
dl::dlclose(h);        // journal replayed backwards, heap freed —
                       // the firmware is bit-identical to before dlopen.
```

---

## 6b. `scl::static` — the standalone ELF32 patcher

**Why.** A self-contained loader (imported from the former `shared-static-rework`
firmware) that takes a PIC `.so`, drops it in a RAM arena, and **patches its GOT**
in place so the bare-metal app can call in — like `dynamic-newlib`, but shipped as
its own demo firmware with its own µOS++ startup and the `_reloc`-table model.

**How.** Its `elf_reader` is a thin adapter over the shared `loader::elf_image`
parser; `shlib_loader` does the segment load, `patch_shlib`/`patch_main` GOT
patching, `R_ARM_*` fixups, weak symbols, constructors, and `DT_NEEDED`. Lives in
`src/static/` + `include/static/`; the demo app is `test-static/test-static/`.

## 6c. `scl::reloc` — load a kernel-module-style `.o`, no GOT at all

**Why.** All the loaders above consume `-fPIC -shared` (**ET_DYN**) objects with a
GOT/PLT. `scl::reloc` is the *other half of the design space*: it loads plain
**relocatable** objects (`-mlong-calls -fno-pic -r`, **ET_REL**) — the way the
Linux kernel loads a module. No GOT, no PLT, no `r10`, no lazy binding.

**How.** `-fno-pic` means the module runs at a fixed absolute address, so every
call is `ldr rN,=target ; blx rN` — an **`R_ARM_ABS32`** literal the loader fills
with the real address at load time (`-mlong-calls` gives it unlimited range).
`reloc::load` places one module; **`reloc::load_set`** links a *group* in two
phases (place all → resolve all → run ctors) so **mutually/cyclically dependent**
modules work — the *eager* twin of `dynamic-nolib`'s *lazy* `dl_cross_call`. The
`test-relocatable/` battery runs the cyclic `c1+c2+cpp3` set and gets values
**bit-identical** to the `dynamic-nolib` `dyn2` run — same behaviour, no GOT.
Lives in `src/reloc/` + `include/reloc/`; demo in the top-level `test-relocatable/`.

**The trade-off in one line.** GOT/PLT (`dynamic-*`) pays one indirection per call
forever but loads lazily and handles cycles trivially; ET_REL (`reloc`) pays a
heavier "see the whole set at load" link for direct, indirection-free calls.

---

## 7. One table to compare them

| | `scl::dynamic-nolib` | flat-fs (backend) | `scl::overlay` | `scl::dynamic-newlib` | `scl::static` | `scl::reloc` | `scl::xip-*` |
|---|---|---|---|---|---|---|---|
| Bytes come from | any `byte_source` (semihost file) | on-chip flash archive | inside the firmware image | any `byte_source` (semihost, heap-buffered) | any `byte_source` | any `span<byte>` | any `byte_source` |
| Runs from | RAM arena slot, address chosen at run time | RAM arena slot | ONE fixed RAM window | heap buffer | caller-provided buffer | RAM arena | flash (R-X) + RAM (RW) |
| Names connected | at load time (or first call), by the loader | same | at LINK time, by the linker | at load time, journalled | at load time, GOT patch | at load time, ABS32 patch | at load time, split-base |
| Calling in | `pic_call_*` (set r10) | same | plain `long_call` | plain call | `pic_call_*` | direct `ldr+blx` | `pic_call_*` (r10) |
| Undo on close | dtors + free slot | same | overwrite window | **byte-exact revert** (patch_journal) | no unload | no unload | free RAM arena |
| Best for | plug-ins, many libs, C++/EH across modules | field devices, no debugger | few fixed alternates, max speed | hosted firmware needing clean unload | standalone patcher, no -fpic fw | kernel-module style, no GOT | RAM-constrained, flash-resident code |
| Cost | strict build contract, RAM per lib | + flash region, RAM-resident flasher | modules frozen into image | heap, hosted runtime | soft-float ABI, no lazy binding | no PIC, no GOT, heavier load | split segments, flash alignment |

*Start-to-close in one sentence each:* **dynamic** = open file → copy to RAM →
patch table → set r10 and call → destructors and free; **flat-fs** = same, but
"open file" is "find blob in flash"; **overlay** = copy fixed bytes into the
fixed window → call directly → next copy replaces it; **static** = copy to
heap → patch (writing everything down) → call directly → replay the notes
backwards; **reloc** = place sections → patch ABS32 → call directly (no GOT);
**xip** = classify segments → copy RW to RAM → patch split-base → execute from flash.

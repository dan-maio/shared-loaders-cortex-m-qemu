# test4 — Standard ARM GOT[2]/PLT0 lazy binding, adapted for Cortex-M

How an external function call reaches the loader on the first call, gets its
symbol resolved "the standard way" (through the GOT[2] resolver slot), and then
runs directly on every later call — and what has to change to make that path
work on a Cortex-M4 with our in-house loader instead of `ld-linux.so`.

Grounded in the actual `cpp3.so` fixture (`arm-none-eabi-objdump -d`); all
addresses are the library's link-time VMAs (add the runtime load base `B`).

---

## 1. The reserved GOT slots (System V, same on ARM/Linux)

A dynamically-linked object's `.got`/`.got.plt` starts with three reserved
words the dynamic linker owns:

| Slot | Address | Holds | Filled by |
|---|---|---|---|
| **GOT[0]** | `.got + 0` | `&_DYNAMIC` | static linker |
| **GOT[1]** | `.got + 4` | module id / `link_map*` | dynamic linker at load |
| **GOT[2]** | `.got + 8` | **`&_dl_runtime_resolve`** (the lazy resolver) | dynamic linker at load |
| GOT[3…] | `.got + 12` | one JUMP_SLOT per imported function | resolved lazily |

In `cpp3.so`: `.got` VMA = `0x1978`, so **GOT[2] = `0x1980`**, and the first
real import slot is GOT[3] = `0x1984`.

---

## 2. The standard lazy-binding path (call → PLTn → PLT0 → GOT[2])

On the **first** call to an imported function, its GOT slot does **not** yet
hold the target — it holds `&PLT0`. So the call bounces through PLT0 to the
resolver:

```
   caller                PLTn (e.g. sinf@plt)         PLT0                     GOT[2]
   ------                --------------------         ----                     ------
   bl sinf@plt  ───►  movw/movt ip,#gotoff       push {lr}              &_dl_runtime_resolve
                      add   ip, pc          ┌►   ldr.w lr,[pc,#8]              │
                      ldr.w pc,[ip]  ───────┘    add   lr, pc  (= &GOT[0])     │
                        │  (GOT[n] == &PLT0        ldr.w pc,[lr,#8]! ──────────┘
                        │   on first call)          (lr = &GOT[2], pc = GOT[2])
                        ▼                                    │
                    ip = &GOT[n]  (kept)                     ▼
                                                     _dl_runtime_resolve
                                                       reads ip = &GOT[n]  (which slot)
                                                       reads lr / stack     (link_map)
                                                       → dl_fixup(&GOT[n]):
                                                          find JUMP_SLOT, resolve name,
                                                          GOT[n] = target|1, return target
                                                                     │
   2nd call: PLTn → ldr pc,[ip] → GOT[n] == target  ◄────────────────┘  (direct; no PLT0)
```

Two invariants make it work: **PLTn leaves `ip (r12) = &GOT[n]`** (so the
resolver learns *which* slot faulted), and **GOT[2] holds the resolver** (so
PLT0 can reach it). GOT[n] starts as `&PLT0`, and `dl_fixup` overwrites it with
the resolved target, so the slot "self-heals" after one call.

### What `cpp3.so` actually contains

```asm
00000480 <.plt>:                        ; PLT0
 480: b500        push {lr}
 482: f8df e008   ldr.w lr,[pc,#8]       ; lr = *(0x48c) = 0x000014ec
 486: 44fe        add   lr, pc           ; lr = 0x14ec + PC_at_add
 488: f85e ff08   ldr.w pc,[lr,#8]!      ; lr += 8 (→ &GOT[2]); pc = GOT[2]
 48c: 000014ec    .word 0x000014ec       ; PC-relative offset to &GOT[0]

00000490 <c2_scale@plt>:                 ; PLTn (every import looks like this)
 490: f241 4ce8   movw ip,#0x14e8
 494: f2c0 0c00   movt ip,#0
 498: 44fc        add  ip, pc            ; ip = &GOT[n]
 49a: f8dc f000   ldr.w pc,[ip]          ; pc = GOT[n]  (ip preserved = &GOT[n])
 49e: e7fc        b.n  49a               ; (dead tail)
```

The initial GOT[n] value in the on-chip dump is `0x00000481` = `0x480 | 1` =
**`&PLT0` with the Thumb bit** — confirming the first call routes PLTn → PLT0.

---

## 3. Why the PLT0 / GOT[2] hop breaks on Cortex-M

The `.so` was linked for `ld-linux.so` on ARM; two of its assumptions do not
hold under our loader:

**(a) PLT0's PC-relative GOT base is off by 4 (Thumb PC-alignment).**
PLT0 computes `&GOT[0] = 0x14ec + PC_at_add`. For Thumb, the PC read by
`add lr, pc` at `0x486` is `Align(0x486 + 4, 4) = 0x488`, giving
`0x14ec + 0x488 = 0x1974`. But `.got` is at **`0x1978`** — **off by 4**. So
`ldr pc,[lr,#8]!` reads `0x1974 + 8 = 0x197c` (GOT[1]), **not** GOT[2]
(`0x1980`): PLT0 jumps through the wrong slot. (The linker assumed a different
PC-alignment convention than the core actually uses.)

**(b) PLT0 clobbers `lr`.** `ldr pc,[lr,#8]!` leaves `lr = &GOT[2]`, but our
`dl_runtime_resolve` expects `lr = the caller's return address`. glibc's
resolver knows PLT0 pushed the real `lr` and recovers it; ours does not.

A wrapper (`resolve_via_got2`: `ldr lr,[sp],#4 ; b dl_runtime_resolve`) can undo
the `push {lr}`, and patching the `0x14ec` word fixes (a) — but chaining both
through the mis-linked PLT0 is fragile and buys nothing on this target.

---

## 4. The Cortex-M adaptation (what the loader and this test do)

**Skip PLT0 and GOT[2] entirely; put the resolver *in the JUMP_SLOT itself*.**
Because PLTn already loads GOT[n] into `pc` with `ip = &GOT[n]`, writing
`dl_runtime_resolve|1` straight into GOT[n] makes the first call land in the
resolver with the slot address already in `ip` — the same information PLT0/GOT[2]
would have provided, without the two broken assumptions:

```
first call:  PLTn → ip=&GOT[n]; pc = GOT[n] = dl_runtime_resolve
                  → dl_fixup(&GOT[n]) → resolve name → GOT[n] = target|1
later calls: PLTn → pc = GOT[n] = target            (direct)
```

GOT[2] is still populated (for demonstration) but is **never consulted**.

Two Cortex-M specifics this test exercises:

- **Combined JMPREL.** `dl_fixup` searches `h->lazy` (the `.rel.dyn` JMPREL).
  C++/`-nostdlib` libs like `cpp3.so` emit their JUMP_SLOTs into **`.rel.plt`**,
  not `.rel.dyn`. The test concatenates the two tables and points `h->lazy` at
  the combined span so `dl_fixup` can find `.rel.plt` slots
  (`build_combined_jmprel`).
- **Force the lazy state.** After `dlopen` (cross-call off) the `.rel.plt` slots
  are *eagerly bound* to their targets, so no fixup would fire. The test rewrites
  each `.rel.plt` JUMP_SLOT back to `dl_runtime_resolve|1` to demonstrate the
  lazy path.

---

## 5. Call-path summary (actual, on Cortex-M)

```
cpp3_method()  ── bl sinf@plt ──►  PLTn: ip = &GOT[n]; pc = GOT[n]
                                          = dl_runtime_resolve
                                     └►  dl_fixup(ip = &GOT[n])
                                           search combined .rel.dyn + .rel.plt
                                           resolve JUMP_SLOT symbol (main exports)
                                           GOT[n] = target | 1  ; ++fixup_count
                                           return target ─► runs, returns to caller
   2nd call:  PLTn → pc = GOT[n] = target          (direct, no fixup)
```

| Metric | Expected |
|---|---|
| `fixup_count` before | 0 |
| after 1st call | **6** (unique JUMP_SLOTs in `cpp3_method`: `trace_event`, `sinf`, `cosf`, `logf`, `sqrtf`, `trace_u32`) |
| after 2nd call | 6 (no new fixups — slots patched) |
| `result1` / `result2` | equal, non-zero (`1432`) |
| verdict | `lazy binding: PASS` + `call result: PASS` |

---

## 6. Reserved-slot cheat sheet

```
GOT[0] = &_DYNAMIC              GOT[3..] = imports (JUMP_SLOTs)
GOT[1] = link_map / module id     first call:  &PLT0  (standard)  OR
GOT[2] = &resolver  ◄── standard              dl_runtime_resolve  (our adaptation)
         path hops here                 after fixup: resolved target | 1
```

The one-line difference between "standard" and "Cortex-M here": **standard**
puts the resolver at GOT[2] and reaches it via PLT0; **we** put the resolver
directly in each import's GOT[n] and never touch PLT0/GOT[2].

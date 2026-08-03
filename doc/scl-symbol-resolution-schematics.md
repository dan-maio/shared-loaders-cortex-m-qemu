# SCL Symbol Resolution Pipeline — Call Stack Schematics

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Relocation Types Reference](#2-relocation-types-reference)
3. [GOT / PLT / .got.plt Architecture](#3-got--plt--gotplt-architecture)
4. [Function Call Flows](#4-function-call-flows)
5. [Data Symbol Access](#5-data-symbol-access)
6. [Lazy Binding (JUMP_SLOT)](#6-lazy-binding-jump_slot)
7. [Cross-Library Calls (r10 Swap)](#7-cross-library-calls-r10-swap)
8. [Exception Handling (EH) Symbols](#8-exception-handling-eh-symbols)
9. [ET_REL (Relocatable) Loader](#9-et_rel-relocatable-loader)
10. [PIC Register (r10) Lifecycle](#10-pic-register-r10-lifecycle)
11. [Source File Index](#11-source-file-index)
12. [Test Files — Where Each Resolution Happens](#12-test-files--where-each-resolution-happens)

---

## 1. Architecture Overview

SCL has **three loader pipelines**, each with distinct symbol resolution:

```
┌─────────────────────────────────────────────────────────────────┐
│                      SCL Loader Pipelines                       │
├──────────────┬──────────────────┬───────────────────────────────┤
│ Static       │ Dynamic-Nolib    │ Relocatable (ET_REL)          │
│ (hosted)     │ (freestanding)   │ (no GOT/PLT)                  │
├──────────────┼──────────────────┼───────────────────────────────┤
│ shlib-loader │ dl.cpp           │ reloc-loader.cpp              │
│ .cpp         │ dlopen/dlsym/    │ load() / load_set()           │
│ Module::     │ dlclose          │ .symtab/.strtab               │
│ build()      │ .dynsym/.dynstr  │ R_ARM_ABS32/REL32/THM_CALL   │
│              │ GOT+PLT+r10      │ -fno-pic -mlong-calls         │
└──────────────┴──────────────────┴───────────────────────────────┘
```

---

## 2. Relocation Types Reference

### ET_DYN (Shared Objects)

| Type | Value | GOT Write Formula | Used For |
|------|-------|-------------------|----------|
| `R_ARM_NONE` | 0 | (no write) | Registration-only entries |
| `R_ARM_ABS32` | 2 | `*slot += sym_target()` | Absolute data pointers |
| `R_ARM_REL32` | 3 | `*slot += sym_target() - (base + r_offset)` | PC-relative references |
| `R_ARM_COPY` | 20 | `memcpy(slot, src, st_size)` | Data in main from lib |
| `R_ARM_GLOB_DAT` | 21 | `*slot = sym_target()` | Data GOT entries |
| `R_ARM_JUMP_SLOT` | 22 | `*slot = sym_target()` | Function PLT entries |
| `R_ARM_RELATIVE` | 23 | `*slot += base` | Local data pointers |

### ET_REL (Relocatable Objects)

| Type | Value | Patch Formula | Used For |
|------|-------|---------------|----------|
| `R_ARM_ABS32` | 2 | `*slot += sym_addr` | Data references |
| `R_ARM_REL32` | 3 | `*slot += sym_addr - offset` | PC-relative refs |
| `R_ARM_THM_CALL` | 10 | Thumb BL encoding | Function calls |
| `R_ARM_TARGET1` | 38 | Same as ABS32 | Bare-metal alias |

---

## 3. GOT / PLT / .got.plt Architecture

### Memory Layout (per loaded library)

```
┌─────────────────────────────────────────────────────────┐
│                    Loaded Library Image                  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .text (code)                                      │  │
│  │   PLT stub 0:  add ip,pc,#off; ldr pc,[ip]       │  │
│  │   PLT stub 1:  add ip,pc,#off; ldr pc,[ip]       │  │
│  │   PLT stub N:  ...                                │  │
│  │   Library code:  bl PLT_stub_N                    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .got (Global Offset Table)                        │  │
│  │   GOT[0]: data symbol A address  (GLOB_DAT)      │  │
│  │   GOT[1]: data symbol B address  (GLOB_DAT)      │  │
│  │   GOT[2]: __gxx_personality_v0   (GLOB_DAT)      │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .got.plt (PLT GOT)                                │  │
│  │   GOT.plt[0]: &_GLOBAL_OFFSET_TABLE_ (reserved)  │  │
│  │   GOT.plt[1]: link_map (reserved)                │  │
│  │   GOT.plt[2]: dl_runtime_resolve (reserved)      │  │
│  │   GOT.plt[3]: func X target  (JUMP_SLOT)        │  │
│  │   GOT.plt[4]: func Y target  (JUMP_SLOT)        │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .rel.dyn / .rel.plt (Relocation Tables)           │  │
│  │   {r_offset, r_info(type, sym)} for each reloc    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .dynsym / .dynstr / .hash (Symbol Table)          │  │
│  │   Symbol resolution via ELF hash chain walk       │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### PLT Stub Anatomy (ARM Thumb)

```
PLT_stub_N:                         GOT.plt[N]:
┌──────────────────────┐            ┌──────────────────────┐
│ add ip, pc, #page_hi │            │                      │
│ add ip, ip, #page_lo │───────►    │ target_func_addr     │
│ ldr pc, [ip]         │            │ (or dl_runtime_resolve│
└──────────────────────┘            │  before first call)  │
                                    └──────────────────────┘
```

---

## 4. Function Call Flows

### 4a. Main → Library (Eager Binding)

```
┌─────────────────────────────────────────────────────────────────┐
│ CALLER: main (compiled -fpic, no r10 usage)                     │
│                                                                 │
│   // C code: lib_func(42);                                      │
│   mov r0, #42                                                   │
│   bl  <PLT_stub_for_lib_func>     ◄── linker-generated PLT     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT STUB (in main's .plt)                                       │
│                                                                 │
│   add ip, pc, #page_offset       ; ip = &main_GOT.plt[N]       │
│   ldr pc, [ip]                   ; jump to *GOT.plt[N]          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ GOT.plt[N] (after reloc_applier applied JUMP_SLOT)              │
│                                                                 │
│   contains: absolute address of lib_func                        │
│   i.e.: library_base + lib_func_offset                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY: lib_func                                               │
│                                                                 │
│   ; Function body uses r10-relative GOT access                  │
│   ldr r3, [r10, #offset]    ; r3 = GOT[sym] = &some_global     │
│   ldr r0, [r3]              ; r0 = some_global value            │
│   ...                                                           │
│   bx lr                     ; return to main                    │
└─────────────────────────────────────────────────────────────────┘
```

### 4b. Library → Library (Same Pipeline, Eager)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY A: caller_func()                                        │
│                                                                 │
│   bl  <PLT_stub_N>             ; PLT in A's .plt               │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ A's PLT stub → A's GOT.plt[N]                                   │
│                                                                 │
│   GOT.plt[N] = &lib_b_func    (resolved at load time)          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY B: lib_b_func()                                         │
│                                                                 │
│   ; B's code uses B's r10 for GOT access                        │
│   ; BUT: r10 is still A's GOT base!                             │
│   ; PROBLEM: B's GOT entries are at B's base, not A's           │
│   ; SOLUTION: cross-library routing (see Section 7)             │
└─────────────────────────────────────────────────────────────────┘
```

**Key insight:** Without cross-library routing (P6), library-to-library calls
work only if the callee doesn't access its own GOT. With P6 enabled, the
`dl_cross_call` trampoline swaps r10 to the callee's GOT base.

### 4c. Library → Main (Callback)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY: event_handler()                                        │
│                                                                 │
│   bl  <PLT_stub_for_main_callback>                              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT stub → GOT.plt[N] = &main_callback                          │
│                                                                 │
│   (resolved via combined_resolver: other libs → main)           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ MAIN: main_callback()                                           │
│                                                                 │
│   ; main is -fpic (PC-relative GOT), does NOT use r10           │
│   ; r10 value is irrelevant for main                            │
│   bx lr                                                         │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. Data Symbol Access

### 5a. Library → Own Global (GLOB_DAT, Eager)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY CODE (compiled -msingle-pic-base -mpic-register=r10)    │
│                                                                 │
│   // C code: int x = my_global;                                 │
│   ldr r3, [r10, #GOT_offset_for_my_global]   ; r3 = &my_global │
│   ldr r0, [r3]                               ; r0 = my_global   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ GOT ENTRY (patched by reloc_applier, GLOB_DAT)                  │
│                                                                 │
│   GOT[k] = library_base + my_global_offset                      │
│                                                                 │
│   Relocation: R_ARM_GLOB_DAT                                    │
│   Formula:   *slot = sym_target()                               │
│   Source:    reloc-applier.cpp:46-48                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ DATA SECTION                                                    │
│                                                                 │
│   my_global: .word 0x42          ; actual data                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5b. Library → External Global (Cross-Library GLOB_DAT)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY A CODE                                                  │
│                                                                 │
│   // extern int ext_var;  ext_var = 7;                          │
│   ldr r3, [r10, #GOT_offset_for_ext_var]    ; r3 = &ext_var    │
│   mov r0, #7                                                        │
│   str r0, [r3]                              ; ext_var = 7       │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ A's GOT ENTRY (patched by combined_resolver)                    │
│                                                                 │
│   GOT[k] = library_B_base + ext_var_offset_in_B                 │
│                                                                 │
│   Resolution chain:                                             │
│     1. combined_resolver searches other loaded libs             │
│     2. Finds ext_var in Library B's .dynsym (st_value != 0)     │
│     3. Returns B_base + st_value                                │
│   Source: dl.cpp:166-180                                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY B's DATA SECTION                                        │
│                                                                 │
│   ext_var: .word 0           ; actual storage                   │
└─────────────────────────────────────────────────────────────────┘
```

### 5c. Main → Library Data (R_ARM_COPY in Static Loader)

```
┌─────────────────────────────────────────────────────────────────┐
│ MAIN CODE                                                       │
│                                                                 │
│   // extern int lib_data;  x = lib_data;                        │
│   ldr r3, [pc, #GOT_offset_for_lib_data]   ; r3 = &lib_data    │
│   ldr r0, [r3]                              ; r0 = lib_data     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ MAIN's GOT ENTRY (patched by patch_main, R_ARM_COPY)            │
│                                                                 │
│   GOT[k] = library_base + lib_data_offset_in_lib                │
│                                                                 │
│   Static loader: shlib-loader.cpp:490-516                       │
│   Writes library symbol's runtime address into main's slot      │
│   (NOT standard memcpy semantics — see comment at line 491)     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY's DATA SECTION                                          │
│                                                                 │
│   lib_data: .word 42         ; actual storage in library        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. Lazy Binding (JUMP_SLOT)

### 6a. Load-Time Setup

```
During dlopen (dl.cpp:279-296):

┌─────────────────────────────────────────────────────────────────┐
│ FOR EACH JUMP_SLOT relocation in .rel.plt:                      │
│                                                                 │
│   GOT.plt[n] = (uint32_t)&dl_runtime_resolve | 1   ; Thumb bit │
│                                                                 │
│   Source: dl.cpp:291                                            │
└─────────────────────────────────────────────────────────────────┘
```

### 6b. First Call — Lazy Resolution

```
┌─────────────────────────────────────────────────────────────────┐
│ CALLER CODE                                                     │
│                                                                 │
│   bl <PLT_stub_N>                                               │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT STUB N                                                      │
│                                                                 │
│   add ip, pc, #page_offset       ; ip = &GOT.plt[N]            │
│   ldr pc, [ip]                   ; GOT.plt[N] = dl_runtime_    │
│                                  ;              resolve | 1     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ dl_runtime_resolve (lazy-resolve.c:26-36)                       │
│                                                                 │
│   push {r0-r3, r4, lr}          ; save integer args + return   │
│   vpush {d0-d7}                 ; save VFP args                │
│   mov r0, r12                   ; r0 = &GOT.plt[N] (from ip)   │
│   bl  dl_fixup                  ; resolve symbol, patch GOT    │
│   mov r12, r0                   ; r12 = target_addr | 1        │
│   vpop {d0-d7}                  ; restore VFP                  │
│   pop {r0-r3, r4, lr}           ; restore args                 │
│   bx r12                        ; tail-call to target          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ dl_fixup (dl.cpp:456-489)                                       │
│                                                                 │
│   1. Find owning handle by GOT slot address                     │
│   2. Map GOT offset → JUMP_SLOT index → symbol name            │
│   3. Resolve: self → other libs → main                          │
│   4. *slot = target | 1         ; patch GOT for future calls   │
│   5. Return target | 1                                          │
│                                                                 │
│   Source: dl.cpp:484-486                                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ TARGET FUNCTION (resolved)                                      │
│                                                                 │
│   ; Executes normally                                           │
│   ; Returns to original caller (dl_runtime_restore popped lr)   │
└─────────────────────────────────────────────────────────────────┘
```

### 6c. Second+ Call — Direct (No Trampoline)

```
┌─────────────────────────────────────────────────────────────────┐
│ AFTER lazy resolution, GOT.plt[N] = target_func | 1            │
│                                                                 │
│   PLT stub:                                                     │
│     add ip, pc, #offset                                         │
│     ldr pc, [ip]           ; → target_func directly             │
│                                                                 │
│   dl_runtime_resolve is BYPASSED entirely                       │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. Cross-Library Calls (r10 Swap)

### 7a. When Enabled

Cross-library routing (P6) is enabled when `g_cross_call` flag is set.
All **external** JUMP_SLOTs (`st_value == 0`) are overwritten with
`&dl_cross_call | 1` (dl.cpp:312-329).

### 7b. Full Call Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY A: caller()                                             │
│                                                                 │
│   // Calls library B's function                                 │
│   bl <PLT_stub_N>                                               │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ A's PLT STUB → A's GOT.plt[N] = dl_cross_call | 1              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ dl_cross_call (xip-cross-call.c:56-78)                          │
│                                                                 │
│   push {r4,r5,r6,r7,r10,lr}     ; save A's r10 + callee-saved  │
│   push {r0,r1,r2,r3}            ; save integer args            │
│   vpush {d0-d7}                 ; save VFP args                │
│                                                                 │
│   mov r0, r12                   ; r0 = &GOT.plt[N]             │
│   bl  dl_cross_lookup           ; returns {target|1, B.got}    │
│   mov r4, r0                    ; r4 = target | 1              │
│   mov r5, r1                    ; r5 = B's GOT base            │
│                                                                 │
│   vpop {d0-d7}                  ; restore VFP                  │
│   ldr r0, [sp, #0]             ; restore args                  │
│   ldr r1, [sp, #4]                                              │
│   ldr r2, [sp, #8]                                              │
│   ldr r3, [sp, #12]                                             │
│                                                                 │
│   mov r10, r5                   ; *** r10 = B's GOT base ***   │
│   blx r4                        ; call B with B's r10          │
│                                                                 │
│   add sp, sp, #16               ; pop saved args               │
│   pop {r4,r5,r6,r7,r10,pc}      ; restore A's r10, return     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ dl_cross_lookup (dl.cpp:501-556)                                │
│                                                                 │
│   1. Find caller handle by GOT slot address                     │
│   2. Scan .rel.dyn + .rel.plt to map offset → symbol name      │
│   3. Resolve: other libs (with their got_base) → main (got=0)  │
│   4. Return (callee_got << 32) | (target | 1)                  │
│                                                                 │
│   For main target: got = 0 (main is -fpic, no r10)             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY B: callee()                                             │
│                                                                 │
│   ; r10 = B's GOT base (set by dl_cross_call)                  │
│   ldr r3, [r10, #offset]    ; access B's GOT entries           │
│   ...                                                           │
│   bx lr                     ; return to dl_cross_call          │
└─────────────────────────────────────────────────────────────────┘
```

### 7c. Call Graph (ASCII Art)

```
    ┌──────────┐     bl      ┌──────────┐     ldr pc    ┌──────────────┐
    │ Library A │────────────►│ PLT Stub │──────────────►│ dl_cross_call│
    │  caller() │             │    N     │               │              │
    └──────────┘             └──────────┘               └──────┬───────┘
                                                               │
                          ┌────────────────────────────────────┘
                          │ bl dl_cross_lookup
                          ▼
                   ┌─────────────────┐
                   │ dl_cross_lookup  │
                   │                 │
                   │ 1. find handle  │
                   │ 2. map symbol   │
                   │ 3. resolve addr │
                   │ 4. return got+  │
                   │    target       │
                   └────────┬────────┘
                            │
                          ┌─┘
                          ▼
                   ┌──────────────┐
                   │ dl_cross_call│
                   │              │
                   │ mov r10, B_got│◄── r10 swap
                   │ blx target   │
                   └──────┬───────┘
                          │
                          ▼
                   ┌──────────┐
                   │ Library B │
                   │ callee()  │
                   │ (uses B's │
                   │  r10)     │
                   └──────────┘
```

---

## 8. Exception Handling (EH) Symbols

### 8a. EH Symbol Resolution Chain

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY CODE (throw)                                            │
│                                                                 │
│   ; throw std::runtime_error("oops");                           │
│   ; Compiler generates:                                         │
│   bl __cxa_allocate_exception      ; via PLT                    │
│   bl __cxa_throw                   ; via PLT                    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT → GOT.plt[N] (resolved eagerly or lazily)                   │
│                                                                 │
│   __cxa_allocate_exception: found in main's .dynsym            │
│   __cxa_throw:              found in main's .dynsym            │
│   __gxx_personality_v0:     found in main's .dynsym            │
│                                                                 │
│   Resolution: combined_resolver → main->resolve(name)          │
│   Source: link-context.cpp:10-27 (parses main's _DYNAMIC)      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ MAIN's EH RUNTIME (libsupc++/libgcc)                            │
│                                                                 │
│   __cxa_throw:                                                  │
│     1. Allocate exception object                                │
│     2. Search unwind table via __gnu_Unwind_Find_exidx          │
│     3. Walk .ARM.exidx/.ARM.extab frames                        │
│     4. Call personality routine (__gxx_personality_v0)           │
│     5. Transfer to catch block                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 8b. Personality Routine Thumb Bit Fix

```
┌─────────────────────────────────────────────────────────────────┐
│ PROBLEM: Linker writes personality as PREL31 without Thumb bit  │
│                                                                 │
│   .ARM.extab entry:                                             │
│     ┌─────────────────────────────────────────┐                 │
│     │ generic model | personality = 0x08001234 │ ◄── even addr  │
│     └─────────────────────────────────────────┘     (wrong!)    │
│                                                                 │
│   Cortex-M: blx 0x08001234 → INVSTATE HardFault                │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ FIX: dl.cpp:350-368                                             │
│                                                                 │
│   Walk each exidx entry:                                        │
│     if (generic model && extab has PREL31 personality):          │
│       personality_addr = extab + PREL31_offset                  │
│       *personality_addr |= 1     ; set Thumb bit                │
│                                                                 │
│   Result:                                                       │
│     ┌─────────────────────────────────────────┐                 │
│     │ generic model | personality = 0x08001235 │ ◄── odd addr   │
│     └─────────────────────────────────────────┘     (Thumb!)    │
└─────────────────────────────────────────────────────────────────┘
```

### 8c. Unwind Table Registration

```
┌─────────────────────────────────────────────────────────────────┐
│ LOADING A LIBRARY (dl.cpp:338-370)                              │
│                                                                 │
│   1. Locate .ARM.exidx section                                  │
│   2. Fix personality Thumb bits (see above)                     │
│   3. dl_exidx_register(base, size, exidx_base, exidx_count)    │
│                                                                 │
│   Registry: unwind-find-exidx.c:38                              │
│     static exidx_reg_t g_exidx[DL_EXIDX_MAX];  // max 4        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ __gnu_Unwind_Find_exidx (unwind-find-exidx.c:67-78)             │
│                                                                 │
│   Called by libgcc's unwinder to find unwind table for a PC:    │
│                                                                 │
│   1. Check loaded images:                                       │
│      for each g_exidx[i]:                                       │
│        if pc ∈ [img_base, img_base+img_size):                   │
│          return g_exidx[i].exidx_base                           │
│                                                                 │
│   2. Fall back to main's own table:                             │
│      *pcount = __exidx_end - __exidx_start                      │
│      return __exidx_start                                        │
│                                                                 │
│   NOTE: Strong override of libgcc's weak undefined symbol       │
└─────────────────────────────────────────────────────────────────┘
```

### 8d. Trampoline Unwind Descriptors

```
┌─────────────────────────────────────────────────────────────────┐
│ pic_call_* trampolines (pic-trampoline.c:16-52)                 │
│                                                                 │
│   .fnstart                  ; begin unwind scope                │
│   .save {r10, lr}           ; save r10 + return address         │
│   push {r10, lr}            ; actual push                       │
│   mov r10, r1               ; set callee's GOT base             │
│   blx r0                    ; call function                     │
│   pop {r10, pc}             ; restore r10, return               │
│   .fnend                    ; end unwind scope                  │
│                                                                 │
│   WHY: Without .fnstart/.save, the unwinder sees a naked asm    │
│   push without unwind info, and terminates the stack walk.      │
│   With proper descriptors, it can step THROUGH the trampoline   │
│   frame during exception propagation.                           │
└─────────────────────────────────────────────────────────────────┘

Exception propagation path:

  Library throw
       │
       ▼
  ┌─────────────────┐
  │ B's code frame  │ ◄── .ARM.exidx for B
  └────────┬────────┘
           │ unwind
           ▼
  ┌─────────────────┐
  │ dl_cross_call   │ ◄── .save {r4-r7,r10,lr} in xip-cross-call.c
  │ trampoline frame│
  └────────┬────────┘
           │ unwind (restores A's r10)
           ▼
  ┌─────────────────┐
  │ A's code frame  │ ◄── .ARM.exidx for A
  └────────┬────────┘
           │ unwind
           ▼
  ┌─────────────────┐
  │ main's catch    │ ◄── main's .ARM.exidx
  └─────────────────┘
```

---

## 9. ET_REL (Relocatable) Loader

### 9a. Key Differences from ET_DYN

```
┌─────────────────────────────────────────────────────────────────┐
│ ET_REL (Relocatable Objects) vs ET_DYN (Shared Objects)         │
├────────────────────────────┬────────────────────────────────────┤
│ ET_REL                     │ ET_DYN                             │
├────────────────────────────┼────────────────────────────────────┤
│ No GOT, no PLT             │ GOT + PLT + .got.plt              │
│ No _DYNAMIC                │ _DYNAMIC array                     │
│ .symtab/.strtab            │ .dynsym/.dynstr                    │
│ -fno-pic -mlong-calls      │ -fpic -msingle-pic-base            │
│ ldr rN,=target; blx rN     │ bl PLT_stub → GOT.plt             │
│ R_ARM_ABS32/REL32/THM_CALL │ GLOB_DAT/JUMP_SLOT/RELATIVE      │
│ No r10 needed              │ r10 = GOT base                     │
│ sec_base[shndx]+st_value   │ base + st_value                    │
└────────────────────────────┴────────────────────────────────────┘
```

### 9b. ET_REL Resolution Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ reloc-loader.cpp: load() / load_set()                           │
│                                                                 │
│   Phase 1: PLACE                                                │
│     For each SHF_ALLOC section in .rel.o:                       │
│       Allocate memory, copy section content                     │
│       Record sec_base[shndx] = allocated_address                │
│                                                                 │
│   Phase 2: RESOLVE                                              │
│     For each relocation entry:                                  │
│       ┌─────────────────────────────────────────────────────┐   │
│       │ R_ARM_ABS32 (2):                                    │   │
│       │   *slot += sym_addr                                 │   │
│       │   where sym_addr = sec_base[shndx] + st_value      │   │
│       │                  or external_resolver(name)         │   │
│       ├─────────────────────────────────────────────────────┤   │
│       │ R_ARM_REL32 (3):                                    │   │
│       │   *slot += sym_addr - (sec_base[r_shndx] + r_off)  │   │
│       ├─────────────────────────────────────────────────────┤   │
│       │ R_ARM_THM_CALL (10):                                │   │
│       │   Patch Thumb BL instruction encoding               │   │
│       │   S=(J1<<24)|(J2<<23)|imm10                        │   │
│       │   I1=NOT(J1 XOR S), I2=NOT(J2 XOR S)              │   │
│       │   imm10hi = target[11:2], imm11lo = target[1:0]    │   │
│       ├─────────────────────────────────────────────────────┤   │
│       │ R_ARM_TARGET1 (38):                                 │   │
│       │   Same as ABS32 (bare-metal alias)                  │   │
│       └─────────────────────────────────────────────────────┘   │
│                                                                 │
│   Thumb bit: OR bit 0 for STT_FUNC symbols (line 98)           │
└─────────────────────────────────────────────────────────────────┘
```

### 9c. ET_REL Call Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│ RELOCATABLE OBJECT CODE                                         │
│                                                                 │
│   ; long-call mode: no PLT, direct literal pool load            │
│   ldr r3, =target_func_addr     ; R_ARM_ABS32 relocation       │
│   blx r3                        ; call target                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LITERAL POOL (patched by reloc-loader)                          │
│                                                                 │
│   .word target_func_addr    ; ABS32: *slot += resolved_addr    │
│                                                                 │
│   If target is in same .rel.o:  sec_base[shndx] + st_value     │
│   If target is external:        external_resolver(name)         │
│   If target is STT_FUNC:        OR in Thumb bit (bit 0)        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ TARGET FUNCTION                                                 │
│                                                                 │
│   ; No GOT, no r10, no PLT                                      │
│   ; Direct execution                                            │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. PIC Register (r10) Lifecycle

### 10a. r10 Value Map

```
┌─────────────────────────────────────────────────────────────────┐
│                    r10 Value at Each Point                      │
├─────────────────────────────┬───────────────────────────────────┤
│ Context                     │ r10 Value                         │
├─────────────────────────────┼───────────────────────────────────┤
│ Main code                   │ Don't care (main is -fpic)       │
│ Main calling lib via pic_*  │ Callee lib's GOT base            │
│ Inside library code         │ This library's GOT base          │
│ dl_runtime_resolve          │ Caller's r10 (untouched)         │
│ dl_cross_call (entry)       │ Caller lib's GOT base            │
│ dl_cross_call (after swap)  │ Callee lib's GOT base            │
│ Library → main callback     │ Don't care (main ignores r10)    │
│ After pic_call_* return     │ Restored to caller's value       │
└─────────────────────────────┴───────────────────────────────────┘
```

### 10b. r10 Setup Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│ dlopen loads library at base address `base`                     │
│                                                                 │
│   1. Parse .dynamic to find GOT address                         │
│   2. got_addr = DT_PLTGOT value from _DYNAMIC                  │
│   3. h->got_base = base + got_addr                             │
│                                                                 │
│   Source: dl.cpp:333                                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Main calls library function via pic_call_* trampoline            │
│                                                                 │
│   pic_call_u32(func_ptr, arg1, got_base):                       │
│     push {r10, lr}                                              │
│     mov r10, r2              ; r10 = got_base                   │
│     blx r0                  ; call func_ptr(arg1)               │
│     pop {r10, pc}           ; restore caller's r10              │
│                                                                 │
│   Source: pic-trampoline.c:27-38                                │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Library code executes with r10 = its GOT base                   │
│                                                                 │
│   ; Access own GOT:                                             │
│   ldr r3, [r10, #offset]    ; r3 = GOT[sym]                    │
│                                                                 │
│   ; Access external symbol:                                     │
│   ldr r3, [r10, #offset]    ; r3 = GOT[ext_sym] (resolved)     │
│   ldr r0, [r3]              ; r0 = ext_sym value                │
└─────────────────────────────────────────────────────────────────┘
```

### 10c. Cyclic Dependencies (load_set)

```
┌─────────────────────────────────────────────────────────────────┐
│ load_set() handles cyclic cross-references (dl.cpp)             │
│                                                                 │
│   Library A references Library B                                │
│   Library B references Library A                                │
│                                                                 │
│   Phase 1: PLACE all modules                                    │
│     Allocate memory for each module's sections                  │
│     Record sec_base[shndx] for each module                      │
│                                                                 │
│   Phase 2: RESOLVE each module against the whole set            │
│     For each module, resolver searches:                         │
│       1. Other modules in the set (by symbol name)              │
│       2. External resolver (main's symbols)                     │
│                                                                 │
│   This allows A→B and B→A to both resolve correctly             │
└─────────────────────────────────────────────────────────────────┘
```

---

## 11. Source File Index

### Core Relocation Pipeline

| File | Purpose |
|------|---------|
| `common-elf-reader/include/loader/elf-types.hpp` | ELF structs, `rel_type` enum |
| `common-elf-reader/include/loader/detail/bytes.hpp` | Bounds-checked memory access |
| `include/loader/detail/elf-hash.hpp` | SysV ELF hash function |
| `src/relocator.cpp` | Relocation classification + plan |
| `src/reloc-applier.cpp` | Applies relocations to image |
| `src/symbol-resolver.cpp` | Hash-chain symbol lookup |
| `src/lazy-resolve.cpp` | JUMP_SLOT lazy resolution |
| `src/dynamic-info.cpp` | Parse _DYNAMIC array |
| `src/link-context.cpp` | Main's symbol surface |

### Dynamic-Nolib Loader

| File | Purpose |
|------|---------|
| `include/dynamic-nolib/dl.hpp` | Handle struct, dlopen API |
| `src/dynamic-nolib/dl.cpp` | Full loader pipeline (842 lines) |

### PIC / Cross-Call Trampolines

| File | Purpose |
|------|---------|
| `include/scl/pic-trampoline.h` | r10-setting trampoline API |
| `src/port/pic-trampoline.c` | Naked asm: push r10, set, blx, pop |
| `src/xip/xip-cross-call.c` | Cross-lib: dl_cross_lookup + r10 swap |
| `src/port/dl-cross-call.c` | Thin wrapper for cross-call names |
| `src/port/lazy-resolve.c` | dl_runtime_resolve trampoline |

### Exception Handling

| File | Purpose |
|------|---------|
| `include/scl/unwind-exidx.h` | dl_exidx_register/unregister API |
| `src/port/unwind-find-exidx.c` | __gnu_Unwind_Find_exidx override |

### Static Loader

| File | Purpose |
|------|---------|
| `include/static/shlib-loader.h` | shlib_loader class API |
| `src/static/shlib-loader.cpp` | Two-phase GOT patching |
| `src/static/elf-reader.cpp` | ELF parsing, hash lookup |
| `src/static/module.cpp` | Module::build() orchestrator |

### Relocatable Loader

| File | Purpose |
|------|---------|
| `include/reloc/reloc-loader.hpp` | ET_REL loader API |
| `src/reloc/reloc-loader.cpp` | Place + resolve + run ctors |

---

---

## 12. Test Files — Where Each Resolution Happens

### 12a. Fixture Libraries Source Map

Each `.so` fixture exercises specific relocation types:

| Fixture | Source | Exports | Imports | Relocations Exercised |
|---------|--------|---------|---------|----------------------|
| `mylib1.so` | `test/fixtures-src/mylib1/mylib1.c` | `library_function`, `foo`, `boo`, `lkj` (weak), `aa`, `bb` (data), `str` (RELATIVE) | `AA`, `BB`, `s1` (GLOB_DAT), `f`, `w` (JUMP_SLOT), `printf` | GLOB_DAT, JUMP_SLOT, RELATIVE, weak interposition |
| `mylib2.so` | `test/fixtures-src/mylib2/mylib2.c` | `lf`, `mm` (weak), `aaa`, `bbb` (data) | `AA`, `BB`, `w`, `printf` | GLOB_DAT, JUMP_SLOT, RELATIVE, weak interposition |
| `c1.so` | `test/fixtures-src/c1/c1.c` | `c1_call_c2`, `c1_into_fm`, `c1_data`, `c1_bss` | `c2_add` (cross-lib), `fm_var` (GLOB_DAT), `trace_event` (JUMP_SLOT) | Cross-call r10 swap, GLOB_DAT, JUMP_SLOT, RELATIVE |
| `c2.so` | `test/fixtures-src/c2/c2.c` | `c2_add`, `c2_chain`, `c2_relay`, `c2_scale` | `c1_into_fm` (cross-lib), `cpp3_step` (cross-lib), `fm_callback` | Cross-call r10 swap, hard-float s0 preservation |
| `cpp3.so` | `test/fixtures-src/cpp3/cpp3.cpp` | `cpp3_method`, `cpp3_orchestrate`, `cpp3_recurse`, `cpp3_heap` | `sinf`, `cosf` (libm), `c2_relay` (cross-lib), `malloc`, `_Znwj` (heap) | Cross-call r10 swap, hard-float, C++ heap, deep recursion |
| `libprov.so` | `test/fixtures-src/libprov/libprov.c` | `prov_value` | (none) | Pure export (no relocations) |
| `libcons.so` | `test/fixtures-src/libcons/libcons.c` | `cons_call` | `prov_value` (cross-lib) | Cross-library JUMP_SLOT |
| `libcppsh.so` | `test/fixtures-src/libcppsh/` | `cppsh_make`, `cppsh_compute` | `_Znwj`, `_ZdlPv` (heap) | C++ vtable, operator new/delete JUMP_SLOT |
| `libcppeh.so` | `test/fixtures-src/libcppeh/` | `eh_selftest`, `eh_throw`, `eh_dyncast` | `__cxa_*`, `__gxx_personality_v0` | EH runtime GLOB_DAT/JUMP_SLOT, .ARM.exidx |
| `ehx_catch.so` | `test/fixtures-src/ehx/ehx-catch.cpp` | `ehx_catch_cross` | `eh_throw` (cross-lib), `__cxa_*` | Cross-lib EH unwind through dl_cross_call |
| `libcpp_features.so` | `test-static/test-static-cpp/new-lib/cpp_features.cpp` | `cpp_features_run`, `cpp_features_summary` | `malloc`, `free` | C++ strategy pattern, GOT/PLT |

Relocatable fixtures (ET_REL, no GOT/PLT):

| Fixture | Source | Mechanism |
|---------|--------|-----------|
| `relprov.rel.o` | `test-relocatable/fixtures-reloc/src/relprov.c` | R_ARM_ABS32 exports |
| `relmod.rel.o` | `test-relocatable/fixtures-reloc/src/relmod.c` | R_ARM_ABS32 cross-module, R_ARM_THM_CALL intra-module |
| `multi.rel.o` | `test-relocatable/fixtures-reloc/src/multi/` | `ld -r` bundle of 3 .cpp files |

### 12b. Test Files — Mechanism to Source Mapping

#### GOT (R_ARM_GLOB_DAT) — Data Symbol Resolution

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `aa`, `bb`, `AAA`, `BB`, `s1` | Loader patches GOT slots for data imports at dlopen |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `c1_data`, `c1_bss`, `c2_data`, `c2_const`, `fm_var`, `fm_info` | TEST 4,5: FM reads lib data; lib reads FM data |
| `test-dynamic/test-dynamic-nolib/test2/src/main-tests.cpp` | Same as above | Refactored battery_run() for flat_fs |
| `test-dynamic/test-dynamic-newlib/test1/src/main.cpp` | `AA`, `BB`, `s1` | Hosted platform with newlib |
| `test-static/test-static/src/main.cpp` | `AA`, `BB`, `s1` | Static loader patch_shlib/patch_main |
| `test-static/test-static-direct/src/main.cpp` | `aa`, `bb` | Direct-link scl::Module |
| `test-overlay/test1/src/main-overlay.cpp` | `fm_var`, `fm_info`, `c2_data`, `c2_const` | TEST 1,2: dynamic vs overlay comparison |

#### PLT/.got.plt (R_ARM_JUMP_SLOT) — Function Call Resolution

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `library_function`, `lf`, `cons_call`, `prov_value`, `cppsh_make`, `eh_selftest` | All function imports resolved via GOT/PLT |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `c1_call_c2`, `c2_chain`, `cpp3_method`, `cpp3_orchestrate`, `ehx_catch_cross` | TEST 1-7,10,13-15: cross-module function calls |
| `test-dynamic/test-dynamic-nolib/test4/src/main.cpp` | `cpp3_method` imports (sinf, cosf, etc.) | PLT0/GOT[2] wiring, combined JMPREL |
| `test-dynamic/test-dynamic-newlib/test1/src/main.cpp` | `library_function`, `f`, `w`, `printf`, `strcat`, `log` | Hosted platform function imports |
| `test-static/test-static/src/main.cpp` | `library_function`, `f`, `w`, `printf` | Static loader GOT/PLT patching |
| `test-static/test-static-cpp/src/main.cpp` | `malloc`, `free`, `cpp_features_run` | C++ PLT entries |
| `test-overlay/test1/src/main-overlay.cpp` | `c1_use_fm`, `ov1_use_fm`, `cpp3_method`, `ovcpp_method` | TEST 1,3,5,6: dynamic JUMP_SLOT vs overlay long_call BLX |

#### R_ARM_RELATIVE — Local Pointer Rebasing

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | String literals in mylib1/mylib2 (`"Hello mylib1!"`) | Pointer rebased by load bias |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `c1_tag`, `c2_tag` (string pointers) | TEST 7: GOT walkthrough traces RELATIVE entries |
| `test-dynamic/test-dynamic-newlib/test1/src/main.cpp` | String literals | Hosted platform RELATIVE |
| `test-static/test-static/src/main.cpp` | String pointers in libsh1/libsh2 | Static loader RELATIVE patching |

#### R_ARM_ABS32 — ET_REL Cross-Module References

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-relocatable/src/main.cpp` | `prov_add`, `trace_event`, `fm_callback`, `fm_var`, `sinf`, `malloc`, `_Znwj` | All cross-module refs are ABS32 (no GOT/PLT) |
| `test-relocatable/src/main-interrupt.cpp` | Same + `my_systick_handler` | ABS32 + RAM vector table patching |
| `test-relocatable/src/classic/main.cpp` | Same + `multi_entry`, `multi_shared` | ABS32 + ld -r bundle + flatfs |

#### R_ARM_THM_CALL — ET_REL Intra-Module Calls

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-relocatable/src/main.cpp` | `helper()` within relmod | Thumb BL encoding patch |
| `test-relocatable/src/classic/main.cpp` | Same | Intra-module calls |

#### Lazy PLT Binding (dl_runtime_resolve)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `prov_value` in libcons | First call triggers dl_runtime_resolve, second uses patched slot |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `c1_into_fm`, `fm_callback`, `trace_event` | TEST 11: dl_enable_cross_call(false), fixup_count verification |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | Same as test2 | Refactored battery |
| `test-dynamic/test-dynamic-nolib/test4/src/main.cpp` | `cpp3_method` imports | PLT0 GOT[2] wiring, combined JMPREL, fixup counter |

#### Cross-Library Calls (r10 Swap via dl_cross_call)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `c1_call_c2`→`c2_add`, `c2_chain`→`c1_into_fm`, `cpp3_orchestrate`→`c2_relay`→`cpp3_step` | TEST 1,2,6: multi-hop cross-lib with r10 swaps |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `cpp3_recurse`↔`c2_step_down` (depth 6) | TEST 14: deep cross-recursion, r10 integrity at every level |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `cpp3_float_cross`→`c2_scale` (float in s0) | TEST 15: hard-float preserved through trampoline |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | Two `cpp3.so` handles | TEST 16: independent r10 per instance |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | Same as test2 | Refactored battery |
| `test-relocatable/src/main.cpp` | `c1_call_c2`→`c2_add` (ET_REL) | No r10 needed: ABS32 direct calls |

#### Weak Symbol Interposition

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `foo`, `boo`, `lkj`, `mm` | main's definition overrides lib's weak default |
| `test-dynamic/test-dynamic-newlib/test1/src/main.cpp` | `mm`, `lkj` | Hosted platform weak interposition |
| `test-static/test-static/src/main.cpp` | `foo`, `boo`, `lkj`, `mm` | Static loader symbol_table: first-loaded-wins for global, weak appended |
| `test-static/test-static-direct/src/main.cpp` | `foo`, `boo` | Direct-link weak resolution |

#### C++ Exception Handling (EH)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `cross_error` thrown in libcppeh, caught in main | .ARM.exidx registration, __cxa_* resolved from main, typeinfo matching |
| `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` | `Base*`→`Derived*` dynamic_cast | RTTI: __dynamic_cast + __cxxabiv1 vtables |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `ehx_catch_cross`→`eh_throw`; `cross_error` | TEST 10: cross-lib EH unwind through dl_cross_call trampoline |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | Same as test2 | TEST 10: refactored |
| `test-static/test-static-direct/src/main.cpp` | (EH anchors kept alive) | -Wl,-u __cxa_* anchors for gc-sections |

#### Hard-Float (FPU s0/d0-d7 Preservation)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `cpp3_method`→`sinf`, `cosf`, `logf`, `sqrtf` | TEST 3: C++ method calls libm with hard-float |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `cpp3_float_cross`→`c2_scale` (float in s0) | TEST 15: float crosses lib boundary through trampoline |
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | All cpp3 imports/exports | TEST 7: full GOT walkthrough with hard-float |
| `test-relocatable/src/main.cpp` | `cpp3_method`→`sinf`, `cosf` | ET_REL: libm resolved from main |
| `test-overlay/test1/src/main-overlay.cpp` | `cpp3_method`/`ovcpp_fcompute`→`sinf`, `cosf` | TEST 5: same FPU formula, two address machines |

#### Cross-Module Heap (malloc/new from Main)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` | `cpp3_heap`→`malloc`, `free`, `_Znwj`, `_ZdlPv` | TEST 13: lib allocates from main's heap; coalesce/reuse verified |
| `test-dynamic/test-dynamic-nolib/test3/src/main-tests.cpp` | Same | TEST 13: refactored |
| `test-overlay/test1/src/main-overlay.cpp` | `cpp3_heap`/`ovcpp_heap`→`malloc`, `free` | TEST 6: same heap, two address machines; checksums match |

#### PLT0 / GOT[2] Wiring

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test4/src/main.cpp` | `resolve_via_got2`, PLT0 at VA 0x480 | PLT0's resolver address wired to GOT[2]; combined JMPREL |

#### Overlay (Link-Time Binding, No GOT/PLT)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-overlay/test1/src/main-overlay.cpp` | `ov1_use_fm`, `ov2_sum`, `ovcpp_method` | TEST 1-6 overlay halves: `long_call` BLX, MOVW/MOVT data, shared SRAM window |

#### XIP (Execute from Flash)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-xip/test-xip-dynamic-nolib/test1/src/` | Same as dynamic-nolib | Code runs from flash, GOT patched in RAM |
| `test-xip/test-xip-static/semihosting/` | Same as static | Static loader, XIP from flash |
| `test-xip/test-xip-static-direct/` | Same as static-direct | Direct-link, XIP |
| `test-xip/test-xip-interrupt-static/` | Same + ISR | XIP with interrupt handling |

#### flat_fs (On-Chip Filesystem Loading)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-dynamic/test-dynamic-nolib/test3/src/main-flatfs.cpp` | All battery symbols | Libraries loaded from on-chip flash archive (append-log FS) |
| `test-relocatable/src/classic/main.cpp` | `fsmod1`, `fsmod2` | ET_REL modules packed into flash-resident flatfs archive |

### 12c. Cross-Reference: Mechanism → Test Files

| Mechanism | Test Files |
|-----------|------------|
| **GOT (GLOB_DAT)** | test-dynamic-nolib/test1, test2 (TEST 4,5,7), test3, test-newlib/test1, test-static/*, test-overlay (TEST 1,2) |
| **PLT (JUMP_SLOT)** | test-dynamic-nolib/test1, test2 (TEST 1-7,10,11,13-15), test3, test4, test-newlib/test1, test-static/*, test-overlay (TEST 1,3,5,6) |
| **RELATIVE** | test-dynamic-nolib/test1, test2 (TEST 7), test3, test-newlib/test1, test-static/* |
| **ABS32 (ET_REL)** | test-relocatable/* (ALL tests) |
| **THM_CALL (ET_REL)** | test-relocatable/src/main.cpp, classic/main.cpp |
| **Lazy binding** | test-dynamic-nolib/test1 (5c), test2 (TEST 11), test3 (TEST 11), test4 |
| **Cross-call r10 swap** | test-dynamic-nolib/test2 (TEST 1-7,10,14-16), test3 |
| **Weak interposition** | test-dynamic-nolib/test1, test-newlib/test1, test-static/* |
| **C++ EH** | test-dynamic-nolib/test1 (5d), test2 (TEST 10), test3 (TEST 10) |
| **Cross-lib EH unwind** | test-dynamic-nolib/test2 (TEST 10), test3 |
| **Hard-float FPU** | test-dynamic-nolib/test2 (TEST 3,7,15), test3, test-relocatable, test-overlay (TEST 5) |
| **Cross-module heap** | test-dynamic-nolib/test2 (TEST 13), test3, test-overlay (TEST 6) |
| **Deep recursion r10** | test-dynamic-nolib/test2 (TEST 14), test3 |
| **Two-instance isolation** | test-dynamic-nolib/test2 (TEST 16), test-relocatable/classic (multi) |
| **dlclose refcount** | test-dynamic-nolib/test1, test2 (TEST 8) |
| **Reload/journal revert** | test-dynamic-nolib/test2 (TEST 9), test-newlib/test1 |
| **PLT0/GOT[2]** | test-dynamic-nolib/test4 |
| **Overlay long_call** | test-overlay/test1 (TEST 1-6) |
| **XIP** | test-xip/* |
| **flat_fs** | test-dynamic-nolib/test3/main-flatfs.cpp, test-relocatable/classic |
| **load_set (cyclic)** | test-relocatable/src/main.cpp (TEST 2), classic/main.cpp |
| **ld -r bundle** | test-relocatable/classic/main.cpp (multi) |
| **RAM vector patching** | test-relocatable/src/main-interrupt.cpp |

---

## Quick Reference Card

```
┌─────────────────────────────────────────────────────────────────┐
│                    SCL SYMBOL RESOLUTION                        │
│                    Quick Reference                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  FUNCTION CALL (ET_DYN):                                        │
│    caller → bl PLT_stub → ldr pc,[GOT.plt] → target            │
│                                                                 │
│  DATA ACCESS (ET_DYN):                                          │
│    ldr r3,[r10,#GOT_off] → ldr r0,[r3] → data value            │
│                                                                 │
│  LAZY BINDING:                                                  │
│    1st call: PLT → dl_runtime_resolve → dl_fixup → target       │
│    2nd+ call: PLT → GOT.plt (now = target) → target             │
│                                                                 │
│  CROSS-LIB (r10 swap):                                          │
│    PLT → dl_cross_call → dl_cross_lookup → mov r10,B_got       │
│    → blx target → pop r10 → return to caller                   │
│                                                                 │
│  EH SYMBOLS:                                                    │
│    __cxa_* / __gxx_personality_v0 → resolved from main          │
│    .ARM.extab personality: OR Thumb bit (bit 0)                 │
│    __gnu_Unwind_Find_exidx: strong override, 4-slot registry    │
│                                                                 │
│  ET_REL (no GOT/PLT):                                           │
│    ldr rN,=target; blx rN  (R_ARM_ABS32/THM_CALL)              │
│    sec_base[shndx] + st_value for same-module symbols           │
│                                                                 │
│  r10 LIFECYCLE:                                                 │
│    main: don't care → pic_call_*: set to lib's got_base         │
│    in lib: lib's got_base → cross-call: swap to callee's        │
│    return: restore caller's r10                                 │
└─────────────────────────────────────────────────────────────────┘
```

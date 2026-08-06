# Cortex-M Relocatable Object Loader (scl::reloc)
## Comprehensive Architecture & Implementation Manual

This document provides a comprehensive overview of the design, motivation, implementation, and verification of the **Cortex-M Relocatable Object Loader (`scl::reloc`)**. It explains how relocatable objects (`ET_REL`) are loaded, linked, and executed on bare-metal ARM Cortex-M microcontrollers without an operating system, dynamic linker, or MMU.

---

## 1. Introduction & Context

Microcontrollers running bare-metal firmware typically execute code out of on-chip Flash memory. While this is highly efficient, it makes dynamic system extension (like downloading plugins, kernel-style modules, or software patches at runtime) difficult. 

To solve this, we need a way to load compiled code into RAM at runtime, resolve any external dependencies against the host application, apply the necessary hardware-level address patches (relocations), and call the loaded entry points.

---

## 2. Dynamic Loading Paradigms: ET_DYN vs. ET_REL

On Cortex-M targets, two main compilation and linkage paradigms can be used for dynamic loading:

### 2.1. The Dynamic Shared Object Model (ET_DYN)
In this model, libraries are compiled with Position-Independent Code flags (`-fPIC -shared`) to produce a `.so` file.
- **Global Offset Table (GOT)**: All references to global variables go through an indirection table loaded in RAM.
- **Procedure Linkage Table (PLT)**: All external function calls go through stub trampolines.
- **The `r10` Register**: Because each dynamic library has its own local GOT, the CPU must swap a base register (traditionally `r10` on ARM) to point to the current library's GOT before executing its code. This requires a complex assembly trampoline dispatcher (`dl_cross_call`) for every cross-library call.

```
[Main Application]
       │
       ▼ (r10 swap via Trampoline)
┌──────────────┐
│  PLT Slot    │
└──────┬───────┘
       │
       ▼ (Indirect Jump)
┌──────────────┐      ┌──────────────┐
│  GOT Entry   ├─────►│ Target Code  │
└──────────────┘      └──────────────┘
```

### 2.2. The Relocatable Object Model (ET_REL)
In this model, modules are compiled as standard object files (`-fno-pic -mlong-calls -r`) to produce a `.rel.o` file.
- **No GOT/PLT**: The code references variables and calls functions directly using absolute addresses.
- **In-Place Patching**: The loader places the sections contiguously in RAM, resolves all symbol addresses at load time, and patches the actual machine code instructions in-place.
- **No Register Pollution**: Because there is no local GOT base register, calls and variable accesses are direct. The compiler is free to use all registers, including `r10`, for general optimizations.

```
[Main Application]
       │
       ▼ (Direct absolute jump patched by Loader)
┌──────────────┐
│ Target Code  │
└──────────────┘
```

---

## 3. Why Relocatable Modules? (Advantages & Tradeoffs)

### Advantages
1. **Zero Runtime Overhead**: No double-dereference penalty for global variables. Jumps are direct absolute jumps.
2. **Simplified Toolchain**: No need for complex dynamic linker scripts. Standard static compilation works out of the box.
3. **No Register Reservation**: The compiler does not need to reserve `r10` as a global base register, which improves register allocation for complex math/DSP code.
4. **Memory Compaction**: Code (`.text`) and read-only data (`.rodata`) are placed contiguously in the RAM allocation arena without needing separate alignment pages.

### Tradeoffs
1. **Eager Loading**: All relocations must be resolved immediately at load time. If a symbol is missing, the load fails.
2. **RAM Consumption**: Because code is patched in place, the `.text` section must live in writable RAM rather than read-only FLASH. Note the *baseline*: this is a cost only against **statically-linked firmware that executes in place from flash** — any in-RAM loader pays it. It is **not** a disadvantage versus the ET_DYN loader in this repo. On this MMU-less Cortex-M4 there is no execute-in-place or page sharing for *either* loader: both copy the whole module image into a RAM arena (the dynamic loader copies every `PT_LOAD` segment; see `doc/dynamic-vs-static-linker-explained.md`), and the ET_DYN image is the *larger* RAM tenant because it also keeps program headers, `.dynamic`, `.dynsym/.dynstr/.hash` and the GOT/PLT resident. So here the relocatable module is lighter on **both** axes — file size (§12) *and* runtime RAM.

---

## 4. The Linker `-r` Option (Incremental/Partial Linking)

When the GNU toolchain linker (`ld`) is run with the `-r` (or `--relocatable`) option, it performs **partial linking** (incremental linking). Instead of producing a fully bound executable, it outputs a single relocatable object file (`ET_REL`).

### 4.1. Internal Workings of `ld -r`
1. **Section Consolidation**: The linker merges identical sections (like `.text`, `.rodata`, `.data`, `.bss`) from all input object files into single, contiguous output sections.
2. **Local Symbol Resolution**: References between the input files are resolved. For example, if `module_a.cpp` calls a function defined in `module_b.cpp`, the linker resolves the call to the function's new offset inside the merged `.text` section.
3. **Relocation Preservation**: Rather than resolving and removing relocations, the linker adjusts the relocation offsets to match the merged sections and preserves the `.rel.text`, `.rel.rodata`, etc. sections.
4. **Undefined Symbols**: Any external symbols (e.g. calls to host functions like `printf`, standard math helpers, or other libraries) remain undefined (`SHN_UNDEF`) in the final output `.symtab`.
5. **Zero Virtual Addresses**: No absolute memory or virtual addresses are assigned. All section header addresses (`sh_addr`) in the generated ELF are set to `0`.

### 4.2. ASCII Layout of Partial Linking
```text
  [Input module1.o]               [Input module2.o]
  ┌────────────────┐              ┌────────────────┐
  │  .text         │              │  .text         │
  │  .rodata       │              │  .rodata       │
  │  .rel.text     │              │  .rel.text     │
  └───────┬────────┘              └───────┬────────┘
          │                               │
          └───────────────┬───────────────┘
                          │
                          ▼ (arm-none-eabi-ld -r)
              ┌────────────────────────┐
              │ Consolidated ET_REL    │
              │     (output.rel.o)     │
              ├────────────────────────┤
              │  .text (merged)        │
              │  .rodata (merged)      │
              │  .rel.text (adjusted)  │
              │  .symtab (with UNDEFs) │
              └────────────────────────┘
```

### 4.3. Key point: a `.o` is *already* ET_REL

A compiler object file (`gcc -c foo.c -o foo.o`) is **already** an `ET_REL`
object — `readelf -h foo.o` reports `Type: REL (Relocatable file)`. So `ld -r`
does **not** "make an object relocatable"; it performs an **incremental
(partial) re-link** that produces *another* `ET_REL`. Concretely, `ld -r`:

1. **Combines** all input `.o`/`.a` files into one object.
2. **Merges same-named sections** (every input's `.text` → one `.text`, all
   `.rodata` → one, …), renumbering section indices and offsets.
3. **Resolves references *between* the inputs.** A call from file A to a symbol
   defined in file B becomes an internal, resolved reference.
4. **Leaves true externals `SHN_UNDEF`.** Symbols no input defines (`printf`,
   `trace_event`, `sinf`) stay undefined for a later stage — here, the loader.
5. **Preserves relocations** — it *adjusts* `.rel.*` offsets to the merged layout
   and keeps the relocation sections, rather than applying-and-discarding them.
6. **Assigns no addresses** — every `sh_addr` stays `0`; no memory layout, no
   GOT/PLT.

It does **not** run `--gc-sections`, lay out addresses, resolve externals, or
generate a GOT/PLT — that is the `-shared` path, not `-r`.

### 4.4. Single-file vs multi-file: why we still run `-r`

| Inputs | What `ld -r` accomplishes |
|---|---|
| **one** `.o` (e.g. `relprov.o`) | essentially a **normalization/copy** — the `.rel.o` is the same `ET_REL`, same sections. |
| **many** `.o` (a real library) | the real payoff: **bundle N files into one** loadable `.rel.o` with all *intra-library* references pre-resolved, only cross-module + host externals left `UND`. |

Empirically, `ld -r c1.o c2.o -o c1c2.rel.o` turns `c2_add` (which was `UND` in
`c1.o`) into a **defined GLOBAL** in the merged object, while `trace_event`
remains `UND`. Most fixtures here are single-file, so their `-r` step is a near
no-op — but it keeps the recipe uniform and ready for multi-`.c` libraries, and
gives the loader **one** package to parse per library instead of many.

### 4.5. Significance for `scl::reloc`
By partially linking fixtures with `-r`, we hand the loader a single relocatable
package. At load time, `scl::reloc` only needs to parse that one file, allocate a
contiguous RAM block, and patch the remaining unresolved symbols against the load
set and the host.

### 4.6. Worked example — a 3-file bundle (`multi`)

`test-relocatable/fixtures-reloc/src/multi/` is a minimal demonstration: **three C++
files** with a call chain `a → b → c` and a variable shared across them, fused by
`ld -r` into a single `multi.rel.o`.

```cpp
// multi_c.cpp — leaf: the shared global + the innermost helper
extern "C" { int multi_shared = 100; }        // exported data
int c_add(int x) { return x + multi_shared; }  // internal; called by multi_b

// multi_b.cpp — middle
int c_add(int);                                // UND until -r
int b_double(int x) { return c_add(x) * 2; }   // internal; called by multi_a

// multi_a.cpp — front: exports + a constructor
extern "C" { extern int multi_shared; }        // defined in multi_c
int b_double(int);                             // UND until -r
extern "C" void trace_u32(const char*, unsigned);           // host import
__attribute__((constructor)) static void multi_ctor() { multi_shared += 5; }
extern "C" unsigned multi_entry(unsigned x) { return b_double((int)x) + multi_shared; }
extern "C" unsigned multi_get_shared() { return (unsigned)multi_shared; }
```

The build compiles each to a `.o`, then partial-links the three into one object:

```sh
arm-none-eabi-g++ -mlong-calls -fno-pic ... -c multi_a.cpp -o multi_a.o   # (×3)
arm-none-eabi-ld  -r multi_a.o multi_b.o multi_c.o -o multi.rel.o
```

**What `ld -r` produced** (`readelf` on `multi.rel.o`):

| Symbol | Binding / section | Note |
|---|---|---|
| `multi_entry`, `multi_get_shared`, `multi_bump`, `multi_shared` | GLOBAL, defined | exported to the host |
| `c_add` (`_Z5c_addi`), `b_double` (`_Z8b_doublei`) | GLOBAL, **defined in `.text`** | the `a→b→c` refs — **resolved internally** by `-r` (were `UND` in `multi_b.o`) |
| `multi_ctor`, `multi_hits` | LOCAL | the constructor and a `.bss` counter |
| `trace_u32` | **`UND`** | the only external — left for the loader/host |

So the entire internal wiring is pre-resolved; only the true host import remains
undefined. Relocations: `R_ARM_ABS32` (+ one `R_ARM_TARGET1` for `.init_array`).

**Using it** (`test-relocatable/src/classic/main.cpp`) — both a cross-file *function*
chain and a cross-file *variable*:

```cpp
const char* names[] = { "multi" };
if (load_set_of(names, 1)) {                       // one bundle, one arena
    auto multi_entry = fn<unsigned (*)(unsigned)>("multi_entry");
    auto* shared     = (std::uint32_t*)reloc::sym(g_set, "multi_shared");
    multi_entry(5);        // a->b->c + shared  ==  b_double(5)*? ... == 325
    // *shared == 105  (100, +5 from the constructor the loader ran)
}
```

On-target this reports `multi_entry(5) == 325` and `multi_shared == 105` — the
constructor ran, the shared global merged, and the three-file call chain works,
all from one `-r` package.

---

## 5. Loading and Linking Multiple Libraries Simultaneously

When an application loads multiple relocatable modules, they often have dependencies on one another. This includes:
- **Acyclic Dependencies**: Module A calls functions defined in Module B.
- **Cyclic Dependencies**: Module A calls Module B, and Module B calls Module A.

### 5.1. Coordinated Set Loading (`load_set`)
To resolve mutual and cyclic dependencies, `scl::reloc` provides a coordinated **Set Loading** API (`reloc::load_set`). Rather than linking each module in isolation, it processes the entire group in two distinct, set-wide phases:

1. **Phase 1: Set-Wide Placement**:
   - The loader validates and places all modules in the set contiguously into the RAM arena.
   - The runtime addresses of all exported symbols in every module are calculated and indexed.
2. **Phase 2: Set-Wide Eager Linking**:
   - The loader processes all relocation tables. 
   - When resolving undefined symbols (`SHN_UNDEF`), the resolver searches the symbol tables of **all other modules in the current set**.
   - If found, the absolute address is patched directly. If not, it falls back to the host resolver (`main_resolve`).
3. **Phase 3: Constructor Invocation**:
   - The static constructors (`.init_array`) of all modules in the set are executed in sequence.

### 5.2. ASCII Layout of Set Cross-Linking
```text
               RAM Arena (Contiguous Allocation)
 ┌────────────────────────────────────────────────────────┐
 │  Module A (e.g. c1)                                    │
 │    Exports: c1_call_c2  ──┐                            │
 │    Imports: c2_add      ◄─┼──────────────┐             │
 ├───────────────────────────┼──────────────┼─────────────┤
 │  Module B (e.g. c2)       │              │             │
 │    Exports: c2_add      ──┼──────────────┘             │
 │    Imports: c1_state    ◄─┼──────────────┐             │
 ├───────────────────────────┼──────────────┼─────────────┤
 │  Module C (e.g. cpp3)     │              │             │
 │    Exports: c1_state    ──┘              │             │
 │    Imports: c2_step_down ────────────────┘             │
 └────────────────────────────────────────────────────────┘
```

### 5.3. Usage Code Snippet
```cpp
const char* names[] = { "c1", "c2", "cpp3" };
if (load_set_of(names, 3)) {
    // Resolve entry points across the cyclic set
    auto c1_call_c2 = fn<unsigned int (*)(unsigned int)>("c1_call_c2");
    auto cpp3_recurse = fn<unsigned int (*)(unsigned int)>("cpp3_recurse");

    // Execute functions directly with zero runtime overhead
    unsigned int r1 = c1_call_c2(5);
    unsigned int r2 = cpp3_recurse(6);
}
```

---

## 6. Minimum Host Requirements (At the Minima)

For a host application's `main()` function to load and execute one or more relocatable (`ET_REL`) modules, it must perform the following **five minimum steps**:

**Step 1: Implement a Host Symbol Resolver (`loader::symbol_source`)**
Write a free function `result<uint32_t>(void* ctx, string_view name)` mapping the
symbols modules import (`printf`, float wrappers, GPIO/LED, …) to their runtime
addresses, and wrap it in a `loader::symbol_source{ fn, ctx }`. (It is a callback
+ context pair, **not** a virtual class — see the reference manual §2.1.)

**Step 2: Read the module bytes into memory**
The loader takes **in-memory spans**, not a byte-source object. Read the whole
`.rel.o` into a buffer (over semihosting `semihost_open/flen/read`, from flash, or
an SD card) and form a `std::span<const std::byte>`.

**Step 3: Allocate a Relocation RAM Arena**
Declare a contiguous `std::byte` array (local/global) sized for the set's
`SHF_ALLOC` sections; pass it as `std::span<std::byte>`.

**Step 4: Call the Loader API**
- Single module: `reloc::load(image, arena, resolver)`.
- Multiple modules (cyclic deps): `reloc::load_set(images, arena, resolver)`,
  where `images` is `std::span<const std::span<const std::byte>>`.

**Step 5: Resolve and Execute Entry Points**
Get an export's address with `reloc::sym(set, "name")` (returns a `std::uint32_t`,
0 if absent), cast to the function-pointer type, and call it directly.

---

## 7. Loader Architecture & Implementation Details

The `scl::reloc` loader parses standard ELF32 relocatable objects and loads them in three distinct phases:

```
┌─────────────────────────────────────────────────────────┐
│                    Loading Process                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Phase 1: Section Placement                             │
│  [ELF File] ──► Read Headers ──► CONTIGUOUS RAM ARENA   │
│                                                         │
│  Phase 2: Eager Symbol Resolution & Patching             │
│  [Symbols] ──► Main Resolver ──► Patch ABS32/THM_CALL   │
│                                                         │
│  Phase 3: Constructor Invocation                        │
│  [ctors] ──► Execute .init_array Function Pointers      │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 7.1. Phase 1: Section Placement
The loader traverses the Section Header Table of the `.rel.o` files. It identifies allocatable sections (marked with the `SHF_ALLOC` flag, such as `.text`, `.rodata`, `.data`, and `.bss`) and maps them into a contiguous RAM arena:
- **Alignment**: Every section is aligned in RAM according to its `sh_addralign` constraint.
- **BSS Zeroing**: `.bss` sections are zero-initialized in RAM.
- **Address Mapping**: The base address of each section in RAM is saved in `m.sec_base[section_index]`.

### 7.2. Phase 2: Symbol Resolution & Patching
The loader iterates through the relocation sections (`.rel.text`, `.rel.rodata`, etc.). For each relocation entry:
1. **Symbol Lookup**: The symbol associated with the relocation is fetched.
   - If the symbol is defined locally inside the module, its address is computed as `sec_base[shndx] + st_value`.
   - If the symbol is undefined (`SHN_UNDEF`), the loader resolves it across other modules in the current load set. If still unresolved, it queries the host application's callback resolver (`main_resolve`).
2. **Thumb-Mode Adjustment**: If the symbol is of type `STT_FUNC` (function), its address is adjusted by setting the least significant bit (LSB) to `1`. This instructs the Cortex-M processor (which only executes in Thumb state) that the target address is a Thumb function.
3. **Instruction Patching**: The address is written to the instruction offset.

### 7.3. Phase 3: Constructor Invocation
Once all relocations are patched, the loader locates the `.init_array` section of each module, which contains a list of function pointers to static constructors. The loader iterates through these pointers and invokes each constructor directly:
```cpp
void (*ctor)() = reinterpret_cast<void (*)()>(fp | 1u);
ctor();
```

---

## 8. Detailed Relocation Math & Thumb-2 BL Patching

The ARM Cortex-M architecture uses the **Thumb-2** instruction set. Thumb-2 instructions are 16-bit or 32-bit. Branch instructions like Branch-with-Link (`BL`) use a 32-bit encoding where the offset is scrambled across several bitfields.

### 8.1. R_ARM_ABS32 Relocation
This is the simplest relocation type. It patches a direct 32-bit pointer.
- **Math**: `*P = S + A`
- Where:
  - `P` is the address of the 4-byte slot being patched.
  - `S` is the absolute address of the resolved symbol.
  - `A` is the addend value already present in the slot.

### 8.2. R_ARM_THM_CALL (Thumb-2 BL Instruction)
The `BL` instruction encodes a PC-relative offset. The instruction consists of two 16-bit halfwords:

```
Halfword 1:  1 1 1 1 0 [ S ] [      imm10      ]
Halfword 2:  1 1 1 1 1 [ J1] [ J2] [    imm11   ]
```

To compute the final target offset, the loader must decode these bits:
1. Extract `S` (Sign bit), `imm10`, `J1`, `J2`, and `imm11`.
2. Compute intermediate bits `I1` and `I2`:
   - `I1 = ~(J1 ^ S) & 1`
   - `I2 = ~(J2 ^ S) & 1`
3. Assemble the 25-bit sign-extended addend `A`:
   - `A = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)`
4. Compute the target PC-relative offset:
   - `off = (SymbolAddress - P) + A`
5. Re-encode the new offset back into the instruction halfwords and write them back to RAM.

---

## 9. Classic Test Suite Porting (Step-by-Step)

The classic dynamic/static loader test suite (`test-static/test-static/src/main.cpp`) was
ported to the relocatable loader under `test-relocatable/src/classic/main.cpp`.

### 9.1. Eliminating STM32 HAL Dependencies
The original test code relied on a heavy STM32 HAL library configuration. To build a lightweight, freestanding `-nostdlib` application, we stubbed out these dependencies using the simple register-level `board.c` API:

```cpp
class led {
public:
    led(unsigned int port, unsigned int bit, bool activeLow) {}
    void power_up() {}
    void turn_on() { led_set(1); }
    void turn_off() { led_set(0); }
    void toggle() { led_toggle(); }
};

class timer {
public:
    using ticks_t = uint32_t;
    static constexpr ticks_t FREQUENCY_HZ = 1000;
    void start() {}
    void sleep(ticks_t ticks) { delay_ms(ticks); }
};
```

### 9.2. Custom LibC/LibM Formatting Shims
To resolve external symbols used by `mylib1` and `mylib2` at load time, we implemented a custom mini-logger inside `main.cpp`:
- **`trace_printf`**: A lightweight stack-only formatter supporting `%u`, `%d`, `%x`, and `%s` format tags.
- **`printf`**: A wrapper prefixing prints with `[printf]` and routing them to the semihosting console.
- **`strcat`**: Custom char buffer copy routine.
- **`log`**: Double-precision wrapper invoking hardware float-level `logf`.
- **`__aeabi_d2f` / `__aeabi_f2d`**: Standard float-double translation routines.

### 9.3. Relocation Testing Setups
The classic runner loads **four** module sets in turn (`load_set_of`, reusing
the same three image buffers + arena each time). Every block is written in classic
demo style — it loads, calls the entry points, and logs the observed values with
`log_info` (`[INFO] …`); there is **no PASS/FAIL bookkeeping**:
1. **relprov + relmod (Acyclic)**: cross-module ABS32 calls and absolute variable reads.
2. **c1 + c2 + cpp3 (Cyclic C++)**: cyclic inter-library references, static constructor executions, and C++ class instance methods using hardware floating-point math.
3. **libprov + libcons (Inter-Library)**: `libcons` calls resolved to the target provider `libprov`.
4. **mylib1 + mylib2 (Classic Suite)**: the classic cases ported from `test-static`.

The runner also exercises reloc-specific behaviour — two independent instances of the
same module (separate `.bss` at different addresses), reload (`.bss`/ctor reset),
and the loader error contract (unresolved import, non-ELF, out-of-memory) — plus
the flatfs demos below. All of it logs its results the same demo way and ends with
a plain `==== classic demo complete ====` line rather than a pass/fail tally.

### 9.4. flatfs (I) — modules embedded in FLASH (no semihosting)

TESTs 1–4 read each `.rel.o` over **semihosting** at runtime (`RELOC_LIB_DIR`).
The **flatfs** demos show the real deployment shape instead: the module lives in
the firmware's own **flash**, and is loaded from there — no host, no filesystem.
There are two variants: this one embeds each module as its own flash array
(simplest); [§9.5](#95-flatfs-ii--a-real-flash-archive-loaded-by-name) packs
them into one real archive addressed by name.

**The fixtures.** `fixtures-reloc/src/flatfs/{fsmod1,fsmod2}.c` are two tiny,
self-contained plugins (they import only the host `trace_u32`, so each loads
standalone). `fsmod1` keeps `.data` state (`fs1_state`) and a constructor;
`fsmod2` is stateless.

**Embedding.** Each `.rel.o` is turned into a C array by
`test-relocatable/tools/embed-blob.py` and compiled into the classic firmware, so it
lands in `.rodata` (flash):

```c
// generated: fsmod1_blob.c
__attribute__((aligned(8))) const unsigned char fsmod1_rel_o[] = { 0x7f,0x45,0x4c,0x46, ... };
const unsigned fsmod1_rel_o_len = 1440;
```
The CMake wiring (`add_custom_command` + `target_sources` on
`scl-fw-classic-reloc`) runs `embed-blob.py` after the fixtures build.

**Loading.** Because flash is memory-mapped, a module is simply a `std::span`
over its flash address — the loader reads the ELF straight from flash and copies
only the `SHF_ALLOC` sections into the RAM arena. No copy of the whole file, no
`byte_source`:

```cpp
extern "C" { extern const unsigned char fsmod1_rel_o[]; extern const unsigned fsmod1_rel_o_len; }

std::span<const std::byte> img[1] = {
    { reinterpret_cast<const std::byte*>(fsmod1_rel_o), fsmod1_rel_o_len } };
auto r = reloc::load_set(img, g_arena, resolver);      // load from flash
auto run = fn<unsigned(*)(unsigned)>("fsmod1_run");
run(5);                                                 // call the flash-loaded plugin
```

This variant links each module as a *separate* array, so there is no directory
and no lookup — the code names `fsmod1_rel_o`/`fsmod2_rel_o` directly. On-target
output:

```text
== flatfs: modules loaded from FLASH ==
  [fsmod1] ctor state=1000
[INFO] fsmod1 @flash(1440 B): id=0xf1  run(5)=1005  run(5)=1010
[INFO] fsmod2 @flash(1280 B): id=0xf2  square(9)=81
```

The demo is written in classic (`log_info`) style — like the rest of the runner it
loads, calls, and logs; there is no PASS/FAIL.

### 9.5. flatfs (II) — a real flash archive, loaded by name

The blob variant above is really *"modules baked into the image"*, not a
filesystem: there is no directory and no by-name lookup. This variant is the
genuine **flat filesystem in flash** — the ET_REL analogue of
`test-dynamic/test-dynamic-nolib/test3` (which provisions an on-chip archive over
semihosting), but simpler, because the eager/absolute model needs no GOT and
flash is directly addressable.

**The archive format.** `test-relocatable/tools/pack-flatfs.py` packs the same
`fsmod1.rel.o` + `fsmod2.rel.o` into **one** flash-resident blob with a
directory (little-endian, data 8-aligned):

```text
offset 0   "FLFS"                              magic
       4   u32   count                         number of entries
       8   count × { char name[16];           entry: NUL-padded name
                     u32  offset;                     byte offset from archive start
                     u32  size; }                     module length
       …   (pad to 8)
       …   data: each module's .rel.o, each padded to 8
```

It is emitted as a single `const` array in `.rodata` (flash), exactly like the
blobs — but one archive for all modules. The CMake wiring mirrors §9.4: an
`add_custom_command` runs `pack-flatfs.py` after the fixtures build, and
`target_sources` links `flatfs_archive.c` into `scl-fw-classic-reloc`.

**Mount + open by name.** Because flash is memory-mapped, mounting is just
recording the base pointer, and `open()` returns a zero-copy `std::span`
straight into flash — no staging, no copy of the whole archive:

```cpp
struct flatfs {
    const unsigned char* base; unsigned len;
    bool     mount(const unsigned char* p, unsigned n);   // check "FLFS" magic
    unsigned count() const;                                // read u32 at +4
    std::span<const std::byte> open(std::string_view name) const;  // scan directory
};

flatfs fs;
fs.mount(flatfs_archive, flatfs_archive_len);
auto blob = fs.open("fsmod1");                 // span into flash, by NAME
std::span<const std::byte> img[1] = { blob };
auto r = reloc::load_set(img, g_arena, resolver);   // same loader path as §9.4
```

`open()` walks the fixed-size directory, matches the NUL-padded `name[16]`
field, and returns `{ base + offset, size }`. The resulting span feeds the
*identical* `reloc::load_set` path — the only difference from §9.4 is *how the
bytes are addressed* (by name through a directory vs. by a linker symbol).
On-target output:

```text
== flatfs: real archive mounted from FLASH (load by name) ==
[INFO] mounted flatfs archive @flash(2776 B): 2 modules
  [fsmod1] ctor state=1000
[INFO] opened 'fsmod1' (1440 B) from archive: id=0xf1
  [fsmod2] ctor id=242
[INFO] opened 'fsmod2' (1280 B) from archive: id=0xf2
```

Also classic (`log_info`) style — no PASS/FAIL. Compared to the blobs, the
archive adds a real by-name lookup and a single packing step, and is the
foundation for an *updatable* store (rewrite the archive's flash sector without
relinking the firmware) — the practical "filesystem in flash".

---

## 10. Execution Logs (Classic Reloc Suite)

Below is the semihosting output generated when running `scl-fw-classic-reloc.hex` on the WeAct BlackPill STM32F411CE board:

```text
=== flash + run classic (semihosting console follows) ===
flashing: test-relocatable/build/scl-fw-classic-reloc.hex
semihosting is enabled
Hello Arm World!

== relprov + relmod (cross-module ABS32) ==
[prov] ctor(8721)
[relmod] ctor(193)
[relmod] rel_compute(100)
[FM] fm_callback(193)
  relmod helper  = 492
  relmod prov    = 13173
  relmod fmcb    = 449
[INFO] rel_compute(100)=14307  rel_read_data=0x1100  prov_state=1

== c1 + c2 + cpp3 (cyclic set, load_set) ==
[c1] c1_ctor(bss)(4369)
[c2] c2_ctor(bss)(8738)
[cpp3] Banner.ctor(made)(768)
[c1] c1_call_c2(5)
[c2] c2_add(4357)
c1: c2_add -> 13096
[c1] c1_call_c2(5)
[c2] c2_add(4357)
c1: c2_add -> 13097
[INFO] c1_call_c2(5)=17467  (2nd call=17469, .bss persists)  c1_state=3
[c1] c1_use_fm(fm_var)(170)
c1: fm_info.id = 241
[INFO] c1_use_fm()=611 (reads FM data)
[cpp3] cpp3_recurse(6)
[c2] c2_step_down(5)
[cpp3] cpp3_recurse(4)
[c2] c2_step_down(3)
[cpp3] cpp3_recurse(2)
[c2] c2_step_down(1)
[cpp3] cpp3_recurse(0)
[INFO] cpp3_recurse(6)=21
[cpp3] cpp3_method(100)
cpp3.Mathy.compute = 1432
[cpp3] cpp3_method(100)
cpp3.Mathy.compute = 1432
[INFO] cpp3_method(2,100)=1432 (2nd call=1432, libm)

== libprov + libcons ==
[INFO] cons_call(10)=111

== multi (3-file ld -r bundle) ==
  [multi] ctor multi_shared=105
  [multi] entry x=5
[INFO] multi_entry(5)=325 (a->b->c across 3 files)  get_shared()=105  shared-as-data=105

== two independent instances (same module, 2 arenas) ==
  [multi] ctor multi_shared=105
  [multi] ctor multi_shared=105
[INFO] A@0x200028a4 hits=2   B@0x200008a4 hits=1 (independent .bss)

== reload resets .bss/ctor state ==
  [multi] ctor multi_shared=105
  [multi] ctor multi_shared=105
[INFO] hits before reload=3  after=0 (.bss cleared)  shared=105 (ctor re-ran)

== loader error contract (negative paths) ==
  load_set FAILED with error: 7
[INFO] unresolved import -> load fails (no crash)
[INFO] garbage buffer -> err=3 (NOT_ELF=3)
[INFO] tiny arena -> err=9 (OUT_OF_MEMORY=9)

== mylib1 + mylib2 ==
[printf] mylib1 constructor: bb=40
[printf] mylib1 second constructor
[printf] mylib2 constructor: bbb=50
[INFO] library_function(7)=30  lib2(7)=23  lib3(7)=160
[INFO] aa=20 bb=65 aaa=20 bbb=40
[INFO] g(7)=92  fff(10)=70  gg(7)=106
[INFO] retstr=Hello mylib1!  retstr_=Hello from mylib1!
[INFO] Struct: 1000 xxx 2 3 %f
[INFO] Struct from lib: 15_ 120 1235 30 %f
[INFO] Struct ret: 15_1000_12345xxx 12 000_12345xxx 6 %f
[INFO] callback_f(7) = 281
[INFO] fff(10) = 70
[printf] mylib2 ...TOTO...Help....

[printf] mylib2 ...55.
[INFO] xxx() = 60

== flatfs: modules loaded from FLASH ==
  [fsmod1] ctor state=1000
[INFO] fsmod1 @flash(1440 B): id=0xf1  run(5)=1005  run(5)=1010
  [fsmod2] ctor id=242
[INFO] fsmod2 @flash(1280 B): id=0xf2  square(9)=81

== flatfs: real archive mounted from FLASH (load by name) ==
[INFO] mounted flatfs archive @flash(2776 B): 2 modules
  [fsmod1] ctor state=1000
[INFO] opened 'fsmod1' (1440 B) from archive: id=0xf1
  [fsmod2] ctor id=242
[INFO] opened 'fsmod2' (1280 B) from archive: id=0xf2

==== classic demo complete ====
Second 1
Second 2
Second 3
Second 4
Second 5
Second 6
```

---

## 11. Firmware Build Inputs (Translation Units & Paths)

`test-relocatable/CMakeLists.txt` builds **two** separate Cortex-M4 firmwares, each with
its own `main.cpp`:

| Firmware | Entry `main.cpp` | Style |
| --- | --- | --- |
| `scl-fw-reloc` | `test-relocatable/src/main.cpp` | **check() battery** — `check()`, `g_pass`/`g_fail`, `== TEST N ==`, `BATTERY SUMMARY` |
| `scl-fw-classic-reloc` | `test-relocatable/src/classic/main.cpp` | **classic demo** — `log_info`, no PASS/FAIL (§9–§10) |

Paths below are relative to the repo root (`shared-lib-loaders/`); the shared
platform layer lives under `PLAT = test/platforms/board`.

### 11.1. Sources compiled & linked into each firmware

Both firmwares link the *same* platform + loader translation units; they differ
only in their `main.cpp` and (for the classic app) three generated flatfs sources.

**Shared by both — platform layer** (reused from the ET_DYN test tree):

| File | Path | Role |
| --- | --- | --- |
| `startup.c` | `test/platforms/board/src/startup.c` | vectors + reset (copy `.data`, zero `.bss`, FPU, call `main`) |
| `semihost.c` | `test/platforms/board/src/semihost.c` | `trace_puts/putc` + `semihost_open/flen/read/close` |
| `board.c` | `test/platforms/board/src/board.c` | `board_init` / SysTick |
| `crt.cpp` | `test/platforms/board/src/crt.cpp` | `operator new/delete` + `malloc/free` (static heap) |
| `libc-shim.c` | `test/platforms/board/src/libc-shim.c` | `memcpy/memset/…` (freestanding, `-nostdlib`) |

**Shared by both — the `scl::reloc` loader library** (`target_link_libraries(… scl::reloc)`, compiled from source into each image):

| File | Path | Role |
| --- | --- | --- |
| `reloc-loader.cpp` | `src/reloc/reloc-loader.cpp` | `reloc::load_set` public API |
| `reloc-applier.cpp` | `src/reloc-applier.cpp` | relocation math + Thumb-2 BL patching (§8) |
| `symbol-resolver.cpp` | `src/symbol-resolver.cpp` | symbol table / cross-module resolution |
| `region-allocator.cpp` | `src/region-allocator.cpp` | arena / section placement |
| `lazy-resolve.cpp` | `src/lazy-resolve.cpp` | lazy-resolution support |
| `dynamic-info.cpp` | `src/dynamic-info.cpp` | dynamic/section info parsing |
| `log.c` | `src/log.c` | loader logging |
| `elf-image.cpp` | `common-elf-reader/src/elf-image.cpp` | ELF32 reader/parser (git submodule) |

**`scl-fw-reloc` only:**

| File | Path | Role |
| --- | --- | --- |
| `main.cpp` | `test-relocatable/src/main.cpp` | the check()/PASS-FAIL battery entry point |

**`scl-fw-classic-reloc` only:**

| File | Path | Role |
| --- | --- | --- |
| `main.cpp` | `test-relocatable/src/classic/main.cpp` | the classic/demo entry point (§9) |
| `fsmod1_blob.c` | `test-relocatable/build/blobs/fsmod1_blob.c` | *generated* — `fsmod1.rel.o` embedded in FLASH (§9.4) |
| `fsmod2_blob.c` | `test-relocatable/build/blobs/fsmod2_blob.c` | *generated* — `fsmod2.rel.o` embedded in FLASH (§9.4) |
| `flatfs_archive.c` | `test-relocatable/build/blobs/flatfs_archive.c` | *generated* — fsmod1+fsmod2 packed archive (§9.5) |

The three generated sources are produced at build time under `test-relocatable/build/blobs/`
by `tools/embed-blob.py` and `tools/pack-flatfs.py` from the fixtures — they are
not checked in.

### 11.2. Headers included by `main.cpp`

Both `main.cpp` files include the identical set:

| Include | Resolved path | Provided via |
| --- | --- | --- |
| `"board.h"` | `test/platforms/board/include/board.h` | `target_include_directories(… PLAT/include)` |
| `"semihost.h"` | `test/platforms/board/include/semihost.h` | `PLAT/include` |
| `"reloc/reloc-loader.hpp"` | `include/reloc/reloc-loader.hpp` | `scl::reloc` INTERFACE includes |
| `<cstddef> <cstdint> <span> <string_view>` | arm-none-eabi libstdc++ | toolchain headers |

`reloc-loader.hpp` in turn pulls the common-elf-reader interface headers from
`common-elf-reader/include/loader/` (`error.hpp`, `elf-image.hpp`,
`elf-types.hpp`, `byte-source.hpp`, …).

### 11.3. Link-time inputs (both firmwares)

| Input | Path / source | Role |
| --- | --- | --- |
| Linker script | `test-relocatable/platform/reloc.ld` | NON-PIC firmware layout (`-T`) |
| `libm.a` | multilib (`-print-file-name=libm.a`) | `sinf/cosf/logf/sqrtf` for `cpp3` |
| `libgcc.a` | multilib (`-print-file-name=libgcc.a`) | compiler helper routines |

Compile flags: `-fno-pic -fno-pie -Os -ffunction-sections -fdata-sections`
(+ `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` for
C++). Link flags: `-nostdlib -nostartfiles -Wl,--gc-sections`.

### 11.4. Runtime fixtures (loaded, *not* linked into `main`)

Both firmwares open ET_REL fixtures at runtime; they are built by `fixtures-reloc/`
into `.rel.o` and reached through the absolute path baked in at compile time as
`RELOC_LIB_DIR = test-relocatable/fixtures-reloc/build` (opened over semihosting):
`relprov, relmod, c1, c2, cpp3, libprov, libcons, multi, mylib1, mylib2`. The
classic app additionally loads `fsmod1`/`fsmod2` straight from FLASH via the
embedded blobs/archive above — no semihosting.

---

## 12. Fixture Formats & Sizes (ET_DYN vs ET_REL)

The same plugin sources are built two ways: as **ET_DYN `.so`** shared objects for
the `test/` loaders (`test-static`, `test-dynamic-*`, `test-overlay`) and as
**ET_REL `.rel.o`** relocatable objects for the `test-relocatable/` loaders (classic +
battery). The `.rel.o` form is markedly smaller — it carries only sections +
relocations + a symtab, with no program headers, `.dynamic`,
`.dynsym/.dynstr/.hash`, or GOT/PLT.

Sizes below are the on-disk bytes of the built artifacts (arm-none-eabi-gcc
15.2.1); they shift slightly with toolchain version. The `.so` set lives in
`test/fixtures/`; the `.rel.o` set is built into `test-relocatable/fixtures-reloc/build/`.

> **File size vs runtime RAM — and why the reloc loader wins both here.** These
> tables measure *on-disk/FLASH* size, where `.rel.o` wins by omitting the
> dynamic-linking metadata (program headers, `.dynamic`, `.dynsym/.dynstr/.hash`,
> GOT/PLT). Runtime RAM is a *separate* axis (the §3 tradeoff) — but on this
> MMU-less target it points the **same way**: neither loader executes in place
> from flash, so *both* copy the entire module image into a RAM arena. The reloc
> loader copies only the `SHF_ALLOC` sections (`.text/.rodata/.data/.bss`)
> contiguously; the ET_DYN loader copies the whole `PT_LOAD` span — the same code
> and data **plus** the resident GOT/PLT and `.dynsym/.dynstr/.hash`, plus
> per-segment alignment. So the larger `.so` is also the **heavier RAM tenant**.
> The §3 "RAM Consumption" drawback is real only against statically-linked
> firmware running from flash, **not** against the ET_DYN loader in this repo.

### 12.1. Object type, not file extension

Each loader accepts exactly one ELF object **type**, and decides by reading the
ELF header's `e_type` field — **not** the file name. The `.rel.o` / `.so`
suffixes are this repo's naming convention only:

| | `scl::reloc` (test-relocatable) | dynamic loader (test/) |
| --- | --- | --- |
| Accepts | **`ET_REL`** — `Type: REL (Relocatable file)` | **`ET_DYN`** — `Type: DYN (Shared object file)` |
| Conventional name | `.rel.o` (a `.o`, or an `ld -r`-merged object) | `.so` |
| Produced by | compile `-c` (+ optional `ld -r` to bundle) | link `-shared -fPIC` |

```text
$ arm-none-eabi-readelf -h c1.rel.o | grep Type
  Type:  REL (Relocatable file)          # loaded by scl::reloc
$ readelf -h c1.so | grep Type
  Type:  DYN (Shared object file)        # loaded by the dynamic loader
```

Consequences:

* Renaming `c1.rel.o` → `c1.so` does **not** change what loads it: `scl::reloc`
  still accepts it (still `ET_REL`) and the dynamic loader still rejects it.
* `scl::reloc` does **not** accept an *arbitrary* `.o` — the fixtures are built
  `-fno-pic -mlong-calls` and use only the `R_ARM_*` relocations the applier
  implements (ABS32 + Thumb-2 `BL`, §8). A stock PIC object with GOT-relative or
  otherwise unsupported relocations fails to load.
* An `ET_DYN` `.so` handed to `scl::reloc` (or an `ET_REL` object handed to the
  dynamic loader) is rejected on the type check rather than mis-loaded.

The size tables below therefore pair each test tree with the object type it
actually loads: **`.so` bytes for the `test/` dynamic/static loaders**, **`.rel.o`
bytes for the `test-relocatable/` loaders**.

### 12.2. ET_DYN `.so` fixtures — `test/` dynamic/static loaders (`test/fixtures/`)

| Fixture | Size (bytes) | Used by |
| --- | ---: | --- |
| `libsh1.so` | 52,496 | `test-static` |
| `libsh2.so` | 42,660 | `test-static` |
| `mylib1.so` | 8,332 | `test-dynamic-nolib`, `test-dynamic-newlib` |
| `cpp3.so` | 5,372 | `test-dynamic-nolib`, `test-overlay` |
| `c2.so` | 5,264 | `test-dynamic-nolib`, `test-overlay` |
| `libcppeh.so` | 4,848 | `test-dynamic-nolib` |
| `c1.so` | 4,780 | `test-dynamic-nolib`, `test-overlay` |
| `mylib2.so` | 4,600 | `test-dynamic-nolib` |
| `libcppsh.so` | 4,068 | `test-dynamic-nolib` |
| `ehx_catch.so` | 3,148 | `test-dynamic-nolib` |
| `libcons.so` | 3,004 | `test-dynamic-nolib` |
| `libprov.so` | 2,676 | `test-dynamic-nolib` |
| **Total** | **141,248** | |

### 12.3. ET_REL `.rel.o` fixtures — `test-relocatable/` loaders (`test-relocatable/fixtures-reloc/build/`)

| Fixture | Size (bytes) | Used by |
| --- | ---: | --- |
| `cpp3.rel.o` | 3,520 | battery, classic |
| `mylib1.rel.o` | 3,556 | battery, classic |
| `c2.rel.o` | 2,848 | battery, classic |
| `c1.rel.o` | 2,392 | battery, classic |
| `mylib2.rel.o` | 2,200 | battery, classic |
| `multi.rel.o` | 2,124 | classic |
| `relmod.rel.o` | 2,096 | battery, classic |
| `relprov.rel.o` | 1,540 | battery, classic |
| `fsmod1.rel.o` | 1,440 | classic (flatfs, in FLASH) |
| `fsmod2.rel.o` | 1,280 | classic (flatfs, in FLASH) |
| `libcons.rel.o` | 880 | classic |
| `libprov.rel.o` | 768 | classic |
| **Total** | **24,644** | |

`battery` = `scl-fw-reloc` (`src/main.cpp`), `classic` = `scl-fw-classic-reloc`
(`src/classic/main.cpp`).

### 12.4. Same fixture, both formats

For the seven plugins built in *both* worlds, the ET_REL object is roughly half
the ET_DYN shared library (or less for the tiny provider/consumer pair):

| Fixture | `.so` (ET_DYN) | `.rel.o` (ET_REL) | `.rel.o` ÷ `.so` |
| --- | ---: | ---: | ---: |
| `c1` | 4,780 | 2,392 | 0.50 |
| `c2` | 5,264 | 2,848 | 0.54 |
| `cpp3` | 5,372 | 3,520 | 0.66 |
| `libcons` | 3,004 | 880 | 0.29 |
| `libprov` | 2,676 | 768 | 0.29 |
| `mylib1` | 8,332 | 3,556 | 0.43 |
| `mylib2` | 4,600 | 2,200 | 0.48 |
| **Total** | **34,028** | **16,164** | **0.48** |

Across these seven, ET_REL is ~48 % of the ET_DYN size — the saving is the whole
dynamic-linking apparatus the eager/absolute loader does not need.

### 12.5. Runtime RAM footprint (measured)

The file-size gap above (§12.4) *understates* the runtime difference. What each
loader actually spends is the size of the in-RAM image it builds — measured here
exactly as the loaders compute it:

* **ET_REL arena** = the loader's own `module::used` counter — the SHF_ALLOC
  section bytes (`.text/.rodata/.data/.bss`) it copies for that module, summed as
  it places them in `place_module()` (`src/reloc/reloc-loader.cpp`). The arena
  additionally rounds each section up to its `sh_addralign` (≤2 B of padding here,
  not counted in `used`).
* **ET_DYN image** = `image_size()` = the highest `PT_LOAD` end address
  (`p_vaddr + p_memsz`), the buffer the dynamic loader allocates
  (`common-elf-reader/src/elf-image.cpp`).

The ET_REL column is **confirmed on hardware** — the classic firmware prints each
module's `module::used` (see the trace below); the ET_DYN column is `image_size()`
computed from the shipped `.so` (the same value the dynamic loader allocates).

| Fixture | ET_REL arena (`.rel.o`) | ET_DYN image (`.so`) | ET_DYN ÷ ET_REL |
| --- | ---: | ---: | ---: |
| `c1` | 443 | 2,036 | 4.6× |
| `c2` | 579 | 2,312 | 4.0× |
| `cpp3` | 918 | 6,604 | 7.2× |
| `libcons` | 16 | 900 | 56× |
| `libprov` | 4 | 784 | 196× |
| `mylib1` | 716 | 4,100 | 5.7× |
| `mylib2` | 320 | 1,820 | 5.7× |
| **Total** | **2,996** | **18,556** | **6.2×** |

On this MMU-less target both images live fully in RAM, so the ET_DYN module is
the heavier tenant by **~6×** (vs. ~2× on disk) — the extra RAM is the resident
GOT/PLT + `.dynsym/.dynstr/.hash` + `.dynamic` plus per-`PT_LOAD` alignment gaps.
That apparatus is a near-fixed floor: `libprov` is a single 4-byte function, yet
as a `.so` it still costs 784 B of RAM (196×). (Figures are the loadable image
only; both loaders also keep a small fixed per-module bookkeeping struct in host
RAM, outside these arenas.)

**Live confirmation (WeAct BlackPill STM32F411CE, semihosting console).** The classic firmware
loads each set and prints `module::used`; the values match the ET_REL column
above to the byte:

```text
== RAM arena footprint (module::used bytes) ==
[INFO] c1 arena=443 B
[INFO] c2 arena=579 B
[INFO] cpp3 arena=918 B
[INFO] libprov arena=4 B
[INFO] libcons arena=16 B
[INFO] mylib1 arena=716 B
[INFO] mylib2 arena=320 B
```

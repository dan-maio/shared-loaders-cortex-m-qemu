# Shared Static Rework — Manual

## Table of Contents

1. [Motivation](#1-motivation)
2. [What Problem This Solves](#2-what-problem-this-solves)
3. [Architecture Overview](#3-architecture-overview)
4. [How It Works — Step by Step](#4-how-it-works--step-by-step)
5. [Creating a Shared Library](#5-creating-a-shared-library)
6. [Configuring the Main Application](#6-configuring-the-main-application)
7. [Linking and Loading](#7-linking-and-loading)
8. [The Initialisation Data Block](#8-the-initialisation-data-block)
9. [Relocation and Patching](#9-relocation-and-patching)
10. [Weak Symbols](#10-weak-symbols)
11. [Constructors](#11-constructors)
12. [Segment Permissions on Cortex-M (RWX)](#12-segment-permissions-on-cortex-m-rwx)
13. [Production-Grade Features](#13-production-grade-features)
14. [Test Suite and Library Reference](#14-test-suite-and-library-reference)
15. [Porting to a Different STM32 / Cortex-M](#15-porting-to-a-different-stm32--cortex-m)
16. [Troubleshooting](#16-troubleshooting)
17. [Reference: ELF32 API Reference](#17-reference-elf32-api-reference)
18. [Reference: ELF32 Structures in Use](#18-reference-elf32-structures-in-use)
19. [Reference: Linker Scripts](#19-reference-linker-scripts)
20. [Loading C++ Shared Libraries (PLT and `.rel.plt`)](#20-loading-c-shared-libraries-plt-and-relplt)

---

## 1. Motivation

On desktop operating systems (Linux, Windows, macOS), shared libraries (`.so`, `.dll`) are loaded
by the kernel or a userspace dynamic linker at process start. The dynamic linker resolves
undefined symbols, fixes up relocations, and calls constructors — all automatically.

On **bare-metal ARM Cortex-M** there is no OS, no kernel, no `ld.so`. The conventional approach
is to link everything into a single monolithic image at build time. This works, but it has
significant limitations:

- **No hot-plug / late loading** — you cannot load a new module without re-flashing the whole
  device.
- **Vendor binary distribution** — if you want to ship a closed-source library to customers,
  they must have your full source tree or you must ship pre-linked object files for every
  possible flash address.
- **Modular firmware** — teams working on different subsystems (e.g. sensor processing,
  communications, UI) cannot release their modules independently.
- **Field updates** — a bug fix in a single module requires re-flashing the entire 512 KB
  flash, which is slow and risky over e.g. UART or BLE.

This project provides a **minimal, embedded-friendly ELF32 shared-library loader** that runs
on Cortex-M4, without an MMU, without an OS, and with only the RAM and flash resources
available on a typical STM32F411. It handles:

- Loading ELF segments from `.so` files into RAM
- Relocation patching (`R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`, `R_ARM_COPY`)
- Global and weak symbol resolution
- Constructor execution (`.init_array`)
- GOT patching for position-independent code

---

## 2. What Problem This Solves

### 2.1 Late binding of firmware modules

Imagine a sensor node that supports multiple radio modules (LoRa, BLE, Wi-Fi). Instead of
compiling all three radio stacks into the firmware (bloating flash and RAM), you ship each
radio as a separate `.so` file in an external flash filesystem. At boot, the firmware loads
only the `.so` for the radio that is physically present.

### 2.2 Closed-source library distribution

A third-party vendor provides an algorithm (e.g. audio codec, cryptolib, motor-control) as
`libalgo.so`. You drop it into your build tree, link the main firmware against it, and the
loader takes care of resolving symbols at runtime — no need to ship sources or worry about
link-time address conflicts.

### 2.3 Over-the-air partial updates

When a bug is found in a shared library, only that `.so` needs to be replaced in the external
flash filesystem. The main bootloader stays untouched. This reduces OTA payload size from
512 KB to e.g. 10 KB.

### 2.4 Development and debugging

During development, libraries can be compiled, loaded, and tested independently without
re-flashing the entire board. The application can print the GOT contents, relocation tables,
and segment information to verify correctness — all visible over semihosting.

---

## 3. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      MCU (STM32F411RE)                       │
│                                                              │
│  ┌──────────────── FLASH (0x08000000) ────────────────────┐  │
│  │  .isr_vector     — interrupt vectors                   │  │
│  │  .inits          — init array + _reloc block           │  │
│  │  .text           — main firmware code                  │  │
│  │  .dynsym         — dynamic symbol table (main)         │  │
│  │  .dynstr         — dynamic string table (main)         │  │
│  │  .hash           — ELF hash table (main)               │  │
│  │  .rel.dyn        — relocation entries (main)           │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌───────────────── RAM (0x20000000) ─────────────────────┐  │
│  │  .data           — initialised data                    │  │
│  │  .bss            — zero-initialised data               │  │
│  │  .got / .got.plt — global offset table (patched)       │  │
│  │  [free heap]     — allocated by new/delete             │  │
│  │  [libsh1.so]     — loaded segments at runtime          │  │
│  │  [libsh2.so]     — loaded segments at runtime          │  │
│  │  stack           — main stack (grows downward)         │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌────────────── CPU (Cortex-M4) ─────────────────────────┐  │
│  │  elf-reader.cpp     — parses ELF headers/sections      │  │
│  │  shlib-loader.cpp   — loads, patches, runs constructors │  │
│  │  main.cpp           — application logic + demo          │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 The two "sides" of the loader

The loader works in **two domains** simultaneously:

| Domain | What it contains | Where it lives |
|--------|-----------------|----------------|
| **Main** (`m_main`) | Sections of the main executable that reference library symbols: `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash`, `.inits` | FLASH (read-only at runtime, but addresses are known via `_reloc`) |
| **Shlib** (`m_shlib`) | Sections loaded from the `.so` file: `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash`, `.inits`, `.got`, `.got.plt`, `.text`, `.data`, `.bss` | RAM (copied there by `load_bin()`) |

The loader reads `m_main` to learn which symbols the main program expects from the library;
it reads `m_shlib` to learn which symbols the library exports and which external symbols it
needs from the main program.

---

## 4. How It Works — Step by Step

### Step 1 — Open and verify the ELF file

```cpp
elf_loader::ElfReader efr;
if (!efr.open_elf("libsh1/libsh1.so")) { /* error */ }
```

`open_elf` calls `verify_elf()` which checks:
- Magic bytes: `0x7f E L F`
- Class: ELF32 (`e_ident[EI_CLASS] == ELFCLASS32`)
- Type: `ET_DYN` (3) — shared library
- Machine: `EM_ARM` (40 / 0x28)

### Step 2 — Determine binary size

`get_binary_size()` iterates all `PT_LOAD` program headers and returns the highest
`p_vaddr + p_memsz`. This is the total contiguous RAM footprint needed.

### Step 3 — Load segments into RAM

```cpp
ShlibLoader shl(std::move(efr));
uint8_t* buffer = new uint8_t[size];
shl.load_bin(buffer);
```

`load_bin()` iterates program headers again and for each `PT_LOAD`:
- Reads `p_filesz` bytes from file offset `p_offset` into `buffer + p_vaddr`
- Zero-fills the remaining `p_memsz - p_filesz` bytes (handles `.bss`)

At this point the library is in RAM but **not yet usable** — all GOT entries still contain
the original (unrelocated) values.

### Step 4 — Populate helper section descriptors

```cpp
shl.populate_helper_sections();
```

This reads section header information for both the loaded library (`m_shlib`) and the
main application (`m_main`). For the main application, it uses the `_reloc` symbol
(placed by the linker script, see §8) to locate `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash`,
and `.inits` in flash.

### Step 5 — Count and allocate patch arrays

```cpp
uint32_t nbr_shlib = shl.get_shlib_patch_count();
uint32_t nbr_main  = shl.get_main_patch_count();
SymbolPatch* ssp_shlib = new SymbolPatch[nbr_shlib];
SymbolPatch* ssp_main  = new SymbolPatch[nbr_main];
shl.allocate_patch_arrays({ssp_shlib, nbr_shlib}, {ssp_main, nbr_main});
```

The count includes:
- All entries from `.rel.dyn` (relocation entries)
- All `STB_WEAK` symbols from `.dynsym` that are **not** already in the relocation list

### Step 6 — Find symbols to patch (library side)

```cpp
shl.find_shlib_symbols_to_patch({ssp_shlib, nbr_shlib});
```

For each relocation entry it builds a `SymbolPatch`:
- `got_address` = `r_offset` (where to write the resolved address, i.e. the GOT slot)
- `type` = `ELF32_R_TYPE(r_info)` — e.g. `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`
- `index` = `ELF32_R_SYM(r_info)` — index into `.dynsym`
- `binding` = `ELF32_ST_BIND(sym.st_info)` — `STB_LOCAL`, `STB_GLOBAL`, `STB_WEAK`
- `virtual_address` = `sym.st_value` — the symbol's value within the library
- `name` = string from `.dynstr`

### Step 7 — Find symbols to patch (main side)

Same process but reading from `m_main` instead of `m_shlib`. This finds the main program's
relocation entries that reference library symbols.

### Step 8 — Patch the library

```cpp
shl.patch_shlib();
```

For each `SymbolPatch` in the library's list, depending on binding and type:

| Binding | Condition | Action |
|---------|-----------|--------|
| `STB_LOCAL` + `R_ARM_RELATIVE` | always | `*GOT = original_value + buffer_base` |
| `STB_GLOBAL` | `sym.st_value == 0` | Symbol is external: look it up in **main**'s symbol table, write address to GOT |
| `STB_GLOBAL` | `sym.st_value != 0` | Symbol defined in library itself: `*GOT = st_value + buffer_base` |
| `STB_WEAK` | `got_address != 0` | Try main first; if not found, use library's own definition |

### Step 9 — Patch the main program

```cpp
shl.patch_main();
```

For each `SymbolPatch` in the main's list:

| Type | Action |
|------|--------|
| `R_ARM_COPY` | Copy data from library symbol address to the address specified in the relocation |
| Other global | Write the library symbol's runtime address into the main program's GOT slot |
| Weak | Try library address; if found, write to GOT or cache for `resolve_weak_function()` |

### Step 10 — Execute constructors

```cpp
shl.execute_constructors();
```

Reads the loaded library's `.inits` section (`.init_array`) and calls every function pointer
in order. The `__attribute__((constructor))` functions in the library source become entries
in this array.

### Step 11 — Use the library

After patching, the library's functions and data are fully accessible:

```c
uint32_t result = library_function(7);   // resolved through GOT
uint32_t val = *(unsigned int*) aa;      // symbol patched to library address
struct Str_Lib s = ret_struct_lib();     // function call through PLT/GOT
float s = sinf(sll.f);                  // math library from main
```

---

## 5. Creating a Shared Library

### 5.1 Source code conventions

```c
// mylib.c
#include "mylib.h"

// Exported global variables (goes to .data, will be accessed by main via R_ARM_COPY)
unsigned int my_var = 42;

// Exported function (callable from main; main's GOT gets patched)
unsigned int my_function(unsigned int x) {
    return x + my_var;
}

// Weak symbol (can be overridden by the main application)
__attribute__((weak)) unsigned int my_weak(void) {
    return 0;
}

// Constructor (runs when library is loaded, via .init_array)
__attribute__((constructor)) void my_init(void) {
    my_var *= 2;
}
```

### 5.2 Compiler flags

```makefile
# Required: position-independent code
LIB_CFLAGS = -fPIC

# Optional but recommended for embedded
LIB_CFLAGS += -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
LIB_CFLAGS += -O0 -g3
LIB_CFLAGS += -fno-inline-functions -fno-common
LIB_CFLAGS += -Wall -Wextra
```

**Critical**: `-fPIC` generates position-independent code that uses the GOT and PLT so
the loader can relocate it at runtime. Without `-fPIC`, the code uses absolute addresses
and will not work when loaded at an arbitrary RAM location.

### 5.3 Linker script

The library must be linked as a **shared object** (`-shared`) with a custom linker script
that places everything at RAM offset 0 (position-independent).

**Minimum memory layout** (`libn/ldscripts/mem.ld`):

```
MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}
```

**Minimum section layout** (`libn/ldscripts/sections.ld`):

```
SECTIONS
{
    .inits : { KEEP(*(.init_array)) KEEP(*(.fini_array)) } > RAM
    .plt    : { *(.plt) } > RAM
    .got    : { *(.got) } > RAM
    .text   : { *(.text*) *(.rodata*) } > RAM
    .data   : { *(.data*) } > RAM
    .bss    : { *(.bss*) *(COMMON) } > RAM
}
```

### 5.4 Compiling and linking

```makefile
# Pattern for compiling
libn/%.o: libn/%.c
    $(CC) $(LIB_CFLAGS) -Ilibn -c $< -o $@

# Pattern for linking
libn/libn.so: libn/mylib.o
    $(CC) $(LIB_CFLAGS) $< -o $@ \
        -shared -nostartfiles \
        -T libn/ldscripts/gcc.ld \
        -Wl,--gc-sections \
        -Wl,--no-warn-rwx-segments

    # Create binary dump for loading into flash filesystem
    $(OBJCOPY) -O binary $@ libn/libn.bin
```

The `.bin` file is the raw image that can be stored in external flash and loaded by
`shlib_loader::load_bin()` at a later time.

### 5.5 Building with CMake

A CMake build is provided in addition to the Makefile (`CMakeLists.txt` +
`cmake/arm-none-eabi.cmake`). It mirrors the Make rules — the same two `.so`
libraries and firmware ELF, and it compiles `elf-image.cpp` from the shared
`../../common-elf-reader` repo:

```console
$ cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
$ cmake --build build
```

- Firmware ELF: `build/f411ce-sh.elf` (+ `.bin`, `.elf.lst`). The `.so` libraries
  are written beside their sources, as with Make.
- Override the toolchain with `-DTOOLCHAIN_PATH=...` and the shared parser with
  `-DCOMMON_ELF_READER_DIR=...`.
- `build-and-run-cmake.sh` builds this tree and flashes `build/f411ce-sh.elf`
  over OpenOCD + GDB.

---

## 6. Configuring the Main Application

### 6.1 Include the library headers

The main application needs the library's header to know the function prototypes and type
definitions. Symbol resolution happens at load time, not compile time, so the headers
do not need to match the exact implementation — they only need to declare the interface.

```c
#include "mylib1.h"   // declares library_function(), aa, bb, etc.
```

### 6.2 Linker configuration for the main executable

The main executable must be linked with:
- Its own dynamic sections (`.dynsym`, `.dynstr`, `.hash`, `.rel.dyn`) — the loader reads them
- The shared libraries listed with `-l` (so the linker generates the necessary relocations)
- The `_reloc` initialisation data block (see §8)

**Makefile linker flags:**

```makefile
APP_LDFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
APP_LDFLAGS += -T ldscripts/mem-sh.ld
APP_LDFLAGS += -T ldscripts/libs-sh.ld
APP_LDFLAGS += -T ldscripts/sections-sh.ld
APP_LDFLAGS += -nostartfiles
APP_LDFLAGS += -Wl,--gc-sections
APP_LDFLAGS += --specs=nano.specs
APP_LDFLAGS += -u _printf_float
APP_LDFLAGS += -Wl,--no-dynamic-linker       # suppress .interp / PT_PHDR
APP_LDFLAGS += -Wl,--no-warn-rwx-segments    # suppress RWX warning on Cortex-M
APP_LDFLAGS += -Llibsh1 -Llibsh2 -lsh1 -lsh2
```

### 6.3 Memory layout for the main executable

**Main linker memory** (`ldscripts/mem-sh.ld`):

```
MEMORY
{
  RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
}
```

Key points:
- RAM is `(rw)` not `(xrw)` — no code executes from RAM in the main executable
- FLASH is `(rx)` — all code runs from flash
- The shared library's code is loaded into the heap area of RAM at runtime

### 6.4 Section placement for dynamic sections

When the main executable is linked with `-lsh1 -lsh2`, the linker emits **dynamic sections**
in the main ELF (`.dynsym`, `.dynstr`, `.hash`, `.rel.dyn`). These sections are normally
used by the system dynamic linker (`ld.so`) on desktop Linux, but here they serve a
different purpose: **they tell the embedded loader which symbols the main program expects
the libraries to provide, and where the main program's GOT slots are.**

Without these sections the loader cannot resolve main→lib references. The linker script
**must** place them explicitly — if omitted, the linker still creates them but puts them
at unpredictable addresses (often before flash base, causing the RWX segment warning).

Each section's role in the loader:

`.dynsym` (dynamic symbol table)
: Lists every symbol the main program imports from shared libraries (`library_function`,
  `aa`, `bb`, etc.) along with their binding (GLOBAL/WEAK) and the main program's own
  exported symbols (`AA`, `BB`, `s1`, `f()`, `w()`). The loader reads this table to know
  what needs patching in the main→lib direction.

`.dynstr` (dynamic string table)
: Holds the ASCII names for every symbol in `.dynsym`. The loader indexes into it via
  each symbol's `st_name` field.

`.hash` (ELF SysV hash table)
: A hash table over `.dynsym` that lets the loader find symbols by name in O(1) average
  time instead of scanning the entire symbol table. Without it, `find_symbol_address()`
  would be O(n) for every lookup.

`.rel.dyn` (relocation table)
: Contains every relocation entry the main program needs. Each entry specifies:
  - `r_offset` — the absolute address (in RAM) of a GOT slot that needs patching
  - `r_info` — the symbol index (which symbol to resolve) and relocation type
    (`R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`, `R_ARM_COPY`, etc.)

  The loader iterates this table in `patch_main()` and writes the resolved runtime
  address of each library symbol into the corresponding GOT slot.

`.inits` (initialisation array + `_reloc` block)
: Contains the `_reloc` data block (see §8) which tells the loader the exact flash
  addresses and sizes of all four sections above. This is the bridge between build-time
  layout and runtime discovery.

**If any of these sections is missing or placed at a wrong address:**
- The loader cannot determine what symbols need patching
- `find_shlib_symbols_to_patch()` / `find_main_symbols_to_patch()` return empty lists
- Library function calls from main will jump to zero or garbage → HardFault
- `R_ARM_COPY` relocations for variables like `aa`, `bb` are never processed

**The `.got` and `.got.plt` sections** are the GOT itself — writable RAM slots that
the loader patches with resolved addresses. They must be in RAM (`> RAM`) with their
initial values stored in flash (`AT> FLASH`):

```
.got     : { _sgot = .; *(.got*);     _esgot = .;     } > RAM AT> FLASH
.got.plt : { _sgot_plt = .; *(.got.plt*); _esgot_plt = .; } > RAM AT> FLASH
```

The `_sgot` / `_esgot` symbols are used by `printGOT()` in the demo to dump the
contents of the GOT for debugging.

---

## 7. Linking and Loading

### 7.1 The two-pass linking model

1. **Build time** — The main executable is linked with `-lsh1 -lsh2`. The linker:
   - Records all undefined symbols in `.dynsym` / `.rel.dyn` of the main ELF
   - Creates GOT/PLT entries in the main ELF for cross-references
   - Places the `_reloc` block in the `.inits` section

2. **Runtime** — The loader:
   - Reads the main ELF's `.rel.dyn` / `.dynsym` / `.dynstr` (located via `_reloc`)
   - Reads the library ELF's `.rel.dyn` / `.dynsym` / `.dynstr` (from RAM)
   - Patches addresses in both directions (main→lib, lib→main)

### 7.2 Why `-lsh1 -lsh2` is needed

Without linking against the shared libraries, the linker would report undefined symbol
errors for every function and variable that comes from the library. By linking with
`-lsh1 -lsh2`, the linker:

- Creates the necessary GOT entries in the main ELF
- Generates `R_ARM_GLOB_DAT` and `R_ARM_JUMP_SLOT` relocation entries
- Records symbol names in `.dynsym` / `.dynstr`

The actual addresses are not filled in at link time — that's the loader's job.

### 7.3 Loading sequence

```
main() ──────────────────────────────────────────────────────┐
  │                                                            │
  ├─ efr.open_elf("libsh1.so")                                 │
  │    └─ verify_elf()                                         │
  │    └─ read header, program headers, section headers         │
  │                                                             │
  ├─ test_elf_reader(efr)   [optional debug]                    │
  │    └─ prints section headers, program headers, .dynsym hex  │
  │                                                             │
  ├─ ShlibLoader shl(std::move(efr))                            │
  │                                                             │
  ├─ shl.load_bin(buffer)                                       │
  │    └─ for each PT_LOAD: read into buffer + p_vaddr          │
  │    └─ prints segment info (type, vaddr, size, permissions)   │
  │                                                             │
  ├─ shl.populate_helper_sections()                              │
  │    └─ reads _reloc from main binary                         │
  │    └─ sets up m_shlib[] and m_main[] section maps           │
  │                                                             │
  ├─ shl.allocate_patch_arrays()                                │
  ├─ shl.find_shlib_symbols_to_patch(ssp_shlib)                 │
  ├─ shl.find_main_symbols_to_patch(ssp_main)                   │
  │                                                             │
  ├─ shl.patch_shlib()                                          │
  │    └─ for each SymbolPatch: *GOT = resolved_addr            │
  │                                                             │
  ├─ shl.patch_main()                                           │
  │    └─ for each main SymbolPatch: *target = resolved_addr    │
  │                                                             │
  ├─ shl.execute_constructors()                                  │
  │    └─ calls each function pointer in .init_array            │
  │                                                             │
  ├─ // Library is now fully usable                              │
  ├─ result = library_function(7);                              │
  └─ val = *(unsigned int*)aa;                                  │
```

---

## 8. The Initialisation Data Block

### 8.1 What is `_reloc`?

`_reloc` is a symbol placed in the `.inits` section of the main executable by the linker
script. It is a table of address/length pairs that tell the loader where to find the main
program's dynamic sections in flash.

### 8.2 Structure

The data block is emitted by this linker script fragment (in `.inits`):

```
_reloc = .;

LONG(LOADADDR(.rel.dyn));           // 0: flash address of .rel.dyn
LONG(SIZEOF(.rel.dyn));            // 1: size of .rel.dyn
LONG(LOADADDR(.dynsym));           // 2: flash address of .dynsym
LONG(SIZEOF(.dynsym));            // 3: size of .dynsym
LONG(LOADADDR(.dynstr));           // 4: flash address of .dynstr
LONG(SIZEOF(.dynstr));            // 5: size of .dynstr
LONG(LOADADDR(.hash));             // 6: flash address of .hash
LONG(SIZEOF(.hash));              // 7: size of .hash
LONG(LOADADDR(.inits));            // 8: flash address of .inits
LONG(SIZEOF(.inits));             // 9: size of .inits
```

### 8.3 How the loader reads it

```cpp
extern uint32_t _reloc;

struct InitLds {
    uint32_t reldyn;       uint32_t reldyn_size;
    uint32_t dynsym;       uint32_t dynsym_size;
    uint32_t dynstr;       uint32_t dynstr_size;
    uint32_t hash;         uint32_t hash_size;
    uint32_t inits;        uint32_t inits_size;
};

const InitLds* init_lds = reinterpret_cast<const InitLds*>(&_reloc);
```

The loader adds `ram_main_address_` (base of main RAM) to each address to get the runtime
pointer.

---

## 9. Relocation and Patching

### 9.1 Relocation types handled

| ELF constant | Value | Direction | Meaning |
|-------------|-------|-----------|---------|
| `R_ARM_RELATIVE` | 23 (0x17) | lib→lib | `*GOT += load_address` (base-relative fixup) |
| `R_ARM_GLOB_DAT` | 21 (0x15) | lib→lib | GOT slot holds address of global symbol |
| `R_ARM_JUMP_SLOT` | 22 (0x16) | lib→lib | PLT stub jumps through this GOT slot |
| `R_ARM_ABS32` | 2 (0x02) | lib→lib | Absolute 32-bit address (used for `.init_array` constructor pointers) |
| `R_ARM_COPY` | 20 (0x14) | main→lib | Copy symbol address from lib to main's BSS/data |

**Note on `R_ARM_ABS32`:** Constructor pointers in `.init_array` use this type.
`r_offset` values of 0, 4, 8, … are valid — they are offsets relative to the `.init_array`
section base, which starts at virtual address 0 in the library. The handler adds the buffer
base to the symbol value: `*slot = sym.st_value + (uint32_t)buffer`.
These entries must **not** be skipped even when `r_offset == 0`.

### 9.2 Patching algorithm (`patch_shlib`)

```
for each SymbolPatch in library:
    skip if name is empty

    if binding == STB_LOCAL AND type == R_ARM_RELATIVE:
        // Library-local relocation: add buffer base to original value
        *GOT_slot = original_value + (uint32_t)buffer

    else if binding == STB_GLOBAL:
        if virtual_address == 0:
            // Symbol is external (defined in main program)
            lookup symbol in main's dynsym
            *GOT_slot = runtime_address_in_main
        else:
            // Symbol is defined in this library
            *GOT_slot = virtual_address + (uint32_t)buffer

    else if binding == STB_WEAK:
        if GOT_slot != 0:
            // Try to find in main first (main's definition wins)
            adr = find_symbol_in_main(name)
            if adr == 0:
                adr = virtual_address + (uint32_t)buffer
            *GOT_slot = adr
```

### 9.3 Patching algorithm (`patch_main`)

```
for each SymbolPatch in main:
    skip if name is empty

    // resolve_addr: global_syms returns final runtime address;
    // find_symbol_address returns raw offset + buffer added.
    adrval = resolve_addr(name)

    if binding != STB_WEAK:
        if type == R_ARM_COPY:
            // Store the library symbol's runtime ADDRESS into main's BSS slot.
            // Main uses the symbol as a pointer: *(uint32_t*)aa dereferences it.
            // Write to m.got_address (r_offset = main's BSS address),
            // NOT m.virtual_address (sym.st_value = library offset).
            if adrval != 0:
                *(uint32_t*)m.got_address = adrval
        else:
            // Standard global symbol: write address to GOT slot
            if adrval != 0 && m.got_address != 0:
                *(uint32_t*)m.got_address = adrval

    else:  // STB_WEAK
        if adrval != 0:
            if m.got_address != 0:
                *(uint32_t*)m.got_address = adrval
            else:
                weak_sym_cache.push({name, adrval, m.virtual_address})
        else:
            // Not found in library — main's default definition is used
```
            // Global symbol from library
            lib_addr = find_symbol_in_library(name)
            if lib_addr != 0:
                *main_GOT_slot = lib_addr + (uint32_t)buffer

    else:  // STB_WEAK
        lib_addr = find_symbol_in_library(name)
        if lib_addr != 0:
            lib_addr += (uint32_t)buffer
            if main_GOT_slot != NULL:
                *main_GOT_slot = lib_addr
            else:
                // No GOT slot: cache for resolve_weak_function()
                weak_sym_cache.push({name, lib_addr, main_virtual_address})
```

### 9.4 Hash-based symbol lookup

The loader implements the ELF System V hash function to look up symbols by name without
scanning the entire symbol table:

```cpp
unsigned long elf_hash(std::string_view name) {
    unsigned long h = 0;
    for (char c : name) {
        h = (h << 4) + (unsigned char)c;
        unsigned long g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
```

Lookup uses the `.hash` section:
```
hashval = elf_hash(name)
index = bucket[hashval % nbucket]
while index != STN_UNDEF:
    if strcmp(dynstr + syms[index].st_name, name) == 0:
        return syms[index].st_value
    index = chain[index]
```

---

## 10. Weak Symbols

### 10.1 Purpose

A **weak symbol** is a symbol that may be overridden by another definition at link/load time.
If no overriding definition exists, the weak definition is used. This is identical to the
`__attribute__((weak))` behaviour at link time, but extended to runtime library loading.

### 10.2 Declaring weak symbols

**In the library** (providing a default implementation):
```c
// mylib.c
#pragma weak foo
unsigned int foo(void) {
    return 20;
}
```

**In the main program** (optionally overriding):
```c
// main.c
unsigned int foo(void) {
    return 100;  // overrides the library's weak definition
}
```

### 10.3 Resolution logic

1. During `patch_shlib`, weak symbols in the library's GOT are patched:
   - First try to find the symbol in the main program's symbol table
   - If found, the GOT entry points to the main program's implementation
   - If not found, the library's own implementation is used

2. During `patch_main`, weak symbols in the main program are patched:
   - Find the symbol in the loaded library's symbol table
   - If found, patch the GOT entry (or cache for lazy resolution)

3. The `resolve_weak_function()` method provides lazy resolution for weak function
   pointers:

```cpp
unsigned int ShlibLoader::resolve_weak_function(funcm f) {
    uint32_t ad = reinterpret_cast<uint32_t>(f);
    for (const auto& wms : weak_sym_cache_) {
        if (ad == wms.old_addr) {
            f = reinterpret_cast<funcm>(wms.addr);
            break;
        }
    }
    return f();
}
```

---

## 11. Constructors

### 11.1 How constructors work

C++ static constructors and `__attribute__((constructor))` functions in C are collected
by the compiler into the `.init_array` section. The compiler also generates a `.preinit_array`
and a `.fini_array` for pre-initialisation and finalisation.

The library linker script collects these into the `.inits` section:

```
.inits : {
    KEEP(*(.init))
    KEEP(*(.fini))
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
} > RAM
```

### 11.2 Executing constructors

The loader's `execute_constructors()` method:

```cpp
void ShlibLoader::execute_constructors() {
    uint32_t ctor = reinterpret_cast<uint32_t>(m_shlib[InitsIdx].address);
    uint32_t szConstr = m_shlib[InitsIdx].size;

    for (uint32_t i = 0; i < szConstr; i += sizeof(uint32_t)) {
        pctor pc = *reinterpret_cast<pctor*>(ctor + i);
        if (pc) {
            pc();   // call the constructor function
        }
    }
}
```

### 11.3 When constructors run

Constructors run after all relocations have been patched but before any library function
is called from the main application. This ensures that any global state initialisation
that depends on patched symbols is complete before first use.

Example output from the demo:
```
I am a Constructor!! 40
I am a Constructor1!!
I am a Constructor2!!
in Constructor2  Hello from mylib!
I am a Constructor3!!
```

---

## 12. Segment Permissions on Cortex-M (RWX)

### 12.1 The ARMv7-M MPU

Cortex-M4 has an optional **Memory Protection Unit (MPU)** that can split memory into
regions with independent permissions (read/write/execute). The MPU is **disabled** in the
default µOS++ startup and is not used by this loader.

### 12.2 Why RWX segments exist

On a system with an MMU (e.g. Linux on Cortex-A), each ELF LOAD segment can be placed
in a separate virtual page with its own permissions: code pages are RX, data pages are RW,
GOT pages are RW. The MMU enforces this at runtime.

On Cortex-M **without an MMU**, all memory is uniform. The linker places code, GOT, PLT,
and data in a single contiguous address range (starting at 0x00000000 for shared libraries).
The resulting LOAD segment is necessarily RWX because:
- `.text` and `.plt` need to be executable (X)
- `.got` and `.got.plt` need to be writable (W — the loader patches them)
- `.rodata` needs to be readable (R)

### 12.3 The linker warning

Newer binutils (GCC 12+) warn when a LOAD segment has RWX permissions. This is a
**security hardening feature** for OS-based systems. On bare-metal without MMU it is
a false positive: the Cortex-M cannot enforce W^X anyway.

### 12.4 How it is handled

| Component | Change | Rationale |
|-----------|--------|-----------|
| Main executable RAM | `(rw)` not `(xrw)` | No code in RAM; `.got`/`.got.plt` only need RW |
| Shared libraries | `--no-warn-rwx-segments` | RWX is required and unavoidable on Cortex-M without MMU |
| Main executable | `--no-warn-rwx-segments` | FLASH remains RWE due to dynamic sections; hardware is RX-only |
| Main executable | `--no-dynamic-linker` | Suppresses `.interp`/PT_PHDR creation by the linker |

### 12.5 If you have an MPU

If you enable the MPU (e.g. in `__initialize_hardware()`), you can set up regions after
the loader patches the library:

```
Region 0: FLASH (0x08000000, 512K)     → RO, XN disabled (code runs from here)
Region 1: RAM  (0x20000000, 128K)      → RW, XN enabled  (data only)
Region 2: lib buffer (0x20000000+offset, size) → RW, XN disabled
```

But the library's RAM buffer would still need to be executable for the loaded code to run.
Without an MMU, you cannot give a sub-region of a 128 KB region different permissions from
the rest. W^X on Cortex-M requires splitting code and data into separate physical memories
(e.g. loading library code into CCMRAM at 0x10000000 and data into SRAM at 0x20000000).

---

## 13. Production-Grade Features

The base loader (sections 1–12) handles the core loading and patching cycle. This section
documents seven additional hardening and capability features that bring it to production quality,
implemented on top of the same architecture.

### 13.1 Segment and Table Bounds Validation

Every data access from the ELF file or in-memory structures is checked against buffer limits
before use. This prevents a corrupt or malformed `.so` from crashing the device.

| Check | Location | What it guards |
|-------|----------|---------------|
| `p_vaddr + p_memsz <= buffer_size` | `load_segments()` | LOAD segment fits allocated RAM |
| `buffer_size >= get_binary_size()` | `load_bin()` | Allocation big enough for all segments |
| `sym_idx < num_syms` | `read_relocation_symbols()` | Symbol index within `.dynsym` |
| `r_offset + 4 <= buffer_size` | `patch_shlib()` | GOT write within library buffer |
| `st_name < dynstr_size` | `read_relocation_symbols()` | Symbol name within `.dynstr` |

If any check fails, the operation returns `LoaderError::InvalidSegment`,
`LoaderError::InvalidRelocation`, or `LoaderError::InvalidSymbol` instead of silently
corrupting memory.

### 13.2 Structured Error Enums

The `LoaderError` enum has been extended with descriptive error codes. Every public method
that can fail returns `LoaderError` instead of `void` or `bool`:

```cpp
enum class LoaderError {
    Ok,
    FileNotFound,
    NotAnElf,
    SectionNotFound,
    InvalidSegment,      // new — segment outside allocated buffer
    InvalidRelocation,   // new — bad symbol index or offset
    InvalidSymbol,       // new — bad string index
    DepNotFound,         // new — DT_NEEDED dependency missing
    OutOfMemory          // new — allocation failure
};

inline const char* loader_error_string(LoaderError e);   // human-readable name
```

Callers check each step and can respond programmatically:

```cpp
LoaderError err = shl.load_bin(buffer, bufsize);
if (err != LoaderError::Ok) {
    printf("load_bin failed: %s\n", loader_error_string(err));
    my_error_handler(err);
    return;
}
```

### 13.3 Per-Symbol Graceful Failure

During `patch_shlib()` and `patch_main()`, if a single symbol cannot be resolved (bad index,
name not found, GOT address out of range), the loader logs a diagnostic and **skips only that
symbol**, continuing with the next one.

```
WARN: reloc 7 has sym idx 99 >= num_syms 42, skipping
WARN: zero got_address, skipping .hidden_sym
Patched 28 symbols in shlib (2 skipped)
```

Previously, a hardware fault or `exit(1)` would terminate the whole load. With graceful
failure, the library is partially usable — a critical functional module might still work
even if a debug symbol fails.

Both `patch_shlib()` and `patch_main()` now return `uint32_t` (count of successfully patched
symbols) instead of `void`, so the caller can decide whether the library is "good enough".

### 13.4 DT_NEEDED Dependency Walk

Shared libraries can declare dependencies via `DT_NEEDED` entries in the `.dynamic` section.
The loader parses these at `load_bin()` time and stores them in `dependencies_()`:

```
$ readelf -d libfoo.so
 0x00000001 (NEEDED)  Shared library: [libcommon.so]
 0x00000001 (NEEDED)  Shared library: [libmath.so]
```

The `ShlibLoader::resolve_dependencies()` method accepts a callback that loads each
dependency by name:

```cpp
LoaderError load_dep(std::string_view soname, ShlibLoader& out) {
    // Look up "soname" in a pre-registered table or filesystem
    // Open it, create an ElfReader, move into ShlibLoader
    return elf.open_elf(find_path_for(soname)) ? LoaderError::Ok : LoaderError::DepNotFound;
}

shl.resolve_dependencies(&global_syms, load_dep);
```

Dependencies are loaded **before** the parent library is patched, ensuring symbols from
dependency libraries are available during relocation fixup.

### 13.5 Symbol Interposition (First-Loaded-Wins)

When multiple loaded libraries export the same global symbol, the POSIX convention is that
the **first loaded library wins** — subsequent definitions of the same symbol are ignored.

The `SymbolTable` class implements this:

```cpp
class SymbolTable {
public:
    void add_global(std::string_view name, uint32_t address);
        // Silently ignored if name already registered (non-weak)
    void add_weak(std::string_view name, uint32_t address);
        // Weak can be overridden by later add_global
    uint32_t find(std::string_view name) const;
        // Returns the first-registered address
};
```

The global `SymbolTable` is shared across all `ShlibLoader` instances. During `patch_shlib()`,
every resolved external symbol is checked against the global table first — if another loaded
library already provides it, that address is used. Afterwards, the library's own exported
symbols are registered in the table (globals first, then weaks), ensuring interposition
rules are respected.

```
libsh1 provides: aa=0x20001000, bb=0x20001004, library_function=0x20001500
libsh2 provides: aaa=0x20002000, bbb=0x20002004, lf=0x20002500
Both provide:   mm (weak, libsh2's is ignored if libsh1 loaded first)
→ global_syms.find("mm") returns libsh1's address
```

### 13.6 Topological Constructor Ordering

When libraries have dependencies (DT_NEEDED), their constructors must run in **dependency
order** — a parent's init must complete before its child's init starts. Likewise, destructors
(if implemented) must run in reverse order.

The loader's `execute_constructors()` runs each library's `.init_array` in the order the
libraries were loaded (which respects dependency order if `resolve_dependencies()` was used).
A standalone topological-sort helper for the general case:

```
For each loaded library:
  1. Build dependency graph from DT_NEEDED entries
  2. Collect all .init_array pointers
  3. Topological sort (DFS, reverse-postorder)
  4. Execute in sorted order

Example: libA → libB → libC
  Constructors: A.init, B.init, C.init  (A before B before C)
  Destructors:  C.fini, B.fini, A.fini  (reverse order)
```

If no dependencies exist (as in the basic demo), constructors run in link order, which is
the order they appear in the section — this matches the compiler's expected behaviour.

### 13.7 DSB/ISB Cache Coherence Barriers

On Cortex-M7, M55, and M85 cores with a **data cache**, writing GOT entries via `patch_shlib()`
and `patch_main()` writes to cached memory. The CPU may continue fetching instructions from
stale cache lines, causing a hard fault when the library code references a GOT entry that
hasn't yet been flushed to main memory.

The `sync_after_patching()` function issues the required barriers:

```cpp
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__)
void sync_after_patching() {
    __asm volatile("DSB" : : : "memory");   // drain write buffer
    __asm volatile("ISB" : : : "memory");   // flush instruction pipeline
}
#else
void sync_after_patching() {
    // Cortex-M0/3/4 have no data cache — no barriers needed
}
#endif
```

`sync_after_patching()` is called automatically after `patch_shlib()` and `patch_main()`.
On M4 (STM32F411), the barrier compiles to nothing. On M7, it ensures correctness without
any code changes.

### 13.8 Relocation Types Handled in `patch_shlib()`

The `patch_shlib()` method handles all relocation types generated by `-fPIC -shared` for
Thumb-2 ARM. The table below shows which types are supported and how they are processed:

| Type | ELF constant | Value | Handling |
|------|-------------|-------|----------|
| `R_ARM_RELATIVE` | `R_ARM_RELATIVE` | 23 (0x17) | Add buffer base to existing value at GOT slot: `*Addr += (uint32_t)buffer` |
| `R_ARM_GLOB_DAT` | `R_ARM_GLOB_DAT` | 21 (0x15) | Write resolved symbol address to GOT slot: lookup in global table → main → library self |
| `R_ARM_JUMP_SLOT` | `R_ARM_JUMP_SLOT` | 22 (0x16) | Identical to GLOB_DAT: write resolved symbol address to PLT GOT slot |
| `R_ARM_ABS32` | `R_ARM_ABS32` | 2 (0x02) | Add buffer base to absolute pointer: `*Addr = st_value + buffer`. Used for `.init_array` |
| `R_ARM_COPY` | `R_ARM_COPY` | 20 (0x14) | Handled in `patch_main()`: stores library symbol address into main's BSS slot |

**Important:** `R_ARM_ABS32` relocations have `r_offset = 0, 4, 8, 12` for the four constructor
pointers in `.init_array`. These are **valid** offsets — the first entry at `buffer + 0` must
not be rejected. The loader processes `R_ARM_ABS32` before any binding-specific logic.

### 13.9 Common Pitfalls and Fixes

| Symptom | Root cause | Fix |
|---------|-----------|-----|
| HardFault during `execute_constructors()` | `R_ARM_ABS32` not handled; first `.init_array` entry (r_offset=0) left unpatched | Add explicit `R_ARM_ABS32` handler; do not skip `got_address == 0` |
| Helper main sections show `0x28xxxxxx` addresses | `set_ram_main(0x20000000)` adds RAM base to FLASH addresses from `_reloc` block | Remove `set_ram_main()` call; `ram_main_address_` stays nullptr (main dynamic sections are in FLASH, not RAM) |
| `patch_main` writes `0x4xxxxxxx` addresses to GOT | `global_syms->find()` returns final runtime address, but code unconditionally adds `buffer` again | Use `resolve_addr()` lambda: add `buffer` only for `find_symbol_address()` path, not for `global_syms` path |
| `R_ARM_COPY` writes to wrong address | Used `m.virtual_address` (library offset) instead of `m.got_address` (main's BSS address) | Write to `m.got_address` which holds `r_offset` — the main's BSS slot |
| Weak symbol fallback corrupts FLASH address | Fallback `m.virtual_address + buffer` added RAM base to main's FLASH function address | Remove fallback; weak symbols not found in library default cleanly to main's implementation |
| "global NOT FOUND" for symbols in other libraries | Symbol searched only in current library's symbol table | Use `SymbolTable` shared across loaders; register each library's exports after patching |
| Constructor order wrong | Constructors run in section order, not dependency order | Use topological sort when libraries have `DT_NEEDED` dependencies |

### 13.10 Summary: 7 Features Added

| # | Feature | Files changed | Lines added |
|---|---------|--------------|-------------|
| 1 | Bounds validation | `elf-reader.cpp`, `shlib-loader.cpp` | ~40 |
| 2 | Structured errors | `elf-reader.h`, `elf-reader.cpp`, `shlib-loader.h` | ~30 |
| 3 | Per-symbol graceful failure | `shlib-loader.cpp` | ~15 |
| 4 | DT_NEEDED dependency walk | `elf-reader.cpp`, `shlib-loader.cpp` | ~50 |
| 5 | Symbol interposition | `elf-reader.h`, `elf-reader.cpp` | ~60 |
| 6 | Topological constructors | `shlib-loader.cpp` (ordering) | ~20 |
| 7 | DSB/ISB barriers | `shlib-loader.cpp` | ~10 |
|   | **Total** | **6 files** | **~225** |

The core loading logic (~900 lines) remains unchanged — these features are layered on top
without changing the fundamental architecture.

---

## 14. Test Suite and Library Reference

This section documents the test functions and the API exercised by the two demo shared
libraries (`libsh1.so` and `libsh2.so`). When run on hardware via semihosting, these tests
produce serial output that verifies each loader feature.

### 14.1 Test Function Reference

All test functions are defined in `src/main.cpp` and called from `main()`. They produce
diagnostic output via `printf()` that can be captured through semihosting
(`build-and-run.sh` for the Make build, `build-and-run-cmake.sh` for the CMake build)
or serial.

| Function | Features exercised | Expected output pattern |
|----------|-------------------|------------------------|
| `test_elf_reader()` | ELF header parse, section/program headers, `.dynsym`/`.rel.dyn` hex dump | Section headers table, program headers, `.dynsym` data rows |
| `test_bounds_validation()` | `InvalidSegment` error on undersized buffer, structured error string | `load_bin(bufsize=..., needed=...) → InvalidSegment` / `PASS` |
| `test_dependency_scan()` | `DT_NEEDED` parsing, `dependencies_()` population | `No dependencies declared (expected for libsh1/libsh2)` |
| `test_symbol_table()` | `SymbolTable::add_global()` / `find()`, interposition first-wins | `aa → 0x20001... found` / `interposition check: PASS` |
| `test_patch_counts()` | `patch_shlib()`/`patch_main()` return values, graceful skip | `patch_shlib: N OK of M total (K skipped)` / `PASS` |
| `test_structured_error_strings()` | All 9 `LoaderError` values via `loader_error_string()` | `Ok → "Ok"` … `OutOfMemory → "OutOfMemory"` |
| `test_Shlib_Loader()` | Full load/patch/constructor cycle, function calls, struct return, weak resolve | Constructors print, function results, GOT dump |

**Execution order in `main()`:**

```
main
├─ open_elf("libsh1/libsh1.so")
├─ open_elf("libsh2/libsh2.so")
├─ test_elf_reader(efr)        — header/section dump for libsh1
├─ test_elf_reader(efr2)       — header/section dump for libsh2
├─ test_Shlib_Loader(shl)      — load libsh1: segments, sections, patches, ctors, calls
│   ├─ test_dependency_scan()  — DT_NEEDED of libsh1
│   └─ test_patch_counts()     — patched/skipped stats for libsh1
├─ test_Shlib_Loader(shl2)     — load libsh2: same sequence
│   ├─ test_dependency_scan()  — DT_NEEDED of libsh2
│   └─ test_patch_counts()     — patched/skipped stats for libsh2
├─ test_structured_error_strings() — all 9 error codes printed
├─ test_symbol_table()         — interposition + lookup from global SymbolTable
├─ test_bounds_validation()    — undersized buffer rejection
└─ timer + LED blink loop      — hardware functional test
```

### 14.2 Expected Semihosting Output (annotated)

```
Hello Arm World!
Standard output message.
Standard error message.
libsh1/libsh1.so                                  ← file names
libsh2/libsh2.so

size: 5645                                        ← test_elf_reader()
elf header size: 52
Section Headers:                                  ← 27 section headers listed
Program Headers:                                  ← 2 phdrs listed
.dynsym hex dump ...
.reldyn hex dump ...

Loading 2 segments into buffer[5645]:             ← load_bin() for libsh1
  [00] LOAD vaddr=0x0000 filesz=0x254c memsz=0x2558 perm=RWX
  [01] DYNAMIC ...

libsh1 size: 0x160D...                            ← binary hex dump
helper shlib sections :
name:      .inits size: ...
...                                               ← all 5 helper sections
helper main sections :                            ← from _reloc block
name:      .inits size: ...

Nbr. symbol to patch in lib: 29
Nbr. symbol to patch in main: 15

Shlib Symbols to patch:                           ← relocation listing
00   0x1558   0x00000000      0x01    0x0017    0x0000
...

 PATCH in LIBs                                    ← patch_shlib() output
00 === ABS32  bind: 1 0x20000FB8 0x20001909   print_constr
01 === ABS32  bind: 1 0x20000FBC 0x20001959   print_constr1
02 === ABS32  bind: 1 0x20000FC0 0x20001971   print_constr2
03 === ABS32  bind: 1 0x20000FC4 0x2000199D   print_constr3
04 ... self global  bind: 1 0x20001098 0x20002528   str_
...                                               ← remaining GOT patches
Patched 28 symbols in shlib (1 skipped)

 PATCH in MAIN                                    ← patch_main() output
00 --- R_ARM_COPY: aaa
       R_ARM_COPY *0x200006EC = 0x20003510
01 --- R_ARM_COPY: bb
       R_ARM_COPY *0x200006F0 = 0x200025BC
...
04 +++ global: library_function
       *Addr: 0x200002B8 val: 0x20001ACD
...
Patched 15 symbols in main (12 skipped)

GOT & lib1                                        ← printGOT()
GOT address : 0x20000000
...

I am a Constructor!! 40                           ← execute_constructors()
I am a Constructor1!!
I am a Constructor2!!
in Constructor2  Hello from mylib!
I am a Constructor3!!

boo weak 30                                       ← function calls
Struct : 1000 xxx 2 3 2.012560
...
=== [TEST] DT_NEEDED dependency scan ===          ← test_dependency_scan()
  No dependencies declared (expected for libsh1/libsh2)

=== [TEST] Per-symbol patch counts (graceful failure) ===
  patch_shlib: 28 OK of 29 total (1 skipped)
  patch_main:  14 OK of 15 total (1 skipped)
  Graceful failure: PASS

... [same sequence for libsh2] ...

=== [TEST] Structured error strings ===           ← test_structured_error_strings()
  Ok                   → "Ok"
  FileNotFound         → "FileNotFound"
  NotAnElf             → "NotAnElf"
  InvalidSegment       → "InvalidSegment"
  InvalidRelocation    → "InvalidRelocation"
  InvalidSymbol        → "InvalidSymbol"
  DepNotFound          → "DepNotFound"
  OutOfMemory          → "OutOfMemory"

=== [TEST] SymbolTable interposition (first-loaded-wins) ===
  aa                   → 0x20001F4C found
  bb                   → 0x20001F50 found
  library_function     → 0x20001540 found
  library_function2    → 0x20001D44 found
  str                  → 0x20001D04 found
  str_                 → 0x20001DC8 found
  s_lib                → 0x20001F54 found
  retstr               → 0x20001CE8 found
  g                    → 0x20001D90 found
  ffff                 → 0x00000000 NOT FOUND
  nonexistent          → 0x00000000 NOT FOUND
  interposition check: aa before=0x20001F4C after=0x20001F4C → PASS (first-wins)

=== [TEST] Bounds validation ===
  load_bin(bufsize=2822, needed=5645) → InvalidSegment
  Expected: InvalidSegment — actual: PASS

System clock: 84000000 Hz                         ← after all loader tests
...
```

### 14.3 `libsh1` — Function and Variable Reference

Source: `libsh1/mylib1.c` / `libsh1/mylib1.h`

**Exported global variables** (visible to main via `R_ARM_COPY`):

| Symbol | Type | Initial value | Tested in main as |
|--------|------|---------------|-------------------|
| `aa` | `unsigned int` | `10` | `*(unsigned int*)aa` |
| `bb` | `unsigned int` | `0` (BSS) | `*(unsigned int*)bb` |
| `s_lib` | `struct Str_Lib` | `{"15_", 125.236, "1235", 120, 30}` | `ret_struct_lib()`, `ret_struct()` |

**Exported functions:**

| Function | Signature | Behaviour | Test call |
|----------|-----------|-----------|-----------|
| `library_function(x)` | `uint32_t → uint32_t` | `x + 3 + aa` (calls main `f()` via GOT) | `z = library_function(7)` |
| `library_function2(x)` | `uint32_t → uint32_t` | `x * 3 - cc + dd` (local static) | `w0 = library_function2(7)` |
| `library_function3(x)` | `uint32_t → uint32_t` | `library_function(x) + library_function2(x) + bb` (cross-call plus `w()`) | `q = library_function3(7)` |
| `callback_f(x)` | `uint32_t → uint32_t` | `AA + f(aa + bb + x)` (main's `f()`, main's `AA`) | `qq = callback_f(7)` |
| `g(x)` | `uint32_t → uint32_t` | `aa + bb + x` | `ggg = g(7)` |
| `gg(x)` | `uint32_t → uint32_t` | `x + ptrfunc(g, x)` (function pointer to `g()`) | `gg7 = gg(7)` |
| `fff(x)` | `uint32_t → uint32_t` | `AA + BB + x` (main's globals) | `fff(10)` |
| `retstr()` | `void → char*` | returns `"Hello mylib!"` | `str = retstr()` |
| `retstr_()` | `void → char*` | returns `"Hello from mylib!"` | `str2 = retstr_()` |
| `print_lib()` | `void` | prints `bb` and local `bss2` | `print_lib()` |
| `print_lib2(s)` | `char* → void` | prints `s1` from main | `print_lib2("help")` |
| `ret_struct_lib()` | `void → struct Str_Lib` | returns `s_lib` | `sl1 = ret_struct_lib()` |
| `ret_struct(stm)` | `struct Str_Lib → struct Str_Lib` | copies + modifies fields, uses `strcat()` | `sll = ret_struct(sl)` |
| `ptrfunc(p, x)` | `(func*)×uint32_t → uint32_t` | `p(x) + x` (calls via function pointer) | `ptrfunc(gfun, 7)` in main |
| `foo()` | `void → uint32_t` | returns `20` (weak, overridable) | `xfoo = foo()` |
| `boo()` | `void → uint32_t` | returns `30` (weak) | `bfoo = boo()` |
| `lkj()` | `void → uint32_t` | returns `BB + 100` (weak, overrides main) | `lkj()` |

**Constructors** (executed by `execute_constructors()`):

```
print_constr()   → aa += aa; bb = aa + 20; printf("I am a Constructor!! %d", bb)
print_constr1()  → printf("I am a Constructor1!!")
print_constr2()  → printf("I am a Constructor2!!"); printf("%s", retstr())
print_constr3()  → printf("I am a Constructor3!!")
```

**Weak symbols declared** (can be overridden by main):

```
#pragma weak foo     → unsigned int foo(void);     default: returns 20
#pragma weak boo     → unsigned int boo(void);     default: returns 30
```

### 14.4 `libsh2` — Function and Variable Reference

Source: `libsh2/mylib2.c` / `libsh2/mylib2.h`

**Exported global variables:**

| Symbol | Type | Initial value | Tested in main as |
|--------|------|---------------|-------------------|
| `aaa` | `unsigned int` | `10` | `*(unsigned int*)aaa` |
| `bbb` | `unsigned int` | `0` (BSS) | `*(unsigned int*)bbb` |
| `XX` | `unsigned int` | `25` | (not directly tested) |

**Exported functions:**

| Function | Signature | Behaviour | Test call |
|----------|-----------|-----------|-----------|
| `lf(x)` | `uint32_t → uint32_t` | `x + 3 + aaa` | `z2 = lf(7)` in lib-count >1 block |
| `lf2(x)` | `uint32_t → uint32_t` | `x * 3 - ccc + ddd` | `w2 = lf2(7)` |
| `lf3(x)` | `uint32_t → uint32_t` | `lf(x) + lf2(x); bbb = ... + w(x)` (cross-call to libsh1 or main's `w`) | `q2 = lf3(7)` |
| `xxx()` | `void → uint32_t` | `printf; return BB + AA` | `xxx()` in lib-count >1 block |
| `pl()` | `void` | decrements `bbb`, prints | — |
| `pl2(s)` | `char* → void` | prints `s1` from main | `pl2("TOTO")` |

**Constructor:**

```
pr() → aaa += aaa; bbb = aaa + 20; printf("I am a Constructor in Lib2!! %d", bbb+10)
```

**Weak symbol:**

```
unsigned int mm() → returns 0 (overrides main's `mm()`, but main loaded first → main's wins)
```

Symbol `mm` is declared `#pragma weak mm` in both `main.cpp` (returns `AA + AA`) and
`libsh2` (returns `0`). Because `main` is loaded first, interposition rules mean main's
version wins — `mm()` returns `AA + AA` even when called from libsh2's context.

### 14.5 Cross-Library Symbol Resolution Map

The diagram below shows which symbols are resolved from which domain. Arrows point from
the relocation site to the provider:

```
 ┌──────────────────────────────────────────────────┐
 │                   main (f411ce-sh.elf)            │
 │                                                   │
 │  Symbols provided:                                │
 │    AA=5, BB=55, s1="Help...", f(), w(), mm()     │
 │    gfun(), lkj()                                  │
 │                                                   │
 │  Calls to libsh1 via GOT:                         │
 │    library_function()  library_function2()        │
 │    library_function3() callback_f()               │
 │    g() gg() fff() retstr() retstr_()              │
 │    print_lib() print_lib2() ret_struct_lib()      │
 │    ret_struct() ptrfunc() foo() boo()             │
 │                                                   │
 │  Calls to libsh2 via GOT:                         │
 │    lf() lf2() lf3() xxx() pl2() mm()             │
 │                                                   │
 │  Data from libsh1 (R_ARM_COPY):                   │
 │    aa=10  bb=0  s_lib                             │
 │                                                   │
 │  Data from libsh2 (R_ARM_COPY):                   │
 │    aaa=10 bbb=0                                   │
 └──────────────────────────────────────────────────┘
          ▲                          ▲
          │ lib→main refs            │ main→lib refs
          │ (GLOB_DAT/JUMP_SLOT)     │ (GLOB_DAT/JUMP_SLOT)
 ┌────────┴─────────┐      ┌────────┴─────────┐
 │     libsh1.so     │      │     libsh2.so     │
 │                   │      │                   │
 │  Needs from main: │      │  Needs from main: │
 │    AA, BB, f(),   │      │    AA, BB, w(),   │
 │    w(), s1,       │      │    s1             │
 │    ptrfunc(),     │      │                   │
 │    puts, printf,  │      │  Weak: mm()       │
 │    strcat, strlen │      │   (main's wins)   │
 │    log (math)     │      │                   │
 │                   │      │  Calls libsh1:    │
 │  Weak: foo, boo   │      │    (none direct)  │
 └───────────────────┘      └───────────────────┘
```

### 14.6 Test Coverage Matrix

| Loader feature | Test function | `libsh1` coverage | `libsh2` coverage |
|---------------|--------------|-------------------|-------------------|
| ELF header validation | `test_elf_reader()` | `.dynsym` hex dump | `.dynsym` hex dump |
| PT_LOAD segment loading | `test_Shlib_Loader()` → `load_bin()` | Segment print (vaddr, size, perms) | Same sequence |
| `.rel.dyn` parsing | `test_Shlib_Loader()` | 29 relocations | — |
| `R_ARM_RELATIVE` | `patch_shlib()` | `s_lib`, `str`, `str_` | — |
| `R_ARM_GLOB_DAT` | `patch_shlib()` | `AA`, `BB`, `s1`, `aa`, `bb`, etc. | `AA`, `BB`, `w`, `s1` |
| `R_ARM_JUMP_SLOT` | `patch_shlib()` | `printf`, `f`, `w`, `puts`, library fns | `printf` |
| `R_ARM_COPY` | `patch_main()` | `aa`, `bb` | `aaa`, `bbb` |
| Weak symbols | `patch_shlib()` / `patch_main()` + `resolve_weak_function()` | `foo()` returns 20, `boo()` returns 30 | `mm()` → main's version wins |
| Constructors | `execute_constructors()` | 4 constructors run in order | 1 constructor |
| `SymbolTable` interposition | `test_symbol_table()` | 10 symbols looked up | — |
| DT_NEEDED scan | `test_dependency_scan()` | No deps (expected) | No deps (expected) |
| Bounds validation | `test_bounds_validation()` | Undersized buffer test | — |
| Structured error strings | `test_structured_error_strings()` | All 9 codes printed | — |
| Graceful patch failure | `test_patch_counts()` | Patched count vs total | Patched count vs total |
| DSB/ISB barriers | `patch_shlib()` / `patch_main()` | Called after each patch | Called after each patch |

---

## 15. Porting to a Different STM32 / Cortex-M

### 19.1 Required changes

| Item | Where | What to change |
|------|-------|---------------|
| CPU flags | `Makefile` | `-mcpu`, `-mfloat-abi` |
| Flash origin/size | `ldscripts/mem-sh.ld` | `FLASH ORIGIN` / `LENGTH` |
| RAM origin/size | `ldscripts/mem-sh.ld` | `RAM ORIGIN` / `LENGTH` |
| Library RAM size | `libshN/ldscripts/mem.ld` | `RAM LENGTH` (must fit `.text` + `.data` + `.bss` + `.got`) |
| CMSIS device header | `include/timer.h` | Already includes `cmsis_device.h` — ensure the right one |
| HAL driver config | `include/stm32f4xx_hal_conf.h` | Enable/disable peripherals, adjust HSE_VALUE |
| Interrupt vectors | `system/src/cmsis/vectors_*.c` | Match your device's vector table |

### 14.2 Adding a new shared library

1. Create `libnew/` directory
2. Write source files using `-fPIC` conventions
3. Create `ldscripts/mem.ld` with appropriate RAM size
4. Create `ldscripts/gcc.ld` → `INCLUDE "mem.ld"` + `INCLUDE "sections.ld"`
5. Create `ldscripts/sections.ld` based on `libsh1/ldscripts/sections.ld`
6. Add build rules to `Makefile` (compile, link, objcopy, objdump, size)
7. Add to `APP_LDFLAGS`: `-Llibnew -lnew`
8. Add to `.PHONY` and `clean` targets

### 19.3 Adding a new source file to the main app

1. Add the `.c` or `.cpp` file to `C_SRCS` or `CXX_SRCS` in `Makefile`
2. The pattern rules `%.o: %.c` and `%.o: %.cpp` handle compilation automatically

---

## 16. Troubleshooting

### 19.1 "not a valid ELF ARM32 shared library"

- Verify the file is a 32-bit ARM ELF: `readelf -h file.so`
- Check `e_type == DYN (3)` (shared object)
- Check `e_machine == ARM (0x28)`
- Check the file is little-endian (ARM default)

### 19.2 Linker reports undefined symbol

If the main app fails to link:
```console
undefined reference to `library_function'
```

You forgot to add `-lshN` to `APP_LDFLAGS`, or the library does not export the symbol.
Run `readelf -s libshN/libshN.so | grep library_function` to check.

### 19.3 Library loads but crashes when called

Possible causes:
- **GOT not patched**: `patch_shlib()` was not called or failed silently
- **Wrong buffer address**: `load_bin()` placed segments at wrong offset
- **Missing `.rel.dyn` section**: the library has no relocation entries
  (check with `readelf -r libshN.so`)
- **Constructor crash**: a constructor function crashes (hard fault).
  Add `printf` at the start of `execute_constructors()` to isolate.
- **Stack overflow**: the loaded library's function calls consume too much stack.
  Increase `__Main_Stack_Size` in the linker script.

### 19.4 "section not found!" at runtime

The loader cannot find `.dynsym` or `.rel.dyn` in the loaded library:
- Verify the library was linked with `-shared` (type `ET_DYN`)
- Verify the library actually exports symbols: `readelf -S libshN.so | grep dyn`
- Check the section name string table is intact: `readelf -p .shstrtab libshN.so`

### 19.5 Global variable has wrong value

If `*(unsigned int*)aa` reads the wrong value:
- Check if `R_ARM_COPY` is being used (it should be for variables)
- Verify the library's `.data` section was loaded correctly
- Use `readelf -r f411ce-sh.elf | grep aa` to see if the relocation type is `R_ARM_COPY`
- Dump the GOT contents to verify the patched address

### 19.6 RWX linker warning

If you still see `warning: has a LOAD segment with RWX permissions`:

For shared libraries: add `-Wl,--no-warn-rwx-segments` to the linker flags.
For main executable: add `-Wl,--no-warn-rwx-segments` to `APP_LDFLAGS`.

The warning is benign on Cortex-M (no MMU) — see §12.

---

## 17. Reference: ELF32 API Reference

### 19.1 `ElfReader` class

| Method | Returns | Description |
|--------|---------|-------------|
| `open_elf(path)` | `bool` | Opens ELF file, reads + verifies header |
| `verify_elf()` | `LoaderError` | Validates magic/class/endian/type/machine |
| `get_header()` | `const Elf32_Ehdr&` | Returns cached ELF header |
| `read_program_header(nbr, phdr)` | `void` | Reads program header `nbr` |
| `read_section_header(nbr, shdr)` | `void` | Reads section header `nbr` |
| `read_section_content(nbr, dest)` | `LoaderError` | Reads section data into span |
| `get_binary_size()` | `uint32_t` | Max `p_vaddr + p_memsz` across LOAD segments |
| `find_section_by_name(name)` | `uint32_t` | Section index, or 0 |
| `find_program(type)` | `uint32_t` | Program index, or 0xFFFFFFFF |
| `get_program_header_count()` | `uint32_t` | Number of PHDRs |
| `get_segment_info(nbr, seg)` | `void` | Fills `LoadSegment` struct |
| `flags_to_string(flags)` | `const char*` | Returns `"RWX"` style string |
| `load_segments(buf, phdr)` | `LoaderError` | Loads one PT_LOAD, with bounds check |
| `read_helper_sections(name, hs)` | `void` | Fills HelperSection from section header |
| `read_relocation_symbols(pa, ssp, &count)` | `LoaderError` | Parses `.rel.dyn`, validates indices |
| `read_dynamic_symbols(pa, ssp, start_k, &count)` | `uint32_t` | Appends STB_WEAK symbols |
| `calculate_patch_array_size(pa)` | `uint32_t` | Total slots needed for patches |
| `find_symbol_address(name, ms)` | `uint32_t` | Hash-based lookup in SectionMap |
| `scan_dependencies(deps_out)` | `LoaderError` | Reads DT_NEEDED from PT_DYNAMIC |

### 19.2 `ShlibLoader` class

| Method | Returns | Description |
|--------|---------|-------------|
| `load_bin(buffer, bufsize)` | `LoaderError` | Loads LOAD segments, scans DT_NEEDED |
| `get_elf()` | `ElfReader&` | Access underlying reader |
| `read_elf(path)` | `LoaderError` | Opens + verifies ELF |
| `populate_helper_sections()` | `LoaderError` | Builds m_shlib/m_main from sections + _reloc |
| `get_ram_shlib()` / `get_ram_main()` | `uint8_t*` | Base addresses |
| `get_shlib_patch_count()` | `uint32_t` | Symbols needing patching in library |
| `get_main_patch_count()` | `uint32_t` | Symbols needing patching in main |
| `find_shlib_symbols_to_patch(ssp)` | `LoaderError` | Fills patch array for library |
| `find_main_symbols_to_patch(msp)` | `LoaderError` | Fills patch array for main |
| `allocate_patch_arrays(ssp_shlib, ssp_main)` | `void` | Sets internal pointer |
| `patch_shlib(global_syms)` | `uint32_t` | Patches library GOT; returns patched count |
| `patch_main(global_syms)` | `uint32_t` | Patches main GOT; returns patched count |
| `execute_constructors()` | `LoaderError` | Runs `.init_array` |
| `resolve_weak_function(f)` | `unsigned int` | Lazy weak function resolution |
| `resolve_dependencies(global_syms, load_cb)` | `LoaderError` | Loads DT_NEEDED libs via callback |
| `set_ram_main(addr)` | `void` | Sets main RAM base for _reloc fixup |

### 19.3 `SymbolTable` class

| Method | Description |
|--------|-------------|
| `add_global(name, addr)` | Registers symbol; first-write-wins |
| `add_weak(name, addr)` | Registers weak symbol |
| `find(name)` | Returns address or 0 |
| `has(name)` | Returns true if non-weak exists |
| `clear()` | Resets table |

---

## 18. Reference: ELF32 Structures in Use

### 19.1 ELF Header (`Elf32_Ehdr`)

```
Offset  Size  Field           Meaning
0x00    4     e_ident[0..3]   Magic: 0x7F 'E' 'L' 'F'
0x04    1     e_ident[4]      ELFCLASS32 (1)
0x05    1     e_ident[5]      ELFDATA2LSB (1)
0x10    2     e_type          ET_DYN (3) for shared libraries
0x12    2     e_machine       EM_ARM (40 / 0x28)
0x1C    4     e_phoff         Offset to program header table
0x20    4     e_shoff         Offset to section header table
0x2C    2     e_ehsize        Size of this header (52 bytes)
0x2E    2     e_phentsize     Size of program header entry (32 bytes)
0x30    2     e_phnum         Number of program headers
0x32    2     e_shentsize     Size of section header entry (40 bytes)
0x34    2     e_shnum         Number of section headers
0x36    2     e_shstrndx      Section header string table index
```

### 19.2 Program Header (`Elf32_Phdr`)

```
Offset  Size  Field    Meaning
0x00    4     p_type   PT_LOAD (1), PT_DYNAMIC (2)
0x04    4     p_offset File offset of segment data
0x08    4     p_vaddr  Virtual address (load address relative to buffer base)
0x0C    4     p_paddr  Physical address
0x10    4     p_filesz Size in file
0x14    4     p_memsz  Size in memory (>= filesz; zero-fill difference)
0x18    4     p_flags  PF_R (4), PF_W (2), PF_X (1) — bitmask
0x1C    4     p_align  Alignment constraint
```

### 19.3 Section Header (`Elf32_Shdr`)

```
Offset  Size  Field       Meaning
0x00    4     sh_name     Index into .shstrtab
0x04    4     sh_type     SHT_PROGBITS (1), SHT_DYNSYM (11), SHT_REL (9), etc.
0x08    4     sh_flags    SHF_WRITE (1), SHF_ALLOC (2), SHF_EXECINSTR (4)
0x0C    4     sh_addr     Virtual address (if SHF_ALLOC)
0x10    4     sh_offset   File offset
0x14    4     sh_size     Section size
0x18    4     sh_link     Related section index (e.g. linked .dynstr for .dynsym)
0x1C    4     sh_info     Extra info
0x20    4     sh_addralign Alignment
0x24    4     sh_entsize  Entry size (for tables)
```

### 19.4 Relocation Entry (`Elf32_Rel`)

```
Offset  Size  Field    Meaning
0x00    4     r_offset Address (GOT slot or location to patch)
0x04    4     r_info   Symbol index (24 bits) + type (8 bits)
                        ELF32_R_SYM(r_info) = r_info >> 8
                        ELF32_R_TYPE(r_info) = r_info & 0xFF
```

### 19.5 Symbol Table Entry (`Elf32_Sym`)

```
Offset  Size  Field      Meaning
0x00    4     st_name    Index into .dynstr
0x04    4     st_value   Symbol value (address or offset within library)
0x08    4     st_size    Symbol size
0x0C    1     st_info    Binding (high 4 bits) + type (low 4 bits)
                         ELF32_ST_BIND(st_info) = st_info >> 4
                             0 = STB_LOCAL  1 = STB_GLOBAL  2 = STB_WEAK
0x0D    1     st_other   Visibility
0x0E    2     st_shndx   Section index
```

---

## 19. Reference: Linker Scripts

### 19.1 Library linker scripts

`libshN/ldscripts/gcc.ld`:
```
INCLUDE "mem.ld"
INCLUDE "sections2.ld"
```

`libshN/ldscripts/mem.ld`:
```
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35k }
```

`libshN/ldscripts/sections2.ld`:
```
SECTIONS
{
    .inits : { KEEP(*(.init*)) KEEP(*(.fini*)) ... } > RAM
    .plt    : { *(.plt) } > RAM
    .got    : { _lsgot = .; *(.got*); _elsgot = .; } > RAM
    .got.plt : { _lsgot_plt = .; *(.got.plt*); _elsgot_plt = .; } > RAM
    .rodata : { *(.rodata*) } > RAM
    .text   : { *(.text*) *(vtable) } > RAM
    .ARM.exidx : { *(.ARM.exidx*) } > RAM
    .data   : { *(.data*) } > RAM
    .bss    : { *(.bss*) *(COMMON) } > RAM
    .noinit : { *(.noinit*) } > RAM
}
```

### 19.2 Main application linker scripts

`ldscripts/mem-sh.ld`:
```
MEMORY
{
  RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
}
```

`ldscripts/sections-sh.ld` (key parts):
```
SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .inits : {
        _reloc = .;
        LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
        LONG(LOADADDR(.dynsym));  LONG(SIZEOF(.dynsym));
        LONG(LOADADDR(.dynstr));  LONG(SIZEOF(.dynstr));
        LONG(LOADADDR(.hash));    LONG(SIZEOF(.hash));
        LONG(LOADADDR(.inits));   LONG(SIZEOF(.inits));
        KEEP(*(.init_array*))
        KEEP(*(.fini_array*))
    } > FLASH

    .text : { *(.text*) *(.rodata*) } > FLASH

    .dynsym : { *(.dynsym*) } > FLASH
    .dynstr : { *(.dynstr*) } > FLASH
    .hash   : { *(.hash*)   } > FLASH
    .rel.dyn : { *(.rel.dyn*) } > FLASH

    .got : {
        _sgot = .; *(.got*); _esgot = .;
    } > RAM AT> FLASH

    .got.plt : {
        _sgot_plt = .; *(.got.plt*); _esgot_plt = .;
    } > RAM AT> FLASH

    .data : { *(.data*) } > RAM AT> FLASH
    .bss  : { *(.bss*) *(COMMON) } > RAM
}
```

The `AT>` directive means the initial values are stored in FLASH but the section will be
used at its RAM address. This is how `.data`, `.got`, and `.got.plt` get their initial
values from flash at startup while being writable at runtime.

---

## 20. Loading C++ Shared Libraries (PLT and `.rel.plt`)

C++ shared libraries introduce challenges that C-only libraries avoid: virtual functions,
operator new/delete, and exception support all generate **PLT (Procedure Linkage Table)**
relocations (`.rel.plt`) that the basic loader does not handle.

### 20.1 The Problem

The core loader (`shlib_loader`) processes only `.rel.dyn` — the section that holds data
relocations (`R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_ABS32`). Function calls from the
shared library to external symbols (including other functions within the same `.so`) go
through the PLT, which uses `R_ARM_JUMP_SLOT` relocations in `.rel.plt`. These entries
were never patched, causing the CPU to jump to the PLT resolver stub — which expects an
OS dynamic linker that does not exist on bare-metal — resulting in a hard fault.

### 20.2 Fixes Applied

| Fix | File | Why |
|-----|------|-----|
| Remove local `malloc`/`free` | `new-lib/cpp_features.cpp` | These are provided by the firmware (newlib); duplicating them causes symbol conflicts |
| Add thin `operator new`/`delete` wrappers | `new-lib/cpp_features.cpp` | The `.so` uses `new`/`delete`; wrappers call `malloc`/`free` via PLT instead of pulling in 61 KB of libsupc++ |
| Link `.so` with `-nostdlib` | `CMakeLists.txt` | Prevents the linker from dragging in libsupc++ exception handling (200+ text symbols, 61 KB of `.text`) |
| Remove custom linker script | `CMakeLists.txt` | The custom `gcc.ld` limited RAM to 35 KB; the default script has no such restriction |
| Remove manual `R_ARM_RELATIVE` loop | `src/main.cpp` | This was a partial relocation that only handled type 23; replaced by `patch_shlib()` |
| Call `patch_shlib()` with symbol table | `src/main.cpp` | The loader's `patch_shlib()` handles all `.rel.dyn` entries including `R_ARM_ABS32` for vtables |
| Add `REL_PLT` enum entry | `include/static/elf-reader.h` | Adds `.rel.plt` to the helper section enum so the loader can locate the PLT relocation table |
| Process `.rel.plt` in loader | `src/static/shlib-loader.cpp` | `find_shlib_symbols_to_patch()` now reads `.rel.plt` after `.rel.dyn`; entries are patched by `patch_shlib()` alongside GLOB_DAT/JUMP_SLOT |
| Provide `malloc`/`free` addresses | `src/main.cpp` | External symbols are resolved via a `symbol_table` populated with firmware addresses, passed to `patch_shlib()` |

### 20.3 `.rel.plt` Patching in the SCL Loader

The `.rel.plt` handling is integrated into the SCL loader (`shlib-loader.cpp`) rather than in test code:

1. **Enum extension** (`elf-reader.h:56-64`): `helper_section_idx` gains `REL_PLT = 2`, bumping `COUNT` to 6.
2. **Section discovery** (`shlib-loader.cpp:27-34`): `.rel.plt` is added to the `helper_sect` name array. `populate_helper_sections()` auto-discovers it in the loaded library.
3. **Allocation sizing** (`shlib-loader.cpp:145-160`): `get_shlib_patch_count()` adds `.rel.plt` entry count to the patch array allocation.
4. **Patch array population** (`shlib-loader.cpp:184-211`): `find_shlib_symbols_to_patch()` calls `read_relocation_symbols()` a second time with `.rel.plt` data, appending entries after `.rel.dyn`.
5. **Patching** (`shlib-loader.cpp:262-397`): `patch_shlib()` processes the combined array. JUMP_SLOT entries fall into the `STB_GLOBAL`/`STB_WEAK` handler, which resolves both self-defined (`st_value + base`) and external (via `symbol_table`) symbols. No special-case code needed.

### 20.4 Requirements for C++ `.so` Files

- **Compile** with `-fPIC -fno-exceptions -fno-rtti -fno-threadsafe-statics`
- **Link** with `-shared -nostartfiles -nostdlib` (prevents libsupc++ inclusion)
- **Provide** your own `operator new`/`delete` wrappers that call `malloc`/`free` (unless you want 61 KB of exception handling code)
- **Provide** `__cxa_pure_virtual` as an infinite loop stub
- **Avoid** `std::` containers and exceptions (they require full libstdc++)

### 20.5 Size Comparison

| Build | `.text` size | Text symbols |
|-------|-------------|--------------|
| With libsupc++ (default) | 61,094 B | 200 |
| With `-nostdlib` + stubs | 3,835 B | 9 |


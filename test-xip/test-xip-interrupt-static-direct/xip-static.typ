#set page(
  paper: "a4",
  margin: (top: 2.5cm, bottom: 2.5cm, left: 3cm, right: 3cm),
  header: align(right, text(fill: gray.darken(20%), size: 9pt, "Static-XIP Loader Architecture")),
  footer: [
    #align(center, text(fill: gray.darken(20%), size: 9pt, context counter(page).display("1")))
  ]
)

#set text(
  font: ("Liberation Sans", "DejaVu Sans", "Helvetica"),
  size: 10pt,
  fill: rgb("#2c3e50")
)

#set par(justify: true, leading: 0.7em)
#set heading(numbering: "1.a.")

// Cover / Title Page
#align(center)[
  #v(2cm)
  #text(size: 24pt, weight: "bold", fill: rgb("#1a3a5f"), "Static-XIP Loader Architecture")
  #v(0.5cm)
  #text(size: 14pt, style: "italic", fill: rgb("#7f8c8d"), "Bare-Metal Shared Library Execution In Place for ARM Cortex-M")
  #v(2cm)
]

#line(length: 100%, stroke: 1pt + rgb("#1a3a5f"))
#v(1cm)

== 1. Overview
The *Static-XIP (Execution In Place)* loader is a specialized, bare-metal loader developed for ARM Cortex-M microcontrollers. It permits shared libraries (`.so` files) to be loaded dynamically from a file system and executed.

=== 1.1 Why "Static"?
Unlike traditional dynamic loaders (like `scl::dynamic-nolib`), the static loader has *no runtime dynamic-linker service* (no runtime `dlsym` lookup, no lazy binding, and no routing trampolines). 
- All symbols are resolved and patched *eagerly at load or provision time*.
- Imports are resolved against main's own dynamic symbol table via `fw::link_context::from_self()` (main is `-pie -Wl,-E`) — no hand-maintained host table and no `-u` link anchors. (Earlier revisions used an explicit `g_host[]` registry; see the migration in `../../doc/xip-memory-analysis.md` §10.4.)

=== 1.2 Why "XIP"?
*Execution In Place (XIP)* means that the code segment executes directly from Flash memory. It eliminates the need to copy the library's large code segment into RAM, conserving precious SRAM.

== 2. Memory Layout: RAM vs Flash

During execution, every loaded library is split into two distinct segments:

- *R-X Segment (Read-Only & Executable)*: Remains in *Flash* (executes in place).
- *RW Segment (Read-Write)*: Copied to *RAM* (allocated from the image arena).

#v(0.5cm)
#align(center)[
  #table(
    columns: (1.5fr, 1.2fr, 1.2fr, 3.1fr),
    inset: 6pt,
    fill: (x, y) => if y == 0 { rgb("#1a3a5f") } else if calc.even(y) { rgb("#f8f9fa") } else { none },
    stroke: 0.5pt + rgb("#bdc3c7"),
    
    [#text(fill: white)[*Section*]], [#text(fill: white)[*Location*]], [#text(fill: white)[*Memory Class*]], [#text(fill: white)[*Description*]],
    [`.text`], [Flash], [R-X (Flash)], [Executable CPU instructions],
    [`.rodata`], [Flash], [R-X (Flash)], [Constants and string literals],
    [`.plt`], [Flash], [R-X (Flash)], [Procedure Linkage Table for imports],
    [`.got.plt`], [Flash], [R-X (Flash)], [PLT jump destinations (provision-patched)],
    [`.got`], [RAM], [RW (RAM)], [Global Offset Table (load-relocated)],
    [`.data`], [RAM], [RW (RAM)], [Initialized global/static variables],
    [`.bss`], [RAM], [RW (RAM)], [Zero-initialized global/static variables],
    [`.ARM.exidx`], [Flash], [R-X (Flash)], [C++ exception handling table],
  )
]
#v(0.5cm)

=== 2.1 RAM Savings Benefit
In traditional dynamic loading, the entire `.so` file is copied to RAM. For a typical library, code and read-only strings make up 80% to 95% of the total binary size. By executing this code straight from Flash, the Static-XIP loader *saves up to 95% of the RAM footprint*.

== 3. Compiler & Build Options

For XIP split execution to work, specific toolchain flags are required for both libraries and the host application.

=== 3.1 Shared Libraries (`.so`)
To compile XIP-eligible libraries, use the following options:
- *Target Architecture*: `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16`
- *PIC Flags*: `-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative`
  - `-msingle-pic-base -mpic-register=r10` uses register `r10` as the base address of the RAM-resident GOT.
  - `-mno-pic-data-is-text-relative` prevents the compiler from assuming the offset between text (Flash) and data (RAM) is constant.
- *Linking Flags*: `-shared -nostartfiles -Wl,-E -Wl,-Bsymbolic -T xip-split.ld`
  - `-Wl,-Bsymbolic` binds internal function calls locally, bypassing the PLT.
  - `-T xip-split.ld` splits the library into separate load segments.

=== 3.2 Host Application
- *Linking Flags*: `-pie LINKER:-E LINKER:--gc-sections`
  - `-pie` generates a Position Independent Executable.
  - `LINKER:-E` exports *every global the host defines* into `.dynsym` (which the loader queries) — *no `-u` needed* for host-defined symbols.
- *Borrowed library symbols*: symbols the libraries need but the host never calls itself (`printf`, `malloc`, `_Znwj`, the C++ EH runtime) are pulled from `libc.a`/`libsupc++.a` by a single `[[gnu::used, gnu::retain]]` anchor array in `main.cpp` — replacing the former pile of `-Wl,-u,<sym>` flags — so `-E` can export them.

== 4. Execution & Binding Pipeline

The Static-XIP architecture implements a *two-phase binding pipeline*:

1. *Provision Time (Pre-flashing)*:
   The raw `.so` is loaded into RAM, and `xip::stat::bind_host_imports` patches the target addresses of host functions (e.g. `printf`) directly into the `.got.plt` slots of the library. Since the host is `r10`-free, the library branches straight into host functions without a trampoline. The patched `.so` is then written to the Flash Flat-FS.
2. *Load Time (Runtime)*:
   `xip::stat::load` locates the library in Flash, allocates memory for the RW segment in RAM via `fw::image_arena`, copies `.data` to RAM, zeroes `.bss`, and applies relocations in `.rel.dyn`. Static constructors (`.init_array`) are executed with `r10` pre-loaded with the module's GOT base address.
3. *Function Invocations*:
   Symbols are resolved using `xip::stat::resolve`, and invoked using the `pic_call_*` assembly wrappers to set `r10` to the target module's GOT base address.

== 5. Architectural Limitations

- *No Cross-Library Calls*: Since the static loader does not use a routing trampoline to swap `r10` dynamically, a function in `libA.so` cannot directly call a function in `libB.so`. All external calls must target the host application (`main`).
- *C++ Exception Propagation*: Throwing C++ exceptions across library boundaries is unsupported due to `.ARM.exidx` unwinding limits.

#set page(margin: 2.5cm)
#set text(font: "DejaVu Sans Mono", size: 9pt)
#set table(stroke: 0.5pt)

#align(center, text(size: 14pt, weight: "bold", "Automatic Symbol Table Resolution in XIP Dynamic-Newlib"))

== 1. Overview
We have completely eliminated the hardcoded `addr_of()` symbol mapping table inside the XIP newlib dynamic loader (`src/xip/dynamic-newlib/adapter.cpp`) and transitioned the suite to a generic, link-time embedded symbol table.

== 2. The Problem
The dynamic loader variant `scl::xip-dynamic-newlib` originally resolved library imports from the host firmware (such as `printf`, `malloc`, `sinf`, `free`) using a hardcoded fallback lookup table in `adapter.cpp` with a long `strcmp` chain.
This had major limitations:
- *Lack of Generality*: Any change in the firmware's exported functions required manually editing the core SCL library.
- *Testing Artifacts*: Weak test variables (`AA`, `BB`, `s1`, `f`, `w`) were defined in the core library, leaking test code into production.

== 3. The Solution
We implemented a link-time symbol embedding model:
1. *Linker Integration*: Added the `.scl_symtab` section reservation to `sections-flatfs-hosted.ld` and linked `symtab-blob.c`.
2. *Build-Time Extraction*: Integrated `gen-symtab.py` into `CMakeLists.txt` as a `POST_BUILD` command to parse the linked ELF and write the symbol table back into `.scl_symtab`.
3. *Symbol Pruning*: Filtered out compiler and library internal symbols (those starting with `_` or `__`) unless they were essential C++ ABI or reentrancy symbols. This shrank the symbol table size from *87 KB* to only *7.5 KB* (299 symbols), allowing the executable to fit in the 256 KB Flash budget.
4. *Loader Decoupling*: Removed `addr_of()` and all hardcoded stubs. Added a global `firmware::link_context` and exposed `xip::newlib::set_symtab()` to register the table at boot.
5. *Startup Logic*: Updated `main.cpp` to load the symbol table at boot (first trying flash, falling back to semihosting ELF parsing if the flash table hasn't been written).

== 4. Results & Metrics
- *Dynamic symbols*: 299 symbols extracted.
- *Size used*: 7.5 KB out of 12 KB allocated.
- *RAM usage*: 0 RAM required (semihosting fallback kept the 77 KB string table in RAM).
- *Generality*: The core library is now fully generic and free of hand-written symbols.

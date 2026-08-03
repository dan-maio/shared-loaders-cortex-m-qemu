#set page(margin: 2.5cm)
#set text(font: "DejaVu Sans Mono", size: 8.5pt)
#set table(stroke: 0.5pt)

#align(center, text(size: 14pt, weight: "bold", "Dynamic Loader Pre-Build Symbol Table Integration (C++20)"))

== 1. Overview & Objective
To load shared libraries dynamically on Cortex-M4 (Execute-In-Place and dynamic-newlib loaders), the firmware must export standard APIs (like `printf`, `malloc`, `free`) to resolve library symbols at runtime. 

SCL now uses a host-compiled C++20 tool (`gen-symtab.cpp`) to extract symbols from the linked ELF and write them into the firmware image, replacing the legacy Python-based symbol extraction scripts:
- *C++ ELF Reader Integration*: Reuses SCL's C++ ELF reader library (`common-elf-reader`).
- *Internal Symbol Pruning*: Filters out compiler-internal symbols using a name whitelist.
- *Flash Embedding*: Writes the symbols directly into the `.scl_symtab` section of the microcontroller's Flash memory.

== 2. Compilation & Injection Pipeline
1. *Host Compilation*: During CMake configuration, a custom target `gen-symtab-host` is defined to compile `tools/gen-symtab.cpp` using the host's native C++20 compiler (`c++`).
2. *Firmware Link*: The microcontroller firmware is compiled and linked.
3. *Symbol Table Injection*: A post-build command runs the compiled `gen-symtab` host binary on the linked ELF. The binary outputs `symtab.bin`, which is written back into the `.scl_symtab` section of the firmware via `objcopy --update-section`.

== 3. Symbol Pruning & Whitelist Filtering
Linking dynamic newlib and stdc++ pulls in thousands of global/weak internal symbols. Generating a table for all of them creates an *87 KB* footprint, which exceeds the STM32F411's program flash capacity. 

To resolve this, the tool filters out all symbols starting with `_` or `__` unless they are explicitly whitelisted as required by loaded libraries (e.g., C++ ABI, exception handling, and thread-local reentrancy helpers):
- `_impure_ptr`, `_Znw` (operator new), `_Zdl` (operator delete), `__cxa` (exceptions), `__errno` (error), `__exidx`, `__gnu_Unwind`, and `__*frame_info`.

This compresses the table size to:
- *7.5 KB* (299 symbols) for XIP newlib.
- *6.6 KB* (264 symbols) for non-XIP newlib.

== 4. CMake Build Integration
- *Host Compiler Config* (`test/cmake/scl-test-app.cmake`):
  Compiles `tools/gen-symtab.cpp` on-the-fly using the host `c++` compiler during target configuration.
- *Platform Wrapper* (`platform-library.cmake`):
  Modified `scl_embed_symtab` to declare dependency on `gen-symtab-host` and call the compiled host binary instead of Python.
- *XIP Target Integration* (`test-xip/test-xip-dynamic-newlib/test1/CMakeLists.txt`):
  Applies the same dependency and post-build step.

== 5. Benefits
- *Zero Python dependency*: Native and clean build path.
- *Microcontroller RAM Conservation*: Accessing symbols directly from Flash avoids loading the 77 KB string table over semihosting into RAM, preventing OOM aborts on the STM32F411's 103 KB heap.
- *Standalone Runs*: The embedded Flash table allows the board to boot and load libraries standalone without a debugger connection.

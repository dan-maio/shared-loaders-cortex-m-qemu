# Dynamic Loader Pre-Build Symbol Table Integration (C++20)

This document describes the native, host-compiled C++20 symbol table generator used to extract and embed SCL firmware symbols into the microcontroller's Flash memory at build time, replacing the legacy Python-based symbol extraction scripts.

---

## 1. Overview & Objective

To load shared libraries dynamically on Cortex-M4 microcontrollers with Execute-In-Place (XIP) and standard dynamic-newlib loaders, the firmware must export standard APIs (like `printf`, `malloc`, `free`) to resolve library symbols at runtime. 

Previously, SCL used a Python script (`gen-symtab.py`) to extract symbols from the linked ELF and write them into the firmware image. This document defines the architecture of the new host-compiled C++20 replacement (`gen-symtab.cpp`), which:
1. **Reuses SCL's C++ ELF reader library** (`common-elf-reader`) to parse the firmware ELF file.
2. **Filters out compiler-internal symbols** using a name whitelist.
3. **Packs the resolved symbols** into a binary table embedded directly inside the `.scl_symtab` section of the microcontroller's Flash memory.

---

## 2. Compilation Pipeline

```
┌─────────────────────────────────────┐
│ Compile gen-symtab host tool        │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ Link Firmware Target                │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ CMake POST_BUILD Step               │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ gen-symtab parses Firmware ELF      │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ Prune internal symbols (compact)    │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ Generate symtab.bin                 │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ objcopy --update-section .scl_symtab│
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ Final Flash-Ready ELF               │
└─────────────────────────────────────┘
```

### Build-Time Stages
1. **Host Compilation**: When configuring the CMake build, a custom target `gen-symtab-host` is defined to compile `tools/gen-symtab.cpp` using the host's native C++20 compiler (`c++` / `g++`).
2. **Firmware Link**: The microcontroller firmware (`scl-fw-dynamic-newlib1`, `scl-fw-xip-newlib1`, etc.) is compiled and linked.
3. **Symbol Table Injection**: A post-build command runs the compiled `gen-symtab` host binary, passing the linked ELF file. The binary outputs `symtab.bin`, which is written back into the `.scl_symtab` section of the firmware via `objcopy --update-section`.

---

## 3. Host Tool & Whitelist Filtering

The host tool source code is located at [`tools/gen-symtab.cpp`](file:///home/dan/Documents/shared-loaders-weact_agy/tools/gen-symtab.cpp) and relies on the SCL `loader::elf_image` parser.

### Symbol Pruning Logic
Linking dynamic newlib and stdc++ pulls in thousands of global/weak internal symbols. Generating a table for all of them creates an **87 KB** footprint, which exceeds the STM32F411's program flash capacity. 

To resolve this, the tool filters out all symbols starting with `_` or `__` unless they are explicitly whitelisted as required by loaded libraries (e.g. C++ ABI, exception handling, and thread-local reentrancy helpers):
* `_impure_ptr`, `_Znw` (operator new), `_Zdl` (operator delete), `__cxa` (exceptions), `__errno` (error), `__exidx`, `__gnu_Unwind`, and `__*frame_info`.

This compresses the table size to:
* **7.5 KB** (299 symbols) for XIP newlib.
* **6.6 KB** (264 symbols) for non-XIP newlib.

---

## 4. CMake Build Integration

The tool compiles automatically on-the-fly and integrates with the build paths:

*   **Host Compiler Config**: Added to [`test/cmake/scl-test-app.cmake`](file:///home/dan/Documents/shared-loaders-weact_agy/test/cmake/scl-test-app.cmake):
    ```cmake
    add_custom_command(
        OUTPUT "${SCL_HOST_GEN_SYMTAB}"
        COMMAND c++ -std=c++20
                "${SCL_ROOT}/tools/gen-symtab.cpp"
                "${SCL_ROOT}/common-elf-reader/src/elf-image.cpp"
                "-I${SCL_ROOT}/common-elf-reader/include"
                -o "${SCL_HOST_GEN_SYMTAB}"
        ...)
    add_custom_target(gen-symtab-host DEPENDS "${SCL_HOST_GEN_SYMTAB}")
    ```
*   **Platform Wrapper Integration**: In [`platform-library.cmake`](file:///home/dan/Documents/shared-loaders-weact_agy/test/platforms/weact-f411ce-hosted/cmake/platform-library.cmake), we updated `scl_embed_symtab` to declare dependency on `gen-symtab-host` and run the binary post-build.
*   **XIP Target Integration**: Added the same dependency and post-build step in [`CMakeLists.txt`](file:///home/dan/Documents/shared-loaders-weact_agy/test-xip/test-xip-dynamic-newlib/test1/CMakeLists.txt).

---

## 5. Benefits

1. **No External Dependencies**: Eliminates the Python interpreter dependency at build time.
2. **Microcontroller RAM Conservation**: Running the symbols directly from Flash avoids having to load the 77 KB string table over semihosting into RAM, preventing Out-Of-Memory aborts on the STM32F411 (which has only a 103 KB heap).
3. **Standalone Board Runs**: The embedded Flash table allows the board to boot and load libraries standalone without a debugger connection.

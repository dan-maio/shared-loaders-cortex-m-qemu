# Refactoring Summary: Clean Dynamic Symbol Resolution Fallback

The hardcoded `strcmp` symbol mapping in `scl::dynamic-newlib` and `scl::xip-dynamic-newlib` loaders has been refactored. The core SCL library is now completely generic, and the firmware-specific symbol fallback mapping has been successfully moved to the application layer.

## Architectural Change

Previously, both `src/dynamic-newlib/dynamic-linker.cpp` and XIP's `src/xip/dynamic-newlib/adapter.cpp` contained a hardcoded `addr_of` function implementing a large `strcmp` chain for system functions (e.g. `printf`, `malloc`), math helpers, C++ exception runtimes, and local test variables. This violated the library's design neutrality.

### New Design

1. **Weak Symbol Decoupling:** The loaders now declare and call a weak global function:
   ```cpp
   extern "C" __attribute__((weak)) void* scl_lookup_firmware_symbol(const char* name) {
       return nullptr;
   }
   ```
2. **Generic Loaders:** All hardcoded string mappings and local `extern "C"` math/runtime stubs have been completely removed from the library sources:
   - [dynamic-linker.cpp](file:///home/dan/Documents/shared-loaders-weact_agy/src/dynamic-newlib/dynamic-linker.cpp)
   - [adapter.cpp](file:///home/dan/Documents/shared-loaders-weact_agy/src/xip/dynamic-newlib/adapter.cpp)
3. **Application Responsibility:** The firmware/application targets that compile the loaders define `scl_lookup_firmware_symbol` to provide the fallback symbol mapping. This keeps target-specific knowledge at the application/platform layer.

---

## Code Modification Diffs

### 1. Library: `src/dynamic-newlib/dynamic-linker.cpp`
* Removed 40+ hardcoded `strcmp` checks, C++ stub operators, math library externs, and weak variables.
* Added weak hook `scl_lookup_firmware_symbol` and replaced all calls to `addr_of` with it.

### 2. Library: `src/xip/dynamic-newlib/adapter.cpp`
* Replaced the local `addr_of` table with the weak hook `scl_lookup_firmware_symbol`, preserving the XIP-specific cross-call bindings.

### 3. Application: `dl-impl.cpp`, `main-no-dyn.cpp` & `main.cpp`
Defined `scl_lookup_firmware_symbol` on the application side to preserve backwards-compatibility for existing tests. Additionally, default weak testing variables (`AA`, `BB`, `s1`, `f`, `w`, and `hf_pi`) that were previously inside SCL were moved and initialized in the application layer (within `dl-impl.cpp` / `main.cpp` / `main-no-dyn.cpp` as appropriate) so they resolve correctly during test runs:
* [dl-impl.cpp](file:///home/dan/Documents/shared-loaders-weact_agy/test/platforms/board-hosted/vendor/firmware/src/dl-impl.cpp) (test-dynamic firmware app facade)
* [main-no-dyn.cpp](file:///home/dan/Documents/shared-loaders-weact_agy/test/platforms/board-hosted/vendor/firmware/src/main-no-dyn.cpp) (no-dyn/static test app facade)
* [main.cpp](file:///home/dan/Documents/shared-loaders-weact_agy/test-xip/test-xip-dynamic-newlib/test1/src/main.cpp) (XIP newlib test app)

Additionally, `LINKER:-E` was enabled in `CMakeLists.txt` for the dynamic-newlib test targets so the linker correctly builds a `.dynsym` table in the executable when compiling for dynamic execution.

# `--gc-sections` in Embedded Firmware with Loaded Libraries

## Overview

`--gc-sections` is a linker flag that removes unused sections from the final binary. When combined with `-ffunction-sections` and `-fdata-sections` compiler flags, it can significantly reduce binary size by eliminating dead code.

However, when firmware loads libraries at runtime (via dlopen, static patching, or XIP), `--gc-sections` can cause problems by removing symbols that the loaded libraries need but the firmware doesn't directly reference.

---

## How It Works

### Compile Time

With `-ffunction-sections` and `-fdata-sections`, the compiler places each function and variable in its own section:

```c
// Without flags: everything goes into .text
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

// With flags: each function gets its own section
// .text.add
// .text.mul
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
```

### Link Time

The linker starts from entry points (like `main()`, interrupt vectors) and marks all reachable sections. Unreachable sections are discarded.

```
main() → calls add() → .text.add is KEPT
         calls mul() → .text.mul is KEPT
         never calls div() → .text.div is DISCARDED
```

---

## The Problem with Loaded Libraries

### Scenario

Firmware exports `printf` for loaded libraries to use:

```c
// firmware.c
extern int printf(const char*, ...);  // from libc

// Never called directly by firmware, but libraries need it
```

### Without `--gc-sections`

```
┌─────────────────────────────────────────────┐
│ Firmware Binary                             │
├─────────────────────────────────────────────┤
│ .text.main    ✓ (firmware code)             │
│ .text.printf  ✓ (libc, kept)               │
│ .text.sinf    ✓ (libm, kept)               │
│ .text.malloc  ✓ (libc, kept)               │
│ ...                                         │
└─────────────────────────────────────────────┘

Library calls printf → ✓ symbol found
```

### With `--gc-sections`

```
┌─────────────────────────────────────────────┐
│ Firmware Binary                             │
├─────────────────────────────────────────────┤
│ .text.main    ✓ (firmware code)             │
│ .text.printf  ✗ DISCARDED (not called)     │
│ .text.sinf    ✗ DISCARDED (not called)     │
│ .text.malloc  ✗ DISCARDED (not called)     │
│ ...                                         │
└─────────────────────────────────────────────┘

Library calls printf → ✗ HARD FAULT (symbol not found)
```

---

## Solutions

### Solution 1: The Linkage Anchor (Keep `--gc-sections`)

Force the linker to keep symbols by creating a dummy reference:

```cpp
// Keep these symbols alive for loaded libraries
extern "C" {
int   printf(const char*, ...);
float sinf(float);
void* malloc(unsigned int);
}

// Dummy array that "uses" the symbols
__attribute__((used, retain))
static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(&printf),
    reinterpret_cast<const void*>(&sinf),
    reinterpret_cast<const void*>(&malloc),
};
```

**Pros:**
- Binary stays small (only keep what libraries need)
- Explicit documentation of library dependencies
- Safe with `--gc-sections`

**Cons:**
- Must manually maintain the list
- Easy to forget a symbol (causes Hard Fault at runtime)
- Verbose boilerplate code

### Solution 2: Remove `--gc-sections` (Simpler)

Don't use `--gc-sections` at all:

```cmake
# Remove these flags:
# -ffunction-sections -fdata-sections  (compile)
# -Wl,--gc-sections                    (link)
```

**Pros:**
- No boilerplate code needed
- All symbols automatically available
- Simpler code, fewer bugs

**Cons:**
- Larger binary (unused code kept)
- May include dead code from static libraries

### Solution 3: Selective Linking (Best of Both)

Use `--gc-sections` but link critical libraries with `--no-gc-sections`:

```cmake
target_link_options(firmware PRIVATE
    -Wl,--gc-sections
    -Wl,--no-gc-sections=-lc    # Keep all libc symbols
    -Wl,--no-gc-sections=-lm    # Keep all libm symbols
)
```

**Pros:**
- Binary stays small
- Critical symbols automatically kept
- No boilerplate

**Cons:**
- Not all linkers support per-library flags
- Still keeps entire libc/libm (more than needed)

---

## Impact on Binary Size

Typical STM32F411 firmware with loaded libraries:

| Configuration | Binary Size | Notes |
|---------------|-------------|-------|
| No `--gc-sections` | ~120 KB | All symbols kept |
| `--gc-sections` + anchor | ~85 KB | Only referenced symbols |
| `--gc-sections` (no anchor) | ~80 KB | Missing library symbols! |

The difference is typically 20-40 KB depending on how much of libc/libm is linked.

---

## Decision Matrix

| Use Case | Recommendation |
|----------|----------------|
| Firmware loads libraries at runtime | **Remove `--gc-sections`** or use anchor |
| Firmware is standalone (no loading) | **Use `--gc-sections`** (saves space) |
| Binary size is critical (<100KB flash) | Use anchor + `--gc-sections` |
| Development/debugging | Remove `--gc-sections` (simpler) |
| Production with size constraints | Use anchor + `--gc-sections` |

---

## Example: XIP Static Direct Loader

### Before (with `--gc-sections` + anchor)

```cpp
// main.cpp - 50 lines of boilerplate
extern "C" {
int   printf(const char*, ...);
int   puts(const char*);
void* _Znwj(unsigned int);
void* _ZnwjRKSt9nothrow_t(unsigned int, const void*);
void* __cxa_allocate_exception(unsigned int);
void  __aeabi_unwind_cpp_pr0(void);
void* __dynamic_cast(const void*, const void*, const void*, long);
float sinf(float);  float cosf(float);  float logf(float);  float sqrtf(float);
}
__attribute__((used, retain)) static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(&printf),
    reinterpret_cast<const void*>(&puts),
    reinterpret_cast<const void*>(&_Znwj),
    reinterpret_cast<const void*>(&_ZnwjRKSt9nothrow_t),
    reinterpret_cast<const void*>(&__cxa_allocate_exception),
    reinterpret_cast<const void*>(&__aeabi_unwind_cpp_pr0),
    reinterpret_cast<const void*>(&__dynamic_cast),
    reinterpret_cast<const void*>(&sinf), reinterpret_cast<const void*>(&cosf),
    reinterpret_cast<const void*>(&logf), reinterpret_cast<const void*>(&sqrtf),
};

// ... actual code starts here
```

### After (without `--gc-sections`)

```cpp
// main.cpp - clean code, no boilerplate
extern "C" {
unsigned int HOST_K = 1000;
unsigned int host_f(unsigned int x) { return x * 2 + 1; }
// ... only actual definitions
}

// ... actual code starts here
```

---

## How to Check What's Being Discarded

### Map File

Generate a linker map file to see what's kept:

```cmake
target_link_options(firmware PRIVATE
    -Wl,-Map=firmware.map
)
```

Then search for discarded sections:

```bash
grep "DISCARDED" firmware.map
```

### Symbol Table

Check if a symbol exists in the binary:

```bash
arm-none-eabi-nm firmware.elf | grep printf
# If empty → symbol was discarded
```

### Read-Only Sections

Check which read-only sections are included:

```bash
arm-none-eabi-readelf -S firmware.elf | grep -E '\.rodata|\.text'
```

---

## Best Practices

1. **Start without `--gc-sections`** during development
2. **Add `--gc-sections`** when optimizing for production
3. **Create a linkage anchor** for symbols loaded libraries need
4. **Test thoroughly** — missing symbols cause Hard Faults, not compile errors
5. **Document dependencies** — list which symbols each library imports
6. **Use map files** to verify symbols are present

---

## Summary

| Aspect | With `--gc-sections` | Without `--gc-sections` |
|--------|---------------------|------------------------|
| Binary size | Smaller | Larger |
| Boilerplate | Need linkage anchor | None |
| Safety | Risk of missing symbols | Always safe |
| Debugging | Harder (missing symbols) | Easier |
| Production use | Recommended (with anchor) | Acceptable |

For firmware that loads libraries at runtime, **removing `--gc-sections` is often the simpler and safer choice**. The binary size increase is usually acceptable (20-40 KB), and it eliminates an entire class of Hard Fault bugs.

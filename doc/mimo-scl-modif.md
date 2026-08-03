# SCL Source Modifications Documentation

## Overview

This document details all modifications made to the SCL (Shared Code Loader) core sources during the development of `test-xip-static/spi-flash`.

---

## 1. shlib-loader.cpp — Auto-register exports from .dynsym

**Commit:** `cfcb7a6` — Fix shlib-loader: auto-register exports from .dynsym when .rel.dyn is missing

**File:** `src/static/shlib-loader.cpp`

**Location:** After the main `patch_shlib()` loop (line ~429)

### Problem

When loading a `.so` file that has no `.rel.dyn` section (common for simple C drivers built with `-shared -nostartfiles`), the `patch_shlib()` function would not register any exports in `global_syms`. This meant:

- Other `.so` files loaded later could not find symbols from the first `.so`
- Example: `cpp_features.so` imports `uart_puts` from `uart.so`, but `uart_puts` was never registered because `uart.so` has no `.rel.dyn`

### Root Cause

`patch_shlib()` iterates over `.rel.dyn` entries to:
1. Patch GOT entries (imports)
2. Register exports in `global_syms`

When `.rel.dyn` is missing, the loop doesn't run, so no exports are registered.

### Solution

After the main patching loop, if no exports were added (`patched == 0`), scan `.dynsym` directly to register GLOBAL/WEAK symbols with non-zero addresses.

### Code Change

```cpp
// If no exports were added from .rel.dyn (e.g. simple .so without relocations),
// scan .dynsym directly to register exported symbols.
if (global_syms && patched == 0) {
    auto si = uint32_t(helper_section_idx::DYN_SYM);
    auto st = uint32_t(helper_section_idx::DYN_STR);
    auto* sym_data = m_shlib[si].address;
    uint32_t sym_size = m_shlib[si].size;
    uint32_t sym_count = sym_size / sizeof(Elf32_Sym);
    auto* str_data = m_shlib[st].address;

    for (uint32_t i = 1; i < sym_count; ++i) {
        auto* sym = reinterpret_cast<Elf32_Sym*>(sym_data) + i;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if ((bind == STB_GLOBAL || bind == STB_WEAK) && sym->st_value) {
            const char* name = reinterpret_cast<const char*>(str_data + sym->st_name);
            uint32_t ra = sym->st_value + uintptr_t(buffer);
            if (bind == STB_WEAK)
                global_syms->add_weak(name, ra);
            else
                global_syms->set_global(name, ra);
        }
    }
}
```

### Technical Details

- **Elf32_Sym structure:** 16 bytes per entry
  - `st_name` (4 bytes): Index into `.dynstr` string table
  - `st_value` (4 bytes): Symbol value (address within the .so)
  - `st_size` (4 bytes): Symbol size
  - `st_info` (1 byte): Binding (high 4 bits) and type (low 4 bits)
  - `st_other` (1 byte): Visibility
  - `st_shndx` (2 bytes): Section index

- **ELF32_ST_BIND(info):** `(info >> 4)` — extracts binding from `st_info`
  - `STB_GLOBAL = 1`: Global symbol
  - `STB_WEAK = 2`: Weak symbol

- **Address calculation:** `ra = sym->st_value + uintptr_t(buffer)`
  - `sym->st_value` is the offset within the .so
  - `buffer` is the RAM base address where the .so was loaded
  - `ra` is the absolute runtime address

- **Guard condition:** `sym->st_value != 0` ensures we only register defined symbols (not undefined imports)

### Impact

- **Before:** Loading a simple C .so (without .rel.dyn) required manual `g_syms.add_global()` calls for each export
- **After:** Exports are automatically registered during `load_buf()` — no manual intervention needed

### Affected Scenarios

- Simple C drivers built with `-shared -nostartfiles` (no .rel.dyn)
- .so files that only export functions without importing from other modules
- Example: `uart.so`, `spi.so`, `led.so` from test-drv fixtures

---

## 2. Related Changes (Non-SCL)

The following changes were made to `test-xip-static/spi-flash` to work with the SCL loader:

### 2.1 UART baud rate fix

**File:** `test-drv/fixtures/src/uart.c`

Changed BRR calculation from PCLK2=84MHz (Nucleo) to PCLK2=100MHz (WeAct BlackPill):

```c
// Old (Nucleo F411RE, HSE=8MHz):
/* PCLK2 = 84 MHz (PLL: HSE/25*336/4). USARTDIV = 84e6/(16*115200) ≈ 45.57 */
USART1_BRR = (45u << 4) | 9u;

// New (WeAct BlackPill, HSE=25MHz):
/* PCLK2 = 100 MHz (PLL: HSE/25*200/2). USARTDIV = 100e6/(16*115200) ≈ 54.25 */
USART1_BRR = (54u << 4) | 4u;
```

### 2.2 Buffer management

**File:** `test-xip/test-xip-static/spi-flash/src/main.cpp`

Separate buffers for each .so to avoid overlap:

- `g_img`: Raw ELF input (32KB)
- `g_spi_img`: uart.so exec buffer (32KB)
- `g_exec`: cpp_features.so exec buffer (32KB)

### 2.3 Symbol table initialization

**File:** `test-xip/test-xip-static/spi-flash/src/main.cpp`

Added `snprintf` to `init_syms()` (needed by `cpp_features_print()`):

```cpp
g_syms.add_global("snprintf", (uint32_t)(uintptr_t)(void*)(int(*)(char*,size_t,const char*,...))snprintf);
```

---

## 3. Test Results

After the shlib-loader fix, the following works without manual symbol registration:

1. **uart.so** — Loaded from internal flash, exports auto-registered
2. **led.so** — Loaded from internal flash, exports auto-registered
3. **cpp_features.so** — Loaded from internal flash, finds `uart_puts` automatically

### Flow

```
main()
  ├── init_syms()                    // Add firmware symbols
  ├── load_spi_from_semihosting()    // Load spi.so into RAM
  ├── spi_init() / spi_format()     // Init SPI flash
  ├── provision_to_spi()            // uart/led/cpp → SPI flash
  ├── copy_spi_to_internal()        // SPI → internal flash
  ├── g_spi_mod = Module{}          // Unload spi.so
  ├── test_uart()                    // Load uart.so, call functions
  ├── test_led()                     // Load led.so, call functions
  └── test_cpp_features()            // Load uart.so + cpp_features.so
        ├── uart_init()              // Init UART
        ├── cpp_features_run()       // Run tests
        ├── cpp_features_print()     // Print results via uart_puts
        └── uart_discard()           // Cleanup
```

---

## 4. Commit History

| Commit | Description |
|--------|-------------|
| `cfcb7a6` | Fix shlib-loader: auto-register exports from .dynsym when .rel.dyn is missing |
| `35be679` | Fix uart garbage: add snprintf to g_syms |
| `6e2c85c` | Revert: don't link .so at build time (causes HardFault) |
| `1093d66` | Link .so at build time for automatic GOT patching |
| `d50bdc7` | Fix uart_puts NOT FOUND: manually register uart exports |
| `8539199` | Fix cpp_features: use g_spi_img for uart.so exec buffer |
| `9fea8ff` | Fix cpp_features: load uart.so first, add uart_puts to g_syms |
| `549651d` | Fix cpp_features_print: init uart before calling |
| `509663c` | Fix UART baud rate: PCLK2=100MHz |
| `67cfe0f` | Add uart_discard(), detailed debug prints |
| `db98fe4` | Add clearer trace prints |
| `b3beed3` | Fix buffer overlap: use g_img for raw, g_exec for exec |
| `b328144` | Use standard test-drv fixtures (not XIP-compatible) |
| `ef93191` | Call fw_spi::fs_init() with spi.so wrappers |
| `17affa1` | Simplify spi.so loading: semihosting directly to RAM |

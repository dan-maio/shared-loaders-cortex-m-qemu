# How the SCL Tests Load Modules (.so / .o)

This workspace loads shared modules (`.so`, and `.rel.o` for the ET_REL loader)
into the Cortex-M4F target over several storage back-ends. Every load path
feeds one **core loader** that parses/relocates an ELF image from an abstract
random-access **`loader::byte_source`**, so "where the bytes come from" and
"how they are linked" are cleanly separated. This document lists every loading
method, the API used, and where it lives.

---

## 1. The common abstraction: `loader::byte_source`

All loaders read an object through this interface — a random-access reader with
a size and a bounds-checked `read_at`. The core never assumes the whole object
is resident.

```cpp
class byte_source {
    virtual std::size_t size() const noexcept = 0;
    virtual result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};
```

- **Interface:** `common-elf-reader/include/loader/byte-source.hpp`

The **core** loader that consumes any `byte_source`:

```cpp
namespace fw {
loader::result<handle*> dlopen_source(loader::byte_source& src, const char* rawname) noexcept;
}
```

- **Definition:** `src/dynamic-nolib/dl.cpp` (`dlopen_source`, line ~190) —
  parses ELF, copies `PT_LOAD` segments to RAM, applies relocations, resolves
  symbols against main, runs constructors.
- **Declaration:** `include/dynamic-nolib/dl.hpp`

`byte_source` implementations in this workspace:

| Implementation | Storage | Source file |
|---|---|---|
| `fw::semihost_file` | Host filesystem over the debug probe | `test/platforms/common/src/semihost-file.cpp`, `test/platforms/board/include/semihost-file.hpp` |
| `fw::SpiByteSource` | External W25Q64 SPI flash | `test-dynamic/test-flatfs-spi/include/spi-byte-source.hpp`, `test-dynamic/test-flatfs-bitbang-spi/include/spi-byte-source.hpp` |
| `fw::Flashbyte_source` | On-chip (internal) flash, memory-mapped | `test-dynamic/test-dynamic-nolib/test3/include/flash-byte-source.hpp` |
| `loader::memory_byte_source` | RAM buffer (host / XIP provisioning) | `common-elf-reader/include/loader/sources/memory-byte-source.hpp` |
| `loader::fd_byte_source` | POSIX file descriptor (host tests) | `common-elf-reader/include/loader/sources/fd-byte-source.hpp` |

---

## 2. Loading from SEMIHOSTING (host filesystem)

The debug probe (CMSIS-DAP / ST-LINK) serves the *host* filesystem to the target
via ARM semihosting calls (`SYS_OPEN`, `SYS_FLEN`, `SYS_SEEK`, `SYS_READ`,
`SYS_CLOSE`). This is the default for the freestanding `dlopen` tests.

**Low-level semihosting API** — `test/platforms/board/src/semihost.c`:

```c
int  semihost_open(const char* path, int mode);   // mode 1 == "rb"
long semihost_flen(int handle);
int  semihost_seek(int handle, long pos);
int  semihost_read(int handle, void* buf, int len);
int  semihost_close(int handle);
int  semihost_get_cmdline(char* buf, int len);
```

**`fw::semihost_file`** — wraps a semihosting handle as a `byte_source`
(`test/platforms/common/src/semihost-file.cpp`):

```cpp
static loader::result<semihost_file> open(const char* path) noexcept;
```

**`fw::dlopen(path)`** — the semihosting `dlopen` (strong definition)
`test/platforms/common/src/dl-semihost.cpp`:

```cpp
namespace fw {
loader::result<handle*> dlopen(const char* path) noexcept {
    auto srcr = semihost_file::open(path);        // open host file
    semihost_file src = std::move(*srcr);
    return dlopen_source(src, path);              // load+relocate from it
}
}
```

**`dl::dlopen()`** — the C-API facade used by the demo mains
(`src/dynamic-nolib/dl.cpp`, ~line 780). It auto-routes through
`fw::dlopen` (semihosting) or `fw::dlopen_fs` (flash) based on the
`g_use_flatfs` flag:

```cpp
extern "C" loader::result<fw::handle*> scl_impl_dlopen(const char* filename, int flag);
void* dl::dlopen(const char* filename, int flag);
void* dl::dlsym(void* handle, const char* symbol);
int   dl::dlclose(void* handle);
```

**Where it is used:**
- `test-dynamic/test-dynamic-nolib/test1/src/main.cpp` — `dl::dlopen(DL_LIB_DIR "/mylib1.so", RTLD_NOW)`
- `test-dynamic/test-dynamic-nolib/test2/src/main.cpp` — `fw::dlopen(path)` (battery `open_sel`)
- `test-dynamic/test-dynamic-nolib/test4/src/main.cpp` — `fw::dlopen(DL_LIB_DIR "/cpp3.so")`
- `test-overlay/test1/src/main-overlay.cpp` — `fw::dlopen(path)`
- `test-dynamic/test-dynamic-newlib/test1|2` — hosted `dynamic_linker::load_from_source` over `semihost_file`
- `test-static/test-static*` — static patcher `scl::Module::load_file` via `elf_reader::open_elf`
- `test-relocatable/src/*` — `scl-fw-reloc` / `classic` read `.rel.o` over semihosting

`DL_LIB_DIR` (an absolute path to `test/fixtures`, injected by the build) makes
the semihosting reads independent of the OpenOCD working directory.

---

## 3. Loading from the ON-CHIP FLASH flat-filesystem

`scl-fw-flatfs` (`test-dynamic/test-dynamic-nolib/test3`) first **programs a
flat-filesystem archive into the internal flash** (sectors at `0x0804xxxx`),
then loads libraries by name straight from that memory-mapped flash.

**Provisioning** — `flash-prog.c` programs the on-chip flash; `fs_format` /
`fs_add` / `fs_find` / `fs_blob` / `fs_count` manage the archive
(`test-dynamic/test-dynamic-nolib/test3/src/fs-archive.cpp`):

```cpp
loader::result<void> fs_format() noexcept;
loader::result<void> fs_add(const char* name, const void* data, std::uint32_t len) noexcept;
const FsEntry* fs_find(const char* name) noexcept;
const std::byte* fs_blob(const FsEntry& e) noexcept;
std::uint32_t fs_count() noexcept;
```

Because internal flash is **memory-mapped** (`0x08xxxxxx`), "reading a file" is
a plain `memcpy` — `Flashbyte_source`
(`test-dynamic/test-dynamic-nolib/test3/include/flash-byte-source.hpp`):

```cpp
class Flashbyte_source final : public loader::byte_source {
    Flashbyte_source(const std::byte* base, std::size_t size) noexcept;
    std::size_t size() const noexcept override;
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
};
```

**`fw::dlopen_fs(name)`** — the on-chip flash loader
(`test-dynamic/test-dynamic-nolib/test3/src/dl-fs.cpp`):

```cpp
namespace fw {
loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);              // look up by basename
    if (!e) return loader::unexpected(loader::error::FILE_NOT_FOUND);
    Flashbyte_source src(fs_blob(*e), e->size);    // byte_source over flash
    return dlopen_source(src, name);               // same core loader
}
}
```

**Where it is used:**
- `test-dynamic/test-dynamic-nolib/test3/src/main-flatfs.cpp` — provisions
  fixtures via semihosting, then `battery_run()` loads them with `dlopen_fs`.
- Routed automatically by `scl_impl_dlopen` when `g_use_flatfs == true`.

---

## 4. Loading from the EXTERNAL SPI FLASH (W25Q64)

`scl-fw-flatfs-spi` / `scl-fw-flatfs-bitbang-spi`
(`test-dynamic/test-flatfs-spi`, `test-dynamic/test-flatfs-bitbang-spi`) format
a flat-filesystem archive on an external W25Q64 SPI flash and load the modules
from it. Two interchangeable drivers bit-bang the GPIO SPI (the only one proven
reliable on the WeAct board):

**SPI flash driver** — `test-dynamic/test-flatfs-spi/src/w25q64.cpp`
(bit-banged GPIO SPI on PA4 CS / PA5 SCK / PA6 MISO / PA7 MOSI; no HAL):

```c
void spi_flash_init(void);
int  spi_flash_read_id(uint8_t* mid, uint8_t* tid);
void spi_flash_sector_erase(uint32_t addr);
int  spi_flash_page_program(uint32_t addr, const uint8_t* data, uint16_t len);
int  spi_flash_write(uint32_t addr, const uint8_t* data, uint32_t len);
void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len);
void spi_flash_get_sr(uint32_t* sr, uint32_t* cr1);
void spi_flash_reinit(void);
```

**SPI flat-filesystem** — `test-dynamic/test-flatfs-spi/src/fs-archive-spi.cpp`
(same `fs_format`/`fs_add`/`fs_find`/`fs_count` API as the on-chip archive).

**`fw::SpiByteSource`** — `test-dynamic/test-flatfs-spi/include/spi-byte-source.hpp`:

```cpp
class SpiByteSource final : public loader::byte_source {
    SpiByteSource(std::uint32_t spi_addr, std::size_t size) noexcept;
    std::size_t size() const noexcept override;
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
};
```

**`fw::dlopen_fs(name)`** — the SPI flash loader
(`test-dynamic/test-flatfs-spi/src/dl-fs-spi.cpp`):

```cpp
namespace fw {
loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);
    if (!e) return loader::unexpected(loader::error::FILE_NOT_FOUND);
    spi_flash_reinit();
    SpiByteSource src(e->off, e->size);            // byte_source over SPI
    return dlopen_source(src, name);
}
}
```

**Where it is used:**
- `test-dynamic/test-flatfs-spi` → `scl-fw-flatfs-spi`
- `test-dynamic/test-flatfs-bitbang-spi` → `scl-fw-flatfs-bitbang-spi`
- `test-relocatable` → `scl-fw-classic-spi-reloc` loads ET_REL `.rel.o` from SPI
- `test-xip/test-xip-static/spi-flash` uses SPI flash as **transit storage**
  (provisions to SPI, then copies into internal flash for XIP — see §5).

---

## 5. Loading from INTERNAL FLASH with XIP (execute-in-place)

The XIP tests (`test-xip/`) load modules whose text stays in **flash** and only
their RW segment (GOT + `.data` + `.bss`) is copied to RAM — a two-base model.
Modules are linked with a split `R-X | RW` layout
(`-fpic -msingle-pic-base -mpic-register=r10`, `xip-split.ld`).

**Segment classification** — `include/xip/segment-map.hpp`:

```cpp
namespace xip {
struct SegmentMap {
    std::uintptr_t text_vaddr, text_off, text_size;   // R-X stays in flash
    std::uintptr_t data_vaddr, data_off, data_filesz, data_memsz; // RW → RAM
    bool split;   // false ⇒ fused RWX, cannot XIP → copy whole
    bool in_data(std::uintptr_t) const noexcept;
    bool in_text(std::uintptr_t) const noexcept;
};
}
```

**The static XIP adapter** — `src/xip/static/adapter.cpp`:

```cpp
namespace xip::stat {
// loads a module (text in flash, data in RAM), registers it for cross-lib calls
loader::result<std::uint32_t> thunk(void* ctx, std::string_view n) noexcept;
int bind_host_imports(std::span<std::byte> image) noexcept;  // pre-bind host fn imports
}
```

**`scl::Module`** — the XIP module handle
(`src/static/module.cpp`, `include/xip/module.hpp`):

```cpp
namespace scl {
class Module {
    bool load_file(const char* path, uint8_t* buf, uint32_t bufsz, elf_loader::symbol_table* syms);
    bool load_buf(const uint8_t* raw, uint32_t rawsz, uint8_t* buf, uint32_t bufsz, elf_loader::symbol_table* syms);
    void* sym(const char* name) const;   // resolve a symbol to its flash/RAM address
};
}
```

**Call helpers** — `test-xip/.../include/test-helpers.hpp`:

```cpp
inline std::uint32_t call_sym0(const scl::Module& m, const char* name);
inline std::uint32_t call_sym1(const scl::Module& m, const char* name, std::uint32_t a);
inline std::uint32_t call_sym2(const scl::Module& m, const char* name, std::uint32_t a, std::uint32_t b);
inline std::uint32_t call_sym3(const scl::Module& m, const char* name, std::uint32_t a, std::uint32_t b, std::uint32_t c);
```

**XIP provisioning pipeline** (`test-xip/test-xip-static/spi-flash/src/main.cpp`):

1. `provision_spi_to_internal()` — read `libspi.so` from **semihosting**,
   `fw_int::fs_add("spi.so", ...)` into **internal flash**.
2. `provision_to_spi("libuart.so")` etc. — read from **semihosting**,
   `fw_spi::fs_add(...)` into the **SPI flash** flatfs.
3. `copy_spi_to_internal("uart.so")` — `fw_spi::fs_find` + read, then
   `fw_int::fs_add` into **internal flash**.
4. `fw_int::fs_find`/`fs_blob` return the memory-mapped blob; `Module::load_buf`
   + `call_sym*` execute it **in place** (XIP).

**Internal-flash flatfs API** — `test-xip/test-xip-static/spi-flash/src/fs-archive-internal.cpp`:

```cpp
loader::result<void> fs_format();
const FsEntry* fs_find(const char* name);
const std::byte* fs_blob(const FsEntry& e);
std::uint32_t fs_used();
loader::result<void> fs_add(const char* name, const void* data, std::uint32_t len);
```

**Where it is used:** every `test-xip/test-xip-*` app
(`scl-fw-xip-nolib1`, `scl-fw-xip-newlib1`, `scl-fw-xip-static*`,
`scl-fw-xip-interrupt*`, `scl-fw-xip-static-spi-flash`).

---

## 6. Other loaders in this workspace

### 6.1 Hosted static loader (`scl::dynamic-newlib`)
Uses `std::vector` and a hosted C++ runtime. Loads over any `byte_source` too:

```cpp
namespace loader {
struct library { ... };
class dynamic_linker {
    result<library*> load_from_source(loader::byte_source& src) noexcept;
};
}
```
- **Source:** `src/dynamic-newlib/dynamic-linker.cpp`
- **Used by:** `test-dynamic/test-dynamic-newlib/test1|2` (over `semihost_file`).

### 6.2 ET_REL module loader (`scl::reloc`)
Loads non-PIC `-mlong-calls -fno-pic -r` ET_REL objects (no GOT/PLT) from raw
spans:

```cpp
namespace reloc {
struct module_set { ... };
loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte> arena,
                                    loader::symbol_source& rs) noexcept;
std::uint32_t sym(const module_set& s, std::string_view name) noexcept;
}
```
- **Source:** `src/reloc/reloc-loader.cpp`
- **Used by:** `test-relocatable` — `scl-fw-reloc`/`classic` (semihosting),
  `scl-fw-classic-spi-reloc` (SPI flash), `scl-fw-reloc-interrupt`.

### 6.3 Standalone static patcher (`scl::static`)
The original `elf_reader`/`shlib_loader`/`Module` API — loads a whole copy-into-RAM
image from a path or a RAM buffer:

```cpp
namespace scl {
class Module {
    bool load_file(const char* path, uint8_t* buf, uint32_t bufsz, elf_loader::symbol_table* syms);
    bool load_buf(const uint8_t* raw, uint32_t rawsz, uint8_t* buf, uint32_t bufsz, ...);
    void* sym(const char* name) const;
};
}
```
- **Source:** `src/static/module.cpp`, `src/static/shlib-loader.cpp`, `src/static/elf-reader.cpp`
- **Used by:** `test-static/test-static*` (`f411ce-sh`), `test-static/test-drv`.

---

## 7. Which method each test uses

| Test app | Storage / loading method | Key entry point |
|---|---|---|
| `scl-fw-dyn1` / `dyn2` / `gotplt-demo` | Semihosting (host filesystem) | `dl::dlopen` → `fw::dlopen` → `dlopen_source` |
| `scl-fw-flatfs` | On-chip internal flash flatfs | `fw::dlopen_fs` + `Flashbyte_source` |
| `scl-fw-flatfs-spi` / `flatfs-bitbang-spi` | External W25Q64 SPI flash flatfs | `fw::dlopen_fs` + `SpiByteSource` |
| `scl-fw-dynamic-newlib1/2` (sf/hf) | Semihosting (hosted static loader) | `dynamic_linker::load_from_source` over `semihost_file` |
| `scl-fw-ovl1` | Semihosting (`dlopen` for dynamic half) | `fw::dlopen` |
| `scl-fw-reloc` / `classic` | Semihosting (ET_REL) | `reloc::load_set` |
| `scl-fw-classic-spi-reloc` | SPI flash (ET_REL) | `fw_spi::fs_find` → `reloc::load_set` |
| `scl-fw-xip-*` | Internal flash + XIP | `xip::stat` + `Module::load_buf` + `call_sym*` |
| `scl-fw-xip-static-spi-flash` | Semihosting → SPI → internal flash → XIP | `fs_add`/`fs_blob` + `Module::load_buf` |
| `f411ce-sh` / `scl-fw-static-*` / `scl-fw-drv` | Semihosting (static patcher) | `Module::load_file`/`load_buf` |

---

## 8. The one core: everything funnels through `dlopen_source`

All dynamic (`ET_DYN`) paths converge on the same core, `fw::dlopen_source`
(`src/dynamic-nolib/dl.cpp`):

```
semihost_file ─┐
SpiByteSource ─┼─► loader::byte_source ─► elf_image::parse ─► copy PT_LOAD
Flashbyte_source┘        │                                   ─► relocate (GOT/PLT)
                          │                                   ─► resolve vs main .dynsym
                          └─► dlopen_source()                 ─► run .init_array
```

This is why adding a new storage medium (a new `byte_source`) requires **zero
changes to the loader** — the same reason the semihosting, on-chip-flash, SPI-flash
and XIP tests all share one relocation engine.

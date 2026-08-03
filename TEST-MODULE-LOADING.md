# How the Tests Read Module Data from Different Sources

This document explains only the **reading mechanism**: how the firmware gets the
bytes of a `.so` / `.o` file out of whatever storage holds them. It covers the
**reader classes**, their **constructors**, where each one is **instantiated**,
and how the `read_at()` method actually moves bytes from the **source** into a
**destination buffer**. Symbol resolution, GOT/PLT patching and relocation are
deliberately out of scope.

---

## 0. The reading contract: `loader::byte_source`

Every reader is a subclass of one abstract class. The class has two pure
virtual methods:

```cpp
class loader::byte_source {
    virtual std::size_t size() const noexcept = 0;
    virtual result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};
```

- `size()` — how many bytes the object (file / blob) has in total.
- `read_at(offset, out)` — **fill `out` with `out.size()` bytes taken from the
  source starting at `offset`**. If `offset + out.size()` exceeds `size()`, it
  fails with `error::SHORT_READ` and writes nothing.

So a reader is just "a thing I can ask for bytes 0..N of". The **caller** of
`read_at` is always the ELF loader (`fw::dlopen_source`, `elf_image::parse`,
`Module::build`), which asks for the ELF header, the program headers, then each
loadable segment — and receives those bytes into its **own destination RAM
buffer**. The caller never knows or cares which storage the bytes came from.

| Reader class | Storage the bytes come from | Where the class is defined |
|---|---|---|
| `fw::semihost_file` | Host filesystem, served by the debug probe | `test/platforms/board/include/semihost-file.hpp` / `test/platforms/common/src/semihost-file.cpp` |
| `fw::Flashbyte_source` | On-chip internal flash (memory-mapped) | `test-dynamic/test-dynamic-nolib/test3/include/flash-byte-source.hpp` |
| `fw::SpiByteSource` | External W25Q64 SPI flash | `test-dynamic/test-flatfs-spi/include/spi-byte-source.hpp` |
| `loader::memory_byte_source` | A RAM buffer | `common-elf-reader/include/loader/sources/memory-byte-source.hpp` |
| `loader::fd_byte_source` | A POSIX file descriptor (host only) | `common-elf-reader/include/loader/sources/fd-byte-source.hpp` |

### 0.1 The responsibility split: the general library vs the individual tests

The single most important idea in this design is **who is responsible for
what**. It is a clean separation between a **general, reusable library** and
**test-specific, per-board/per-storage code**:

**`common-elf-reader/` — the general library (storage-agnostic).**

- It owns the **abstract contract** `loader::byte_source`
  (`common-elf-reader/include/loader/byte-source.hpp`).
- It owns the **consumer** of that contract — the ELF loader
  (`fw::dlopen_source`, `elf_image::parse`, `Module::build`), which only ever
  calls `size()` / `read_at()` and never knows or cares where the bytes come
  from.
- It also ships the **generic host readers** that are useful everywhere
  (`loader::memory_byte_source`, `loader::fd_byte_source`).
- It contains **no board code, no probe code, no flash driver, no test
  logic** — nothing about WEACT/NUCLEO, semihosting, or SPI.

**The tests / platforms — individual readers for their own storage.**

- Each test that needs a new storage medium **derives its own class** from the
  library's abstract `loader::byte_source` and implements `size()` +
  `read_at()` for that medium only.
- The freestanding platform contributes `fw::semihost_file` (semihosting reader)
  in `test/platforms/…`.
- The flatfs test contributes `fw::Flashbyte_source` (on-chip flash) in
  `test-dynamic/test-dynamic-nolib/test3/…`.
- The SPI-flash test contributes `fw::SpiByteSource` (W25Q64) in
  `test-dynamic/test-flatfs-spi/…`.

So the **library defines the interface and does the work** (parse + link from
whatever `read_at` returns); the **tests provide the concrete byte delivery**
for their particular source. Adding a new storage medium requires **zero changes
to the library** — only a new subclass in the test that needs it, exactly the
per-test derivations shown in §1–§5 below.

---

## 1. `fw::semihost_file` — read from the HOST filesystem via the debug probe

The debug probe (CMSIS-DAP on WEACT, ST-LINK on NUCLEO) runs the firmware and
can, on request, open a file **on the host PC** and hand its bytes to the
target. That is what this reader does.

**Class and constructor.** The constructor is private; you obtain an instance
with the static factory `open(path)`:

```cpp
// test/platforms/board/include/semihost-file.hpp
class semihost_file final : public loader::byte_source {
public:
    static loader::result<semihost_file> open(const char* path) noexcept;
    std::size_t size() const noexcept override;
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
private:
    semihost_file(int h, std::size_t sz) noexcept;   // private ctor
    int         handle_;   // the probe's open file handle
    std::size_t size_;     // file size, cached at open()
};
```

**What the constructor does** (`test/platforms/common/src/semihost-file.cpp`):

```cpp
int h = semihost_open(path, 1);        // 1 == "rb": probe opens the host file
long n = semihost_flen(h);             // query its size in bytes
return semihost_file(h, n);            // construct with handle + size
```

**What `read_at` does** (same file) — for each call the probe seeks the host
file, then reads `out.size()` bytes from disk straight into `out`:

```cpp
result<void> semihost_file::read_at(std::size_t offset, std::span<std::byte> out) {
    if (offset > size_ || out.size() > size_ - offset) return SHORT_READ;
    semihost_seek(handle_, offset);                 // SYS_SEEK on the host file
    semihost_read(handle_, out.data(), out.size()); // SYS_READ host -> out
    return {};
}
```

**Instantiation (the only place in the firmware):**
`test/platforms/common/src/dl-semihost.cpp`:

```cpp
namespace fw {
loader::result<handle*> dlopen(const char* path) noexcept {
    auto srcr = semihost_file::open(path);        // 1. build the reader
    fw::semihost_file src = std::move(*srcr);     // 2. keep the reader alive
    return dlopen_source(src, path);              // 3. hand it to the loader
}
}
```

**Data flow, concrete (`scl-fw-dyn1` loading `mylib1.so`):**

- **Source of the bytes:** `test/fixtures/mylib1.so` on the **host PC**. The
  target holds only a handle; no file bytes are on the chip.
- **`dlopen_source` calls `read_at`:** offset `0` (ELF header, 52 B), then the
  program headers, then each `PT_LOAD` segment. Each call makes the probe seek
  the host file and read the requested window.
- **Destination of the bytes:** the loader's RAM image slot inside the 64 KB
  `_simage_arena` (RAM `0x20000000`…`0x20020000`). `read_at` fills that slot
  segment-by-segment; when the last `read_at` returns, the whole image is in
  RAM and the reader is no longer needed.

---

## 2. `fw::Flashbyte_source` — read from ON-CHIP internal flash

The flatfs test writes each `.so` into the **on-chip flash** (bus region
`0x0804xxxx`). Internal flash is **memory-mapped**: the CPU can read it like
RAM, so "reading a file" is a plain `memcpy` — no peripheral, no probe.

**Class and constructor:**

```cpp
// test-dynamic/test-dynamic-nolib/test3/include/flash-byte-source.hpp
class Flashbyte_source final : public loader::byte_source {
public:
    Flashbyte_source(const std::byte* base, std::size_t size) noexcept
        : base_(base), size_(size) {}
    std::size_t size() const noexcept override { return size_; }
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > size_ || out.size() > size_ - offset) return SHORT_READ;
        std::memcpy(out.data(), base_ + offset, out.size());   // flash -> out
        return {};
    }
private:
    const std::byte* base_;   // pointer INTO flash (0x08xxxxxx)
    std::size_t      size_;
};
```

**What the constructor does:** nothing but store `base` (the flash address of
the blob) and `size`. The bytes are already on the chip; there is nothing to
open.

**What `read_at` does:** one `memcpy` from `base_ + offset` (a flash address)
to `out.data()` (a RAM address).

**Instantiation:** `test-dynamic/test-dynamic-nolib/test3/src/dl-fs.cpp`:

```cpp
namespace fw {
loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);                 // find blob by name (TOC in RAM)
    Flashbyte_source src(fs_blob(*e), e->size);       // base = pointer into flash
    return dlopen_source(src, name);
}
}
```

`fs_blob(*e)` returns a `const std::byte*` whose value is a **flash address**
(the blob's offset added to the flash flatfs base, e.g. `0x08041000`).

**Data flow, concrete (`scl-fw-flatfs` loading `c2.so`):**

- **Source of the bytes:** the `c2.so` blob **inside the on-chip flash** at
  `0x08041000` (memory-mapped). The flash was programmed earlier by
  `flash-prog.c`.
- **`read_at`:** every call is `memcpy` from `0x08041000 + offset` into the
  loader's RAM slot.
- **Destination of the bytes:** a RAM slot in `_simage_arena` — the image is
  **copied out of flash** into RAM (copy-whole; no execute-in-place here).

---

## 3. `fw::SpiByteSource` — read from EXTERNAL SPI flash (W25Q64)

The SPI-flash tests store each `.so` on an external W25Q64 chip. That chip is
**not memory-mapped** — the firmware must clock every byte out over the SPI bus
(bit-banged GPIO SPI on PA4 CS / PA5 SCK / PA6 MISO / PA7 MOSI).

**Class and constructor:**

```cpp
// test-dynamic/test-flatfs-spi/include/spi-byte-source.hpp
class SpiByteSource final : public loader::byte_source {
public:
    SpiByteSource(std::uint32_t spi_addr, std::size_t size) noexcept
        : spi_addr_(spi_addr), size_(size) {}
    std::size_t size() const noexcept override { return size_; }
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > size_ || out.size() > size_ - offset) return SHORT_READ;
        spi_flash_read(spi_addr_ + offset, (uint8_t*)out.data(), (uint16_t)out.size());
        return {};
    }
private:
    std::uint32_t spi_addr_;   // byte address inside the W25Q64 (0 = first byte)
    std::size_t   size_;
};
```

**What the constructor does:** stores `spi_addr` (where the blob starts in the
chip's address space, e.g. `0x0000A2C0`) and `size`. Nothing is read yet.

**What `read_at` does:** calls the SPI driver
(`test-dynamic/test-flatfs-spi/src/w25q64.cpp`) to clock bytes out of the chip
into `out`:

```c
void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len) {
    cs_low();                                    // assert chip-select
    xfer(0x03);                                  // W25Q64 READ opcode
    xfer(addr>>16); xfer(addr>>8); xfer(addr);   // 24-bit address
    for (i = 0; i < len; i++) data[i] = xfer(0xFF); // clock len bytes off MISO
    cs_high();                                   // deassert chip-select
}
```

(`xfer()` bit-bangs one byte on GPIOA: MOSI out, SCK strobe 8×, MISO in.)

**Instantiation:** `test-dynamic/test-flatfs-spi/src/dl-fs-spi.cpp`:

```cpp
namespace fw {
loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);               // TOC entry (offset,size in RAM)
    spi_flash_reinit();                             // re-init the GPIO SPI
    SpiByteSource src(e->off, e->size);             // addr = blob offset in the chip
    return dlopen_source(src, name);
}
}
```

**Data flow, concrete (`scl-fw-flatfs-spi` loading `c2.so`):**

- **Source of the bytes:** the `c2.so` blob at address `e->off` (e.g.
  `0x0000A2C0`) **inside the external W25Q64**. Before the load, `fs_format`
  + `fs_add` wrote it there over SPI.
- **`read_at`:** each call asserts CS, sends READ + the 24-bit address, then
  clocks `out.size()` bytes off MISO into `out` (one `xfer(0xFF)` per byte).
- **Destination of the bytes:** a RAM slot in `_simage_arena` — the image is
  copied over SPI into RAM.

---

## 4. `loader::memory_byte_source` — read from a RAM buffer

Used when the object is **already resident in RAM** (host tests, or firmware
that stages an image in a buffer before linking). It is non-owning: it only
references the caller's buffer.

**Class and constructor:**

```cpp
// common-elf-reader/include/loader/sources/memory-byte-source.hpp
class memory_byte_source final : public loader::byte_source {
public:
    memory_byte_source() noexcept = default;
    explicit memory_byte_source(std::span<const std::byte> data) noexcept : data_(data) {}
    std::size_t size() const noexcept override { return data_.size(); }
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > data_.size() || out.size() > data_.size() - offset) return SHORT_READ;
        std::memcpy(out.data(), data_.data() + offset, out.size());   // RAM -> out
        return {};
    }
private:
    std::span<const std::byte> data_;
};
```

**What the constructor does:** stores the caller's span (base pointer + length).
No allocation, no I/O.

**What `read_at` does:** one `memcpy` from the span to `out`.

**Instantiation examples:**
- Host-side ELF tools: `loader::memory_byte_source src(span_of_file_bytes);`
- XIP provisioning (`src/xip/static/adapter.cpp`,
  `int xip::stat::bind_host_imports(std::span<std::byte> image)`) wraps the
  just-read image: `loader::memory_byte_source src(image);`

**Data flow, concrete:** the image bytes are already in a RAM buffer
(`image`); every `read_at` memcpy's the requested window from that buffer into
the destination. Source and destination are both RAM — this reader just
re-windows an existing buffer.

---

## 5. `loader::fd_byte_source` — read from a POSIX file descriptor (host)

For host tools that read an ELF with normal `open()`/`read()` on Linux.

**Class and constructor:** wraps an open `int fd`.

```cpp
// common-elf-reader/include/loader/sources/fd-byte-source.hpp
class fd_byte_source final : public loader::byte_source {
public:
    explicit fd_byte_source(int fd) noexcept;      // takes ownership of fd
    std::size_t size() const noexcept override;    // via fstat at open
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
};
```

**What `read_at` does:** `pread(fd, out.data(), out.size(), offset)` — the
kernel copies the requested window from the file into `out`.

**Instantiation:** host tools (`common-elf-reader/test` etc.):

```cpp
int fd = ::open("module.so", O_RDONLY);
loader::fd_byte_source src(fd);
```

---

## 6. Summary: the same two questions for every source

For every loading method in this workspace, the answer reduces to "who builds
the reader, and what is the reader's constructor handed?":

| Test app | Reader instantiated | Constructor argument = WHERE the bytes are | How `read_at` gets them | Bytes land in |
|---|---|---|---|---|
| `scl-fw-dyn1` | `semihost_file::open(path)` | host file path | probe `SYS_OPEN`/`SYS_READ` | RAM arena slot |
| `scl-fw-flatfs` | `Flashbyte_source(fs_blob(e), size)` | pointer into **on-chip flash** | `memcpy` (flash is memory-mapped) | RAM arena slot |
| `scl-fw-flatfs-spi` | `SpiByteSource(e->off, size)` | byte address in the **external W25Q64** | bit-banged SPI `xfer` loop | RAM arena slot |
| `scl-fw-xip-*` | `scl::Module::load_buf(flash_blob, size, buf, …)` | pointer into **on-chip flash** | `memcpy`; only RW copied, text stays in flash | RAM data base (text stays at flash addr) |
| `scl-fw-dynamic-newlib*` | `semihost_file` (hosted loader) | host file path | probe `SYS_READ` | newlib heap |
| `scl-fw-reloc` | raw `.rel.o` spans (`g_img[]`) | RAM staging buffer | `memcpy` | caller's `g_arena` |
| `scl-fw-classic-spi-reloc` | `SpiByteSource`/raw spans | external SPI flash | bit-banged SPI | caller's arena |
| `f411ce-sh` | `Module::load_file(path, buf, …)` | host file path → caller's `buf` | probe `SYS_READ` | caller's `buf` (RAM) |

The **one rule** that holds everywhere: the loader asks `reader.read_at(offset,
out)`; the reader is responsible for producing those bytes from its source; the
bytes are written into `out`, which the loader owns. Change the storage and you
change **only** which reader (and which constructor argument) is used — the
loader's reading loop never changes.

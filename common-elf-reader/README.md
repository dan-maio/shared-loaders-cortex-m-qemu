# common-elf-reader

A small, standalone **ARM ELF32 little-endian reader/parser** shared by both
Cortex-M shared-library loaders (`cortexm-shared-loader` and
`shared-static-rework`) so they parse ELF objects through **one API**.

This library is *only* the reader/parser. Relocation application, symbol
resolution and GOT patching stay in each consuming loader — this component has
no opinion on how an image is fixed up once parsed.

## What it provides

| Header | Contents |
|--------|----------|
| `loader/error.hpp` | `error` enum + `result<T>` (a C++20 `std::expected` stand-in) |
| `loader/byte-source.hpp` | `byte_source` — abstract random-access reader (the I/O seam) |
| `loader/elf-types.hpp` | ARM ELF32-LE on-disk structs, constants, `enum class rel_type/bind`, decoders |
| `loader/elf-image.hpp` | `elf_image` — validated, typed access to an ELF object |
| `loader/sources/fd-byte-source.hpp` | *(optional)* `byte_source` over a POSIX / semihosting fd |
| `loader/sources/memory-byte-source.hpp` | *(optional)* `byte_source` over an in-memory span |

`src/elf-image.cpp` is the only translation unit; everything else is header-only.

## Design

- **No global state, no exceptions, no RTTI.** Every fallible call returns
  `result<T>` and is `noexcept` / `[[nodiscard]]`. Suitable for freestanding
  bare-metal builds.
- **The core never assumes the whole file is resident.** All I/O goes through
  `byte_source::read_at`, so the same parser works over semihosting on the
  target and over an in-memory buffer in host tests.
- **Little-endian ARM ELF32 as a data format.** `elf-types.hpp` mirrors the ABI
  layout exactly and `static_assert`s a little-endian host/target.
- **Bounds-safe.** Section-name reads are bounded to the string table; malformed
  offsets yield empty views rather than out-of-range reads.

## API sketch

```cpp
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
using namespace loader;

auto src = fd_byte_source::open_file("libsh1.so");     // result<fd_byte_source>
if (!src) { /* src.err() == error::FILE_NOT_FOUND */ }

auto img = elf_image::parse(*src);                     // validates magic/class/machine/type
if (!img) { /* img.err(): NOT_ELF | NOT_ARM32 | NOT_SHARED_OBJECT | SHORT_READ */ }

const elf::Ehdr& eh = img->header();
uint32_t bytes      = *img->image_size();              // highest PT_LOAD end (file+bss)
uint32_t base       = *img->image_base();              // lowest PT_LOAD vaddr

elf::Phdr ph;  img->program_header(0, ph);
elf::Shdr sh;  img->section_header(0, sh);
auto dynsym = img->find_section(".dynsym");            // result<elf::Shdr>, BAD_SECTION if absent
```

To read raw section bytes, use the `elf::Shdr` fields with `img->source().read_at()`.

## Consuming it

**Sibling repo (path):**
```cmake
add_subdirectory(../common-elf-reader common-elf-reader-build)
target_link_libraries(my_loader PRIVATE common-elf-reader::elf-reader)
```

**Git submodule:**
```
git submodule add <url> third_party/common-elf-reader
add_subdirectory(third_party/common-elf-reader)
```

Set `-DSCL_HOSTED=ON` to enable `std::vector`-based host helpers
(`elf_image::collect_relocations`); leave it off for firmware.

## Build & smoke check

```console
cmake -B build && cmake --build build          # host static lib
cmake -B build -DSCL_HOSTED=ON && cmake --build build
```

Verified to compile with GCC C++20 in three modes: host freestanding, host
hosted, and `arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb`.

# `common-elf-reader` — API & Usage Guide

The `common-elf-reader` is a lightweight, freestanding C++20 **ARM ELF32 little-endian reader/parser** library. It decouples file parsing and structural validation from memory relocation and symbol resolution, allowing multiple loaders to reuse the same robust, bounds-checked parsing core.

---

## 1. Core Design Principles

* **No Exceptions & No RTTI**: Suitable for bare-metal targets and freestanding environments.
* **Allocation-Free Core**: The core parser does not allocate memory on the heap.
* **Abstract I/O Seam (`byte_source`)**: The parser does not assume files are fully resident in memory; it streams only the necessary structural headers.
* **Monadic Error Handling (`result<T>`)**: Functions return a result container mimicking `std::expected<T, error>`, enforced via `[[nodiscard]]`.

---

## 2. Error Handling & Result Types (`loader/error.hpp`)

All fallible API calls return a `loader::result<T>`.

### Error Codes (`loader::error`)
```cpp
enum class error {
    OK,                // No error
    FILE_NOT_FOUND,    // File/descriptor could not be opened
    SHORT_READ,        // Source returned fewer bytes than requested
    NOT_ELF,           // Bad ELF magic number (0x7f454c46)
    NOT_ARM32,         // Not a 32-bit little-endian ARM object
    NOT_SHARED_OBJECT, // e_type is not ET_DYN
    BAD_SECTION,       // Required section is missing or malformed
    SYMBOL_NOT_FOUND,  // Symbol hash lookup failed
    UNSUPPORTED_RELOC, // Unhandled relocation type
    OUT_OF_MEMORY,     // Fixed-capacity container overflowed
};
```

### Result Interface (`loader::result<T>`)
* `has_value()` / `operator bool()`: Checks if the operation succeeded.
* `err()`: Gets the error code (meaningful if the operation failed).
* `value()` / `operator*` / `operator->`: Accesses the wrapped type.
* `value_or(fallback)`: Returns the success value or a default fallback.

---

## 3. The I/O Layer (`loader/byte-source.hpp`)

`loader::byte_source` is a pure virtual interface providing random-access reads.

```cpp
class byte_source {
public:
    virtual ~byte_source() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};
```

### Out-of-the-Box Implementations (`loader/sources/...`)
1. **`loader::fd_byte_source`**: Backed by a file descriptor. Performs seeks/reads on demand. Used for host environments and target semihosting.
2. **`loader::memory_byte_source`**: Non-owning wrapper over an in-memory buffer (`std::span<const std::byte>`). Used when the whole ELF file is already loaded (e.g. resident in Flash/RAM).

---

## 4. The Parser Core (`loader/elf-image.hpp`)

`loader::elf_image` represents a validated, parsed view of an ARM ELF32 shared library.

```cpp
class elf_image {
public:
    // Factory method: parses and validates ELF/ARM32 header constraints
    static result<elf_image> parse(byte_source& src) noexcept;

    // Direct accessors
    const elf::Ehdr& header() const noexcept;
    byte_source& source() const noexcept;

    // Structural reading
    result<void> program_header(elf::half index, elf::Phdr& out) const noexcept;
    result<void> section_header(elf::half index, elf::Shdr& out) const noexcept;
    result<elf::Shdr> find_section(std::string_view name) const noexcept;

    // Geometry helpers
    result<std::uint32_t> image_size() const noexcept; // Total virtual footprint
    result<std::uint32_t> image_base() const noexcept; // Lowest PT_LOAD vaddr

#if defined(SCL_HOSTED)
    // Combines all SHT_REL sections into one flat vector (requires std::vector)
    result<std::vector<std::byte>> collect_relocations() const noexcept;
#endif
};
```

---

## 5. Usage Example

The following example demonstrates opening a file, validating it as an ELF, extracting segment information, and locating the `.dynamic` section.

```cpp
#include <loader/elf-image.hpp>
#include <loader/sources/fd-byte-source.hpp>
#include <iostream>

void parse_shared_object(const char* filepath) {
    // 1. Open the file descriptor byte source
    auto src_res = loader::fd_byte_source::open_file(filepath);
    if (!src_res) {
        std::cerr << "Failed to open file. Error code: " 
                  << static_cast<int>(src_res.err()) << "\n";
        return;
    }
    loader::fd_byte_source& src = *src_res;

    // 2. Parse and validate the ELF header
    auto img_res = loader::elf_image::parse(src);
    if (!img_res) {
        std::cerr << "Validation failed. Error code: " 
                  << static_cast<int>(img_res.err()) << "\n";
        return;
    }
    loader::elf_image& img = *img_res;

    // 3. Inspect image base and size
    std::cout << "ELF verified successfully.\n"
              << "Image Base Address: 0x" << std::hex << *img.image_base() << "\n"
              << "Image Footprint Size: " << std::dec << *img.image_size() << " bytes\n";

    // 4. Query specific section info (e.g., .dynamic)
    auto dyn_res = img.find_section(".dynamic");
    if (dyn_res) {
        std::cout << "Found .dynamic section at offset 0x" 
                  << std::hex << dyn_res->sh_offset 
                  << ", size: " << std::dec << dyn_res->sh_size << " bytes\n";
    }
}
```

---

## 6. CMake Integration

To consume this library as an interface dependency in your build, include it in your `CMakeLists.txt`:

```cmake
# Pull in the parser subdirectory
add_subdirectory(path/to/common-elf-reader common-elf-reader-build)

# Link your loader or application target against the interface library
target_link_libraries(my_custom_loader PRIVATE common-elf-reader::elf-reader)
```

By default, the library compiles in freestanding mode. If you need standard library allocations (such as `std::vector` for compiling host tests or static relocation collectors), set the hosted configuration variable:

```cmake
set(SCL_HOSTED ON CACHE BOOL "" FORCE)
```

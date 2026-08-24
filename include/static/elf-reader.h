#ifndef ELF_LOADER_ELF_READER_H_
#define ELF_LOADER_ELF_READER_H_

// Standalone shared ELF32 reader/parser (one API for both Cortex-M loaders).
// elf_reader is a thin adapter over loader::elf_image; the relocation, symbol
// and GOT patching below is this project's own and is unchanged.
//
// These MUST precede <elf.h>: loader/elf-types.hpp declares constexpr names
// (SHF_ALLOC, PT_LOAD, EM_ARM, ...) that <elf.h> also defines as macros. Parsed
// first, the constexpr definitions compile; the code below then uses only the
// non-colliding loader::elf::Ehdr/Phdr/Shdr types alongside the Elf32_* API.
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
#include "loader/sources/memory-byte-source.hpp"

#include <elf.h>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <optional>

namespace elf_loader {

struct load_segment {
    uint32_t type{0};
    uint32_t vaddr{0};
    uint32_t memsz{0};
    uint32_t filesz{0};
    uint32_t offset{0};
    uint32_t flags{0};
    uint32_t align{0};
};

struct helper_section {
    uint8_t* address{nullptr};
    uint32_t size{0};
    uint32_t entry_size{0};
    const char* name{nullptr};
};

struct pass_args {
    uint8_t* reldyn_start{nullptr};
    uint32_t reldyn_size{0};
    uint32_t reldyn_entry_size{0};
    uint8_t* dynsym_start{nullptr};
    uint32_t dynsym_size{0};
    uint32_t dynsym_entry_size{0};
    uint8_t* dynstr_start{nullptr};
    uint32_t dynstr_size{0};
    uint8_t* hash_start{nullptr};
    uint32_t hash_size{0};
};

enum class helper_section_idx : uint32_t {
    INITS = 0,
    REL_DYN = 1,
    REL_PLT = 2,
    DYN_SYM = 3,
    DYN_STR = 4,
    HASH = 5,
    COUNT = 6
};

using section_map = helper_section[static_cast<uint32_t>(helper_section_idx::COUNT)];

struct symbol_patch {
    std::string_view name{};
    uint32_t got_address{0};
    uint32_t virtual_address{0};
    uint8_t type{0};
    uint16_t index{0};
    uint8_t binding{0};
};

/// One word written into MAIN's memory during load (address + the value it held
/// before the patch). Collected by patch_main so Module::unload can restore the
/// firmware byte-for-byte.
struct patch_record {
    uint32_t address{0};
    uint32_t original{0};
};

enum class loader_error {
    OK,
    FILE_NOT_FOUND,
    NOT_AN_ELF,
    SECTION_NOT_FOUND,
    INVALID_SEGMENT,
    INVALID_RELOCATION,
    INVALID_SYMBOL,
    DEP_NOT_FOUND,
    OUT_OF_MEMORY
};

inline const char* loader_error_string(loader_error e) {
    switch (e) {
        case loader_error::OK: return "Ok";
        case loader_error::FILE_NOT_FOUND: return "FileNotFound";
        case loader_error::NOT_AN_ELF: return "NotAnElf";
        case loader_error::SECTION_NOT_FOUND: return "SectionNotFound";
        case loader_error::INVALID_SEGMENT: return "InvalidSegment";
        case loader_error::INVALID_RELOCATION: return "InvalidRelocation";
        case loader_error::INVALID_SYMBOL: return "InvalidSymbol";
        case loader_error::DEP_NOT_FOUND: return "DepNotFound";
        case loader_error::OUT_OF_MEMORY: return "OutOfMemory";
    }
    return "Unknown";
}

class symbol_table {
public:
    void add_global(std::string_view name, uint32_t address);
    void set_global(std::string_view name, uint32_t address);
    void add_weak(std::string_view name, uint32_t address);
    uint32_t find(std::string_view name) const;
    bool has(std::string_view name) const;
    // Remove a name and all entries for it (used by Module::unload to unpublish
    // a module's exports). Returns true if anything was removed.
    bool remove(std::string_view name);
    void clear();
private:
    struct Entry {
        std::string_view name;
        uint32_t address;
        bool is_weak;
    };
    static constexpr uint32_t INIT_CAPACITY = 64;
    Entry* entries_{nullptr};
    uint32_t count_{0};
    uint32_t capacity_{0};
    void ensure_capacity();
};

class elf_reader {
public:
    loader_error error{loader_error::OK};

    elf_reader() = default;

    elf_reader(const elf_reader&) = delete;
    elf_reader& operator=(const elf_reader&) = delete;

    // The byte source lives in a unique_ptr so its address is stable: the
    // elf_image holds a byte_source* into it, which must survive moves of the
    // elf_reader (e.g. elf_reader -> shlib_loader).
    elf_reader(elf_reader&&) noexcept = default;
    elf_reader& operator=(elf_reader&&) noexcept = default;
    ~elf_reader() = default;

    bool open_elf(std::string_view file_path);
    bool open_buf(const uint8_t* data, uint32_t size);
    loader_error verify_elf();

    const Elf32_Ehdr& get_header();
    void read_program_header(uint32_t nbr, Elf32_Phdr& phdr);
    void read_section_header(uint32_t nbr, Elf32_Shdr& shdr);
    loader_error read_section_content(uint32_t nbr, std::span<uint8_t> dest);

    // Read @p dest bytes from the raw file/buffer at @p file_offset (used to copy
    // the PT_TLS .tdata image into a freshly allocated TLS block).
    loader_error read_image_data(uint32_t file_offset, std::span<uint8_t> dest);

    uint32_t get_binary_size();
    uint32_t find_section_by_name(std::string_view name);
    uint32_t find_program(uint16_t type);
    std::string_view get_elf_name() const;

    uint32_t get_program_header_count() const;
    void get_segment_info(uint32_t nbr, load_segment& seg);
    static const char* flags_to_string(uint32_t flags);

    loader_error load_segments(std::span<uint8_t> ram_buffer, const Elf32_Phdr& phdr);
    void read_helper_sections(const char* name, helper_section& hs);

    loader_error read_relocation_symbols(const pass_args& pa, std::span<symbol_patch> ssp, uint32_t& count_out);
    uint32_t read_dynamic_symbols(const pass_args& pa, std::span<symbol_patch> ssp, uint32_t start_k, uint32_t& count_out);
    uint32_t calculate_patch_array_size(const pass_args& pa);

    uint32_t find_symbol_address(std::string_view name, const section_map& ms);

    loader_error scan_dependencies(std::vector<std::string>& deps_out);

private:
    void close_file();
    void read_at_offset(std::span<uint8_t> dest, uint32_t offset);
    static bool bounds_check(const pass_args& pa, uint32_t sym_idx, uint32_t str_idx);

    Elf32_Ehdr elf_header_{};
    std::unique_ptr<loader::byte_source> src_{};
    std::optional<loader::elf_image> img_{};
    std::string_view file_name_{};
    uint32_t elf_size_{0};

    friend class shlib_loader;
};

} // namespace elf_loader

#endif // ELF_LOADER_ELF_READER_H_

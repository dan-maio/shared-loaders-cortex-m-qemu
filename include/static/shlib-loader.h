#ifndef ELF_LOADER_SHLIB_LOADER_H_
#define ELF_LOADER_SHLIB_LOADER_H_

#include "elf-reader.h"
#include <vector>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace elf_loader {

enum class lib_type {
    SHLIB,
    MAIN
};

struct weak_main_sym {
    std::string_view name{};
    uint32_t addr{0};
    uint32_t old_addr{0};
};

using pctor = void (*)(void);
using funcm = unsigned int (*)(void);

class shlib_loader {
public:
    shlib_loader() = default;

    explicit shlib_loader(elf_reader&& ef) : efr_(std::move(ef)) {}

    shlib_loader(const shlib_loader&) = delete;
    shlib_loader& operator=(const shlib_loader&) = delete;
    shlib_loader(shlib_loader&&) = default;
    shlib_loader& operator=(shlib_loader&&) = default;

    ~shlib_loader() = default;

    loader_error load_bin(uint8_t* buffer, uint32_t bufsize);
    elf_reader& get_elf();
    const elf_reader& get_elf() const;
    loader_error read_elf(std::string_view file);
    loader_error populate_helper_sections();

    uint8_t* get_ram_shlib();
    uint8_t* get_ram_main();

    loader_error find_shlib_symbols_to_patch(std::span<symbol_patch> ssp);
    loader_error find_main_symbols_to_patch(std::span<symbol_patch> msp);

    symbol_patch find_symbol_by_name(lib_type type, std::string_view name);

    uint32_t get_shlib_patch_count();
    uint32_t get_main_patch_count();

    void allocate_patch_arrays(std::span<symbol_patch> ssp_shlib, std::span<symbol_patch> ssp_main);

    uint32_t patch_shlib(symbol_table* global_syms);
    uint32_t patch_main(symbol_table* global_syms);
    loader_error execute_constructors();

    /// Journal of every word patch_main wrote into main's memory (address +
    /// original value), for a byte-exact unload. Moved out by scl::Module.
    const std::vector<patch_record>& main_journal() const { return main_journal_; }
    std::vector<patch_record> take_journal() { return std::move(main_journal_); }

    /// RTLD_LOCAL/RTLD_GLOBAL: when false, patch_shlib/patch_main do not publish
    /// this library's exports into the shared symbol_table.
    void set_publish(bool p) { publish_ = p; }

    /// Register this module's TLS template and reserve its offset in the shared
    /// per-thread TLS area (multi-thread model). No-op if the image has no PT_TLS
    /// segment. Must be called BEFORE patch_shlib() (which bakes the offset into
    /// the GOT). Returns OK (or OUT_OF_MEMORY).
    loader_error allocate_tls();

    /// This module's byte offset within a thread's TLS area (0 if no TLS).
    uint32_t tls_offset() const { return tls_offset_; }
    unsigned int resolve_weak_function(funcm f);

    loader_error resolve_dependencies(symbol_table* global_syms,
                                     loader_error (*load_dep)(std::string_view soname, shlib_loader& out));

    const std::vector<std::string>& dependencies() const { return dependencies_; }

    void set_ram_main(uint8_t* addr) { ram_main_address_ = addr; }

    symbol_patch* sh_sp{nullptr};
    symbol_patch* main_sp{nullptr};

    uint32_t sh_patch_nbr{0};
    uint32_t main_patch_nbr{0};

    section_map m_shlib{};
    section_map m_main{};

private:
    elf_reader efr_{};
    uint8_t* ram_main_address_{nullptr};
    uint8_t* ram_shlib_address_{nullptr};
    std::vector<weak_main_sym> weak_sym_cache_{};
    std::vector<std::string> dependencies_{};
    std::vector<patch_record> main_journal_{};
    bool publish_{true};
    uint32_t tls_offset_{0};
    uint32_t tls_module_id_{0};
    uint32_t tls_size_{0};
    uint32_t tls_filesz_{0};
    uint32_t tls_data_off_{0};
    bool     has_tls_{false};
    // Auto-loaded DT_NEEDED dependencies are kept here so their loaders (and
    // the parsed images / section maps they own) outlive resolve_dependencies.
    std::vector<std::unique_ptr<shlib_loader>> loaded_deps_{};
};

void sync_after_patching();

} // namespace elf_loader

#endif // ELF_LOADER_SHLIB_LOADER_H_

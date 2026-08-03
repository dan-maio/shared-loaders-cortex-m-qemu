/**
 * @file dynamic-linker.hpp
 * @brief Orchestrates loading, relocation, construction, and unloading of
 *        shared libraries on the target.
 *
 * Symbol resolution is fully automatic — no hardcoded symbol lists.
 * All symbols come from main's .dynsym (parsed from _DYNAMIC) or loaded libraries.
 */
#pragma once

#include "image-memory.hpp"
#include "link-context.hpp"
#include "patch-journal.hpp"
#include "loader/elf-image.hpp"
#include "loader/relocator.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include <memory>
#include <vector>

namespace loader { struct reloc_entry; }

namespace firmware {

/** @brief Represents a loaded shared library with its image, journal, and dynamic section metadata. */
struct library {
    image_memory   image;         ///< Loaded ELF image in RAM
    patch_journal  journal;       ///< Patch journal for dlclose revert
    std::uint32_t base_va{};     ///< Base virtual address of the first LOAD segment
    std::uint32_t dynsym_va{};   ///< VA of .dynsym section in the library
    std::uint32_t dynstr_va{};   ///< VA of .dynstr section in the library
    std::uint32_t hash_va{};     ///< VA of .hash section in the library
    std::uint32_t dynsym_sz{};   ///< Size of .dynsym section
    std::uint32_t dynstr_sz{};   ///< Size of .dynstr section
    std::uint32_t hash_sz{};     ///< Size of .hash section
    void*         init_array{};  ///< Pointer to .init_array in loaded RAM
    std::size_t   init_count{};  ///< Number of constructors in .init_array
    bool          constructed{false}; ///< Whether constructors have been run
};

/** @brief Orchestrates loading, relocation, construction, and unloading of shared libraries.
 *  Symbol resolution is fully automatic via main's .dynsym (parsed from _DYNAMIC). */
class dynamic_linker {
public:
    /** @brief Construct with a reference to the firmware's link_context (exposes .dynsym etc.). */
    dynamic_linker(const link_context& ctx) noexcept;

    /** @brief Parse, load, relocate, and construct from a byte_source. Applications
     *         wrap this with their own storage front-end (semihosting file, ...). */
    [[nodiscard]] loader::result<library*>
    load_from_source(loader::byte_source& src) noexcept;

    /** @brief Unload a library: revert patches, free memory, remove from libs_ vector. */
    void unload(library* lib) noexcept;

    /** @brief Resolve a symbol by name across firmware, loaded libs, and cross-libs.
     *  Fully automatic — no hardcoded symbol lists. */
    [[nodiscard]] loader::result<void*>
    lookup(library* lib, const char* name) noexcept;

    /** @brief Access all loaded libraries (for iteration + unload in main). */
    [[nodiscard]] auto& libs() noexcept { return libs_; }

private:
    const link_context& ctx_;                    ///< Firmware's dynamic section accessor (parses _DYNAMIC)
    std::vector<std::unique_ptr<library>> libs_; ///< All loaded libraries

    /** @brief Apply a relocation plan: patch GOT entries, record journal for revert. */
    [[nodiscard]] loader::result<void>
    apply_plan(loader::relocation_plan& plan, std::byte* base,
               library* lib) noexcept;

    /** @brief Iterate .init_array and call each constructor function. */
    [[nodiscard]] loader::result<void>
    run_constructors(library* lib) noexcept;

    /** @brief Patch the firmware's own GOT entries using symbols from a loaded library. */
    [[nodiscard]] loader::result<void>
    patch_main_got(library* lib) noexcept;

    /**
     * @brief Search all loaded libraries' .dynsym for a symbol.
     * @param name  Symbol name to find.
     * @return Absolute address, or error::SYMBOL_NOT_FOUND.
     */
    [[nodiscard]] loader::result<std::uint32_t>
    resolve_cross(const char* name) const noexcept;

    /**
     * @brief Records a GOT entry that was set to 0 because no library
     *        providing that symbol was loaded yet. After a new library
     *        is loaded, the cross-resolution pass re-patches these.
     */
    struct pending_reloc {
        std::byte* address{};   ///< GOT slot to patch.
        std::uint32_t orig_val{}; ///< Original value before patching.
        char name[32]{};         ///< Symbol name (for cross-resolution).
    };
    std::vector<pending_reloc> pending_;
};

} // namespace firmware

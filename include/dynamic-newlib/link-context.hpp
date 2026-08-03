/**
 * @file link-context.hpp
 * @brief Exposes the firmware main's symbol table for automatic cross-boundary symbol resolution.
 *
 * Two modes:
 *   1. .dynsym populated (-pie/-shared): uses ELF hash lookup
 *   2. Bare-metal: platform code reads .symtab from firmware ELF and passes it via set_symtab()
 */
#pragma once
#include "loader/elf-types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace firmware {

struct reloc_table {
    std::uint32_t reldyn_addr{};
    std::uint32_t reldyn_size{};
    std::uint32_t dynsym_addr{};
    std::uint32_t dynsym_size{};
    std::uint32_t dynstr_addr{};
    std::uint32_t dynstr_size{};
    std::uint32_t hash_addr{};
    std::uint32_t hash_size{};
    std::uint32_t inits_addr{};
    std::uint32_t inits_size{};
};

struct sym_entry { const char* name; std::uint32_t addr; };

class link_context {
public:
    link_context() noexcept;

    /** @brief Set external symtab (read from firmware ELF by platform code). */
    void set_symtab(const sym_entry* entries, std::size_t count) noexcept {
        symtab_ = entries; symtab_count_ = count;
    }

    [[nodiscard]] const reloc_table& table() const noexcept { return table_; }
    [[nodiscard]] std::span<const std::byte> reldyn() const noexcept;
    [[nodiscard]] std::span<const std::byte> dynsym() const noexcept;
    [[nodiscard]] std::span<const std::byte> dynstr() const noexcept;
    [[nodiscard]] std::span<const std::byte> hash() const noexcept;
    [[nodiscard]] std::byte* got_base() const noexcept;
    [[nodiscard]] std::size_t got_size() const noexcept;

    /** @brief Resolve a symbol by name. Returns 0 if not found. */
    [[nodiscard]] std::uint32_t resolve(const char* name) const noexcept;

private:
    reloc_table table_;
    const sym_entry* symtab_ = nullptr;
    std::size_t symtab_count_ = 0;
};

} // namespace firmware

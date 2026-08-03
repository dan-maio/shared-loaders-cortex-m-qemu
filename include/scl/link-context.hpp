/**
 * @file link-context.hpp
 * @brief main's own dynamic-symbol surface, used to resolve library imports.
 */
#pragma once
#include "loader/reloc-applier.hpp"   // symbol_source
#include "loader/symbol-resolver.hpp"

namespace fw {

class link_context {
public:
    /** @brief Build from main's _DYNAMIC (parses DT_HASH/SYMTAB/STRTAB/STRSZ). */
    static loader::result<link_context> from_self() noexcept;

    /** @brief A resolver callback: extern name -> absolute address in main. */
    [[nodiscard]] loader::symbol_source source() const noexcept;

    /** @brief Resolve @p name against main's dynamic symbols (absolute address). */
    [[nodiscard]] loader::result<std::uint32_t> resolve(std::string_view name) const noexcept {
        return resolver_.address_of(name);
    }

private:
    /** @brief Construct from main's .dynsym/.dynstr/.hash spans (used by from_self()). */
    link_context(std::span<const std::byte> dynsym,
                std::span<const std::byte> dynstr,
                std::span<const std::byte> hash) noexcept
        : resolver_(dynsym, dynstr, hash) {}
    loader::symbol_resolver resolver_;   ///< ELF-hash resolver over main's dynamic symbols.
};

} // namespace fw

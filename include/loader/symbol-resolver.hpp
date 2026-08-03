/**
 * @file symbol-resolver.hpp
 * @brief Resolves symbol names to image-relative addresses via the ELF hash table.
 */
#pragma once
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include <cstddef>
#include <span>
#include <string_view>

namespace loader {

/// @brief Looks up symbols in a (.dynsym, .dynstr, .hash) triple.
class symbol_resolver {
public:
    /**
     * @brief Construct over raw section contents.
     * @param dynsym Bytes of .dynsym (array of elf::Sym).
     * @param dynstr Bytes of .dynstr (NUL-separated names).
     * @param hash   Bytes of .hash (nbucket, nchain, bucket[], chain[]).
     */
    symbol_resolver(std::span<const std::byte> dynsym,
                   std::span<const std::byte> dynstr,
                   std::span<const std::byte> hash) noexcept
        : dynsym_(dynsym), dynstr_(dynstr), hash_(hash) {}

    /**
     * @brief Find the value (st_value) of the named symbol.
     * @return The symbol's image-relative value (0 for external symbols).
     * @retval error::SYMBOL_NOT_FOUND No matching name in the hash chain.
     */
    [[nodiscard]] result<std::uint32_t> address_of(std::string_view name) const noexcept;

private:
    std::span<const std::byte> dynsym_;
    std::span<const std::byte> dynstr_;
    std::span<const std::byte> hash_;
};

} // namespace loader

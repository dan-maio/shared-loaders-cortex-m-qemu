/**
 * @file symbol-resolver.cpp
 * @brief Implementation of ELF-hash symbol resolution.
 */
#include "loader/symbol-resolver.hpp"
#include "loader/detail/bytes.hpp"
#include "loader/detail/elf-hash.hpp"

namespace loader {
namespace {

using detail::load_word;
using detail::name_at;

/** @brief st_name (name string-table offset) of dynsym entry @p i. */
std::uint32_t sym_name (std::span<const std::byte> dynsym, std::uint32_t i) noexcept {
    return load_word(dynsym, std::size_t(i) * sizeof(elf::Sym) + 0);
}
/** @brief st_value (symbol value/address) of dynsym entry @p i. */
std::uint32_t sym_value(std::span<const std::byte> dynsym, std::uint32_t i) noexcept {
    return load_word(dynsym, std::size_t(i) * sizeof(elf::Sym) + 4);
}

} // namespace

result<std::uint32_t> symbol_resolver::address_of(std::string_view name) const noexcept {
    const std::uint32_t nbucket = load_word(hash_, 0);
    if (nbucket == 0) return loader::unexpected(error::SYMBOL_NOT_FOUND);
    const std::uint32_t nchain = load_word(hash_, 4);
    const std::size_t bucket_off = 8;                      // after nbucket, nchain
    const std::size_t chain_off  = bucket_off + std::size_t(nbucket) * 4;

    std::uint32_t y = load_word(hash_, bucket_off + (detail::elf_hash(name) % nbucket) * 4);
    // Cap the walk at nchain steps: a valid chain never visits more nodes, and
    // this prevents a malformed cyclic chain from hanging the target.
    for (std::uint32_t steps = 0; y != 0 && steps < nchain; ++steps) {
        if (name == name_at(dynstr_, sym_name(dynsym_, y)))
            return sym_value(dynsym_, y);
        y = load_word(hash_, chain_off + std::size_t(y) * 4);
    }
    return loader::unexpected(error::SYMBOL_NOT_FOUND);
}

} // namespace loader

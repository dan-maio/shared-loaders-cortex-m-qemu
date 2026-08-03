/**
 * @file dynamic-info.hpp
 * @brief Extracts symbol-table pointers from a .dynamic (_DYNAMIC) array.
 */
#pragma once
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include <cstdint>
#include <span>

namespace loader {

struct dynamic_info {
    std::uint32_t hash   = 0;  ///< DT_HASH value (address of .hash).
    std::uint32_t symtab = 0;  ///< DT_SYMTAB value (address of .dynsym).
    std::uint32_t strtab = 0;  ///< DT_STRTAB value (address of .dynstr).
    std::uint32_t strsz  = 0;  ///< DT_STRSZ value (bytes of .dynstr).

    /**
     * @brief Parse a sequence of elf::Dyn until DT_NULL.
     * @param bytes Raw bytes of the .dynamic array.
     * @retval error::BAD_SECTION hash/symtab/strtab missing.
     */
    static result<dynamic_info> parse(std::span<const std::byte> bytes) noexcept;
};

} // namespace loader

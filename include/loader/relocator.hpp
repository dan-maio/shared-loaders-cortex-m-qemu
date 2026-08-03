/**
 * @file relocator.hpp
 * @brief Pure computation of an ARM relocation plan (no memory is written).
 */
#pragma once
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace loader {

/// @brief One computed relocation: what to patch, where, and derived from which symbol.
struct reloc_entry {
    std::uint32_t    offset;    ///< Image-relative location to patch (r_offset).
    elf::rel_type     type;      ///< ARM relocation kind.
    elf::bind        bind;      ///< Binding of the associated symbol.
    std::string_view name;      ///< Symbol name (view into the .dynstr buffer).
    std::uint32_t    sym_value; ///< Image-relative symbol value; 0 => external.
    std::uint32_t    sym_size;  ///< st_size from .dynsym (bytes, used by R_ARM_COPY).
};

/// @brief Sequence of computed relocations. Host: std::vector; target plan revisits in Plan 3.
using relocation_plan = std::vector<reloc_entry>;

/// @brief Builds a relocation_plan from raw .rel.dyn / .dynsym / .dynstr buffers.
class relocator {
public:
    /**
     * @brief Compute the relocation plan for one ELF object.
     * @param reldyn Bytes of .rel.dyn (array of elf::Rel).
     * @param dynsym Bytes of .dynsym (array of elf::Sym).
     * @param dynstr Bytes of .dynstr (NUL-separated names).
     * @return The plan, or an Error.
     * @retval error::UNSUPPORTED_RELOC A relocation type is not recognised.
     * @note Names in the result are views into @p dynstr; keep it alive as long
     *       as the returned relocation_plan is in use.
     */
    static result<relocation_plan> compute(std::span<const std::byte> reldyn,
                                          std::span<const std::byte> dynsym,
                                          std::span<const std::byte> dynstr) noexcept;
};

} // namespace loader

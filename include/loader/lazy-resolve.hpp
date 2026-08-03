/**
 * @file lazy-resolve.hpp
 * @brief Maps a lazily-bound GOT.PLT slot (by image-relative offset) to its
 *        R_ARM_JUMP_SLOT symbol and resolves it — the host-testable core of
 *        phase 5c lazy binding. The firmware trampoline (dl_fixup) supplies the
 *        offset and the resolver chain and performs the actual GOT-slot write.
 */
#pragma once
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include "loader/reloc-applier.hpp"   // symbol_source
#include <cstdint>
#include <span>

namespace loader {

/// The per-library tables needed to resolve one JUMP_SLOT lazily.
struct lazy_tables {
    std::span<const std::byte> jmprel;  ///< JUMP_SLOT relocs (array of elf::Rel).
    std::span<const std::byte> dynsym;  ///< .dynsym (array of elf::Sym).
    std::span<const std::byte> dynstr;  ///< .dynstr (NUL-separated names).
};

/**
 * @brief Resolve the JUMP_SLOT whose GOT slot lives at image-relative byte offset @p got_off.
 *
 * Scans @p t.jmprel for the entry with `r_offset == got_off`, reads its symbol name from
 * (.dynsym, .dynstr), and resolves it via @p resolve (the loaded-libs -> main chain). Returns
 * the absolute target with the Thumb bit (bit 0) set so a `bx` stays in Thumb state.
 *
 * @retval error::UNSUPPORTED_RELOC The matching entry is not an R_ARM_JUMP_SLOT.
 * @retval error::SYMBOL_NOT_FOUND   No JMPREL entry matches @p got_off, or the name does not resolve.
 */
[[nodiscard]] result<std::uint32_t>
lazy_resolve_slot(std::uint32_t got_off, const lazy_tables& t, symbol_source resolve) noexcept;

} // namespace loader

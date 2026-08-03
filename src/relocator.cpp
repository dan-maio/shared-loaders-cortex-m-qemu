/**
 * @file relocator.cpp
 * @brief Implementation of relocation-plan computation.
 */
#include "loader/relocator.hpp"
#include "loader/detail/bytes.hpp"

namespace loader {
namespace {

using detail::load_byte;
using detail::load_word;
using detail::name_at;

/** @brief Map a raw ARM relocation type byte to elf::rel_type (or UnsupportedReloc). */
result<elf::rel_type> classify(std::uint8_t t) noexcept {
    auto rt = static_cast<elf::rel_type>(t);
    switch (rt) {
        case elf::rel_type::ABS32:
        case elf::rel_type::REL32:
        case elf::rel_type::COPY:
        case elf::rel_type::GLOB_DAT:
        case elf::rel_type::JUMP_SLOT:
        case elf::rel_type::RELATIVE:
            return rt;
        default:
            return loader::unexpected(error::UNSUPPORTED_RELOC);
    }
}

/** @brief Map a symbol's st_info binding field to elf::bind (Weak/Global/Local). */
elf::bind classify_bind(std::uint8_t st_info) noexcept {
    switch (elf::st_bind(st_info)) {
        case 2:  return elf::bind::WEAK;
        case 1:  return elf::bind::GLOBAL;
        default: return elf::bind::LOCAL;
    }
}

} // namespace

result<relocation_plan> relocator::compute(std::span<const std::byte> reldyn,
                                          std::span<const std::byte> dynsym,
                                          std::span<const std::byte> dynstr) noexcept {
    relocation_plan plan;
    const std::size_t count = reldyn.size() / sizeof(elf::Rel);
    plan.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t base = i * sizeof(elf::Rel);
        const std::uint32_t r_offset = load_word(reldyn, base + 0);
        const std::uint32_t r_info   = load_word(reldyn, base + 4);

        auto t = classify(elf::r_type(r_info));
        if (!t) return loader::unexpected(t.err());

        const std::uint32_t sym = elf::r_sym(r_info);
        const std::uint32_t sym_count =
            static_cast<std::uint32_t>(dynsym.size() / sizeof(elf::Sym));
        // sym 0 is the null symbol (used by R_ARM_RELATIVE); any other index must
        // be in range, else the .dynsym is malformed.
        if (sym > 0 && sym >= sym_count)
            return loader::unexpected(error::BAD_SECTION);
        const std::size_t   so  = std::size_t(sym) * sizeof(elf::Sym);
        const std::uint32_t st_name  = load_word(dynsym, so + 0);
        const std::uint32_t st_value = load_word(dynsym, so + 4);
        const std::uint32_t st_size  = load_word(dynsym, so + 8);
        const std::uint8_t  st_info  = load_byte(dynsym, so + 12);

        plan.push_back(reloc_entry{
            .offset    = r_offset,
            .type      = *t,
            .bind      = classify_bind(st_info),
            .name      = name_at(dynstr, st_name),
            .sym_value = st_value,
            .sym_size  = st_size,
        });
    }
    return plan;
}

} // namespace loader

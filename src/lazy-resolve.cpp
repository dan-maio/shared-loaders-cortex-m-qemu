/** @file lazy-resolve.cpp @brief See lazy-resolve.hpp. */
#include "loader/lazy-resolve.hpp"
#include "loader/detail/bytes.hpp"

namespace loader {

using detail::load_word;
using detail::name_at;

result<std::uint32_t>
lazy_resolve_slot(std::uint32_t got_off, const lazy_tables& t, symbol_source resolve) noexcept {
    const std::size_t n = t.jmprel.size() / sizeof(elf::Rel);
    for (std::size_t i = 0; i < n; ++i) {
        if (load_word(t.jmprel, i * sizeof(elf::Rel) + 0) != got_off) continue;
        const std::uint32_t r_info = load_word(t.jmprel, i * sizeof(elf::Rel) + 4);
        if (elf::r_type(r_info) != static_cast<std::uint8_t>(elf::rel_type::JUMP_SLOT))
            return loader::unexpected(error::UNSUPPORTED_RELOC);
        const std::uint32_t sym     = elf::r_sym(r_info);
        const std::uint32_t st_name = load_word(t.dynsym, sym * sizeof(elf::Sym));
        const std::string_view name = name_at(t.dynstr, st_name);
        auto target = resolve(name);
        if (!target) return loader::unexpected(target.err());
        return *target | 1u;   // Thumb bit so the trampoline's `bx` stays in Thumb state
    }
    return loader::unexpected(error::SYMBOL_NOT_FOUND);
}

} // namespace loader

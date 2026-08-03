/** @file reloc-applier.cpp @brief See reloc-applier.hpp. */
#include "loader/reloc-applier.hpp"
#include "loader/detail/bytes.hpp"
#include <cstring>   // std::memcpy for R_ARM_COPY

namespace loader {
namespace {

using detail::load_word;
using detail::name_at;
using detail::store_word;
using elf::rel_type;

} // namespace

result<void> reloc_applier::apply_rel(std::span<std::byte> image, std::uint32_t base,
                                     std::span<const std::byte> reldyn,
                                     std::span<const std::byte> dynsym,
                                     std::span<const std::byte> dynstr,
                                     symbol_source resolve) noexcept {
    const std::size_t count = reldyn.size() / sizeof(elf::Rel);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t   rb       = i * sizeof(elf::Rel);
        const std::uint32_t r_offset = load_word(reldyn, rb + 0);
        const std::uint32_t r_info   = load_word(reldyn, rb + 4);
        const std::uint8_t  type     = elf::r_type(r_info);
        const std::uint32_t sym      = elf::r_sym(r_info);

        const std::size_t   so       = std::size_t(sym) * sizeof(elf::Sym);
        const std::uint32_t st_name  = load_word(dynsym, so + 0);
        const std::uint32_t st_value = load_word(dynsym, so + 4);
        const std::uint32_t st_size  = load_word(dynsym, so + 8);   // bytes for R_ARM_COPY

        if (r_offset + 4 > image.size()) return loader::unexpected(error::BAD_SECTION);

        auto sym_target = [&]() -> std::uint32_t {
            if (st_value != 0) return base + st_value;          // defined in this image
            auto r = resolve(name_at(dynstr, st_name));         // external -> resolver
            return r.has_value() ? *r : 0u;                     // unresolved -> 0
        };

        switch (type) {
        case static_cast<std::uint8_t>(rel_type::RELATIVE):
            store_word(image, r_offset, load_word(image, r_offset) + base);
            break;
        case static_cast<std::uint8_t>(rel_type::GLOB_DAT):
        case static_cast<std::uint8_t>(rel_type::JUMP_SLOT):
            store_word(image, r_offset, sym_target());
            break;
        case static_cast<std::uint8_t>(rel_type::ABS32):
            store_word(image, r_offset, load_word(image, r_offset) + sym_target());
            break;
        case static_cast<std::uint8_t>(rel_type::REL32):
            store_word(image, r_offset,
                       load_word(image, r_offset) + sym_target() - (base + r_offset));
            break;
        case static_cast<std::uint8_t>(rel_type::COPY): {
            // Size-aware bound (the blanket +4 check above only guards word-sized stores).
            // Overflow-safe: never compute r_offset + st_size (both u32) directly.
            if (st_size > image.size() || r_offset > image.size() - st_size)
                return loader::unexpected(error::BAD_SECTION);
            const std::uint32_t src = sym_target();          // absolute addr of main's symbol
            if (src != 0 && st_size != 0)                    // unresolved/empty -> leave as-is
                std::memcpy(image.data() + r_offset,
                            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(src)),
                            st_size);
            break;
        }
        default:
            return loader::unexpected(error::UNSUPPORTED_RELOC);
        }
    }
    return {};
}

} // namespace loader

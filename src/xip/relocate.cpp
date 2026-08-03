/**
 * @file relocate.cpp
 * @brief R-X immutability safety scan + split-base relocation apply.
 */
#include "xip/relocate.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"
#include <cstring>

namespace xip {
namespace {
constexpr loader::elf::word SHF_WRITE = 0x1;   ///< writable section flag
}  // namespace

bool text_is_immutable(std::span<const std::byte> image, const SegmentMap& seg) noexcept {
    loader::memory_byte_source src(image);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) return false;

    // Collect the VA ranges of every writable (SHF_WRITE) allocated section.
    // A relocation targeting one of those is legitimate (GOT/.data/.data.rel.ro);
    // only a fixup landing in the *non-writable* R-X region disqualifies XIP.
    struct Range { std::uint32_t lo, hi; };
    Range writable[16];
    std::uint32_t n_wr = 0;
    for (loader::elf::half i = 0; i < parsed->header().e_shnum && n_wr < 16; ++i) {
        loader::elf::Shdr sh{};
        if (auto r = parsed->section_header(i, sh); !r) return false;
        if ((sh.sh_flags & SHF_WRITE) && sh.sh_size > 0)
            writable[n_wr++] = { sh.sh_addr, sh.sh_addr + sh.sh_size };
    }
    auto in_writable = [&](std::uint32_t va) {
        for (std::uint32_t i = 0; i < n_wr; ++i)
            if (va >= writable[i].lo && va < writable[i].hi) return true;
        return false;
    };

    for (loader::elf::half i = 0; i < parsed->header().e_shnum; ++i) {
        loader::elf::Shdr sh{};
        if (auto r = parsed->section_header(i, sh); !r) return false;
        if (sh.sh_type != loader::elf::SHT_REL) continue;

        const loader::elf::word ent = sh.sh_entsize ? sh.sh_entsize
                                                    : sizeof(loader::elf::Rel);
        const loader::elf::word count = ent ? sh.sh_size / ent : 0;
        for (loader::elf::word k = 0; k < count; ++k) {
            loader::elf::Rel rel{};
            if (auto r = src.read_at(sh.sh_offset + k * ent,
                    std::span<std::byte>(reinterpret_cast<std::byte*>(&rel), sizeof(rel)));
                !r)
                return false;
            if (in_writable(rel.r_offset)) continue;      // legit RW fixup
            if (seg.in_text(rel.r_offset)) return false;  // a fixup into R-X code/rodata
        }
    }
    return true;
}

loader::result<void> apply_rel_xip(const SegmentMap& seg,
                                   std::uintptr_t text_base,
                                   std::uintptr_t data_base,
                                   std::span<std::byte> ram_data,
                                   std::span<const std::byte> reldyn,
                                   std::span<const std::byte> dynsym,
                                   std::span<const std::byte> dynstr,
                                   loader::symbol_source resolve) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::name_at;
    using loader::error;
    using RT = loader::elf::rel_type;

    // Resolve a symbol value to its runtime address, routed to the right base.
    auto sym_target = [&](std::uint32_t st_value, std::uint32_t st_name) -> std::uint32_t {
        if (st_value != 0)
            return seg.in_text(st_value) ? text_base + (st_value - seg.text_vaddr)
                                         : data_base + (st_value - seg.data_vaddr);
        auto r = resolve(name_at(dynstr, st_name));
        return r.has_value() ? *r : 0u;
    };

    const std::size_t count = reldyn.size() / sizeof(loader::elf::Rel);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t   rb       = i * sizeof(loader::elf::Rel);
        const std::uint32_t r_offset = load_word(reldyn, rb + 0);
        const std::uint32_t r_info   = load_word(reldyn, rb + 4);
        const auto          type     = static_cast<RT>(loader::elf::r_type(r_info));
        const std::uint32_t sym      = loader::elf::r_sym(r_info);

        const std::size_t   so       = std::size_t(sym) * sizeof(loader::elf::Sym);
        const std::uint32_t st_name  = load_word(dynsym, so + 0);
        const std::uint32_t st_value = load_word(dynsym, so + 4);
        const std::uint32_t st_size  = load_word(dynsym, so + 8);

        // Only RW words are patched — the R-X segment executes read-only from flash.
        if (r_offset < seg.data_vaddr || r_offset + 4 > seg.data_vaddr + seg.data_memsz)
            return loader::unexpected(error::BAD_SECTION);
        const std::size_t off = r_offset - seg.data_vaddr;
        if (off + 4 > ram_data.size())
            return loader::unexpected(error::BAD_SECTION);

        switch (type) {
        case RT::RELATIVE: {                       // *P += (addend in text? text_bias : data_bias)
            const std::uint32_t addend = load_word(ram_data, off);
            const std::uint32_t bias = seg.in_text(addend) ? text_base - seg.text_vaddr
                                                           : data_base - seg.data_vaddr;
            store_word(ram_data, off, addend + bias);
            break;
        }
        case RT::GLOB_DAT:
        case RT::JUMP_SLOT:
            store_word(ram_data, off, sym_target(st_value, st_name));
            break;
        case RT::ABS32:
            store_word(ram_data, off, load_word(ram_data, off) + sym_target(st_value, st_name));
            break;
        case RT::REL32: {
            const std::uint32_t P = data_base + off;
            store_word(ram_data, off, load_word(ram_data, off) + sym_target(st_value, st_name) - P);
            break;
        }
        case RT::COPY: {
            if (st_size > ram_data.size() || off > ram_data.size() - st_size)
                return loader::unexpected(error::BAD_SECTION);
            const std::uint32_t src = sym_target(st_value, st_name);
            if (src != 0 && st_size != 0)
                std::memcpy(ram_data.data() + off,
                            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(src)), st_size);
            break;
        }
        default:
            return loader::unexpected(error::UNSUPPORTED_RELOC);
        }
    }
    return {};
}

}  // namespace xip

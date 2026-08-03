/**
 * @file elf-image.cpp
 * @brief Implementation of elf_image parsing and section access.
 */
#include "loader/elf-image.hpp"
#include <cstring>
#include <type_traits>

namespace loader {
namespace {

/// @brief Read a trivially-copyable POD of type T at @p offset.
template <class T>
result<T> read_pod(byte_source& src, std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "read_pod requires trivially-copyable T");
    T out{};
    std::byte buf[sizeof(T)];
    auto r = src.read_at(offset, std::span<std::byte>(buf, sizeof(T)));
    if (!r) return loader::unexpected(r.err());
    std::memcpy(&out, buf, sizeof(T)); // host is little-endian; layout matches ABI
    return out;
}

} // namespace

result<elf_image> elf_image::parse(byte_source& src) noexcept {
    auto eh = read_pod<elf::Ehdr>(src, 0);
    if (!eh) return loader::unexpected(eh.err());
    const elf::Ehdr& h = *eh;

    if (h.e_ident[0] != elf::ELFMAG0 || h.e_ident[1] != elf::ELFMAG1 ||
        h.e_ident[2] != elf::ELFMAG2 || h.e_ident[3] != elf::ELFMAG3)
        return loader::unexpected(error::NOT_ELF);

    if (h.e_ident[elf::EI_CLASS] != elf::ELFCLASS32 ||
        h.e_ident[elf::EI_DATA]  != elf::ELFDATA2LSB ||
        h.e_machine != elf::EM_ARM)
        return loader::unexpected(error::NOT_ARM32);

    return elf_image(src, h);
}

result<void> elf_image::program_header(elf::half index, elf::Phdr& out) const noexcept {
    if (index >= ehdr_.e_phnum) return loader::unexpected(error::BAD_SECTION);
    auto p = read_pod<elf::Phdr>(*src_, ehdr_.e_phoff + std::size_t(index) * sizeof(elf::Phdr));
    if (!p) return loader::unexpected(p.err());
    out = *p;
    return {};
}

result<void> elf_image::section_header(elf::half index, elf::Shdr& out) const noexcept {
    if (index >= ehdr_.e_shnum) return loader::unexpected(error::BAD_SECTION);
    auto s = read_pod<elf::Shdr>(*src_, ehdr_.e_shoff + std::size_t(index) * sizeof(elf::Shdr));
    if (!s) return loader::unexpected(s.err());
    out = *s;
    return {};
}

result<elf::Shdr> elf_image::find_section(std::string_view name) const noexcept {
    elf::Shdr strtab{};
    if (auto r = section_header(ehdr_.e_shstrndx, strtab); !r)
        return loader::unexpected(r.err());

    for (elf::half i = 0; i < ehdr_.e_shnum; ++i) {
        elf::Shdr sh{};
        if (auto r = section_header(i, sh); !r) return loader::unexpected(r.err());
        // Read the name (bounded to 31 chars) from the section string table.
        char buf[32] = {};
        std::size_t name_off = std::size_t(strtab.sh_offset) + sh.sh_name;
        std::size_t avail = (name_off < src_->size()) ? (src_->size() - name_off) : 0;
        std::size_t want = avail < 31 ? avail : 31;
        if (want) {
            auto rr = src_->read_at(name_off, std::span<std::byte>(reinterpret_cast<std::byte*>(buf), want));
            if (!rr) return loader::unexpected(rr.err());
        }
        if (name == buf) return sh;
    }
    return loader::unexpected(error::BAD_SECTION);
}

result<std::uint32_t> elf_image::image_size() const noexcept {
    // The loaded image is vaddr-addressed (image[0] == vaddr 0), so its footprint is
    // the highest PT_LOAD end address, NOT the sum of segment sizes: segments may be
    // page-aligned with gaps between them (e.g. libcppsh: code at 0, data at 0x14b0).
    // Summing filesz under-sizes the buffer and corrupts vaddr-based relocation.
    std::uint32_t high = 0; // 32-bit (fixes the original uint16_t overflow bug)
    for (elf::half i = 0; i < ehdr_.e_phnum; ++i) {
        elf::Phdr ph{};
        if (auto r = program_header(i, ph); !r) return loader::unexpected(r.err());
        if (ph.p_type != elf::PT_LOAD) continue;
        const std::uint32_t end = ph.p_vaddr + ph.p_memsz; // segment's end in memory
        if (end > high) high = end;
    }
    return high;
}

result<std::uint32_t> elf_image::image_base() const noexcept {
    std::uint32_t low = 0xFFFFFFFFu;
    for (elf::half i = 0; i < ehdr_.e_phnum; ++i) {
        elf::Phdr ph{};
        if (auto r = program_header(i, ph); !r) return loader::unexpected(r.err());
        if (ph.p_type != elf::PT_LOAD) continue;
        if (ph.p_vaddr < low) low = ph.p_vaddr;
    }
    return low == 0xFFFFFFFFu ? 0u : low;
}

#if defined(SCL_HOSTED)
result<std::vector<std::byte>> elf_image::collect_relocations() const noexcept {
    std::vector<std::byte> all;
    for (elf::half i = 0; i < ehdr_.e_shnum; ++i) {
        elf::Shdr sh{};
        if (auto r = section_header(i, sh); !r) return loader::unexpected(r.err());
        if (sh.sh_type != elf::SHT_REL) continue;
        if (!(sh.sh_flags & elf::SHF_ALLOC)) continue;
        std::size_t pos = all.size();
        all.resize(pos + sh.sh_size);
        if (auto r = src_->read_at(sh.sh_offset, std::span<std::byte>(all.data() + pos, sh.sh_size)); !r)
            return loader::unexpected(r.err());
    }
    return all;
}
#endif // SCL_HOSTED

} // namespace loader

#pragma once
#include "loader/elf-image.hpp"
#include "loader/detail/bytes.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace test {

inline std::span<const std::byte> section_bytes(
    const loader::elf_image& img,
    std::span<const std::byte> file,
    const char* name)
{
    auto sh = img.find_section(name);
    if (!sh) return {};
    return file.subspan(sh->sh_offset, sh->sh_size);
}

inline std::vector<std::byte> dynsym_two(
    std::uint32_t st_name_0, std::uint32_t st_value_0,
    std::uint32_t st_name_1, std::uint32_t st_value_1)
{
    std::vector<std::byte> buf(sizeof(loader::elf::Sym) * 2);
    loader::detail::store_word(buf, 0,  st_name_0);
    loader::detail::store_word(buf, 4,  st_value_0);
    loader::detail::store_word(buf, 8,  0);
    loader::detail::store_word(buf, 16, st_name_1);
    loader::detail::store_word(buf, 20, st_value_1);
    loader::detail::store_word(buf, 24, 0);
    return buf;
}

inline std::uint32_t word_at(std::span<const std::byte> sp, std::size_t off) {
    return loader::detail::load_word(sp, off);
}

inline std::vector<std::byte> rel(std::uint32_t r_offset, std::uint32_t r_info) {
    std::vector<std::byte> buf(8);
    loader::detail::store_word(buf, 0, r_offset);
    loader::detail::store_word(buf, 4, r_info);
    return buf;
}

inline void put_le32(std::vector<std::byte>& out, std::uint32_t v) {
    auto it = out.insert(out.end(), 4, std::byte{});
    loader::detail::store_word({&*it, 4}, 0, v);
}

} // namespace test

#pragma once
#include <cstdint>
#include <string_view>

namespace loader::detail {

inline std::uint32_t elf_hash(std::string_view name) noexcept {
    std::uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        std::uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

} // namespace loader::detail

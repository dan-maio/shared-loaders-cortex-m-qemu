/** @file dynamic-info.cpp @brief See dynamic-info.hpp. */
#include "loader/dynamic-info.hpp"
#include <cstring>

namespace loader {

result<dynamic_info> dynamic_info::parse(std::span<const std::byte> bytes) noexcept {
    dynamic_info info;
    const std::size_t n = bytes.size() / sizeof(elf::Dyn);
    for (std::size_t i = 0; i < n; ++i) {
        elf::Dyn d;
        std::memcpy(&d, bytes.data() + i * sizeof(elf::Dyn), sizeof(elf::Dyn));
        if (d.d_tag == elf::DT_NULL) break;
        switch (d.d_tag) {
        case elf::DT_HASH:   info.hash   = d.d_val; break;
        case elf::DT_SYMTAB: info.symtab = d.d_val; break;
        case elf::DT_STRTAB: info.strtab = d.d_val; break;
        case elf::DT_STRSZ:  info.strsz  = d.d_val; break;
        default: break;
        }
    }
    if (info.hash == 0 || info.symtab == 0 || info.strtab == 0)
        return loader::unexpected(error::BAD_SECTION);
    return info;
}

} // namespace loader

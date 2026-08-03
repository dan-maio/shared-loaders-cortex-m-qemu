/**
 * @file bytes.hpp
 * @brief Bounds-checked little-endian byte access into ELF byte spans.
 *
 * The relocator, reloc-applier and symbol-resolver all decode 32-bit words,
 * single bytes and NUL-terminated names out of raw ELF section spans. These
 * helpers were duplicated across all three; they live here now that there are
 * 3+ consumers. Every accessor is bounds-checked (out-of-range reads yield 0,
 * stores are no-ops) so a malformed/truncated section can never read or write
 * past its span. Header-only and freestanding (no POSIX `strnlen`) so the core
 * stays usable on the bare-metal target.
 */
#ifndef LOADER_DETAIL_BYTES_HPP
#define LOADER_DETAIL_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace loader::detail {

/** @brief Little-endian 32-bit read; out-of-range returns 0. */
inline std::uint32_t load_word(std::span<const std::byte> s, std::size_t off) noexcept {
    std::uint32_t v = 0;
    if (off + sizeof(v) <= s.size()) std::memcpy(&v, s.data() + off, sizeof(v));
    return v;
}

/** @brief Single byte read; out-of-range returns 0. */
inline std::uint8_t load_byte(std::span<const std::byte> s, std::size_t off) noexcept {
    return off < s.size() ? std::to_integer<std::uint8_t>(s[off]) : std::uint8_t{0};
}

/** @brief Little-endian 32-bit write; out-of-range is a no-op. */
inline void store_word(std::span<std::byte> s, std::size_t off, std::uint32_t v) noexcept {
    if (off + sizeof(v) <= s.size()) std::memcpy(s.data() + off, &v, sizeof(v));
}

/**
 * @brief NUL-terminated string at byte offset @p off into a string table.
 * @return Span-bounded view; empty if @p off is out of range. The scan is
 *         hand-rolled (not `strnlen`, which is POSIX-only) for freestanding use.
 */
inline std::string_view name_at(std::span<const std::byte> tab, std::uint32_t off) noexcept {
    if (off >= tab.size()) return {};
    const char* p = reinterpret_cast<const char*>(tab.data()) + off;
    const std::size_t max = tab.size() - off;
    std::size_t len = 0;
    while (len < max && p[len] != '\0') ++len;
    return std::string_view(p, len);
}

struct rel_slot {
    std::uint32_t r_offset;
    std::uint32_t r_info;
};

inline void for_each_rel(std::span<const std::byte> rel, auto fn) {
    for (std::size_t o = 0; o + 8 <= rel.size(); o += 8)
        fn(load_word(rel, o), load_word(rel, o + 4));
}

} // namespace loader::detail

#endif // LOADER_DETAIL_BYTES_HPP

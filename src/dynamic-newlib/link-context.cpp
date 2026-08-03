/**
 * @file link-context.cpp
 * @brief Reads _reloc table; accepts external .symtab from platform code.
 */
#include "dynamic-newlib/link-context.hpp"
#include "loader/dynamic-info.hpp"
#include <cstring>

extern "C" const std::uint32_t _reloc;
extern "C" std::byte _sgot[];
extern "C" std::byte _esgot[];
extern "C" __attribute__((weak)) loader::elf::Dyn _DYNAMIC[];

namespace firmware {

static bool valid_addr(std::uint32_t addr, std::uint32_t size) noexcept {
    if (size == 0) return true;
    if (addr >= 0x08000000 && addr + size <= 0x08080000) return true;
    if (addr >= 0x20000000 && addr + size <= 0x20020000) return true;
    return false;
}

link_context::link_context() noexcept {
    auto t = table_;
    const auto* p = &_reloc;
    t.reldyn_addr = p[0]; t.reldyn_size = p[1];
    t.dynsym_addr = p[2]; t.dynsym_size = p[3];
    t.dynstr_addr = p[4]; t.dynstr_size = p[5];
    t.hash_addr   = p[6]; t.hash_size   = p[7];
    t.inits_addr  = p[8]; t.inits_size  = p[9];
    if (!valid_addr(t.reldyn_addr, t.reldyn_size) && t.reldyn_size != 0) t.reldyn_addr = t.reldyn_size = 0;
    if (!valid_addr(t.dynsym_addr, t.dynsym_size) && t.dynsym_size != 0) t.dynsym_addr = t.dynsym_size = 0;
    if (!valid_addr(t.dynstr_addr, t.dynstr_size) && t.dynstr_size != 0) t.dynstr_addr = t.dynstr_size = 0;
    if (!valid_addr(t.hash_addr, t.hash_size) && t.hash_size != 0) t.hash_addr = t.hash_size = 0;
    if (!valid_addr(t.inits_addr, t.inits_size) && t.inits_size != 0) t.inits_addr = t.inits_size = 0;
    table_ = t;
}

std::uint32_t link_context::resolve(const char* name) const noexcept {
    /* Linear scan over .symtab (provided by platform code) */
    if (symtab_ && symtab_count_ > 0) {
        for (std::size_t i = 0; i < symtab_count_; ++i) {
            if (std::strcmp(symtab_[i].name, name) == 0)
                return symtab_[i].addr;
        }
    }
    return 0;
}

std::span<const std::byte> link_context::reldyn() const noexcept {
    return {reinterpret_cast<const std::byte*>(table_.reldyn_addr), table_.reldyn_size};
}
std::span<const std::byte> link_context::dynsym() const noexcept {
    return {reinterpret_cast<const std::byte*>(table_.dynsym_addr), table_.dynsym_size};
}
std::span<const std::byte> link_context::dynstr() const noexcept {
    return {reinterpret_cast<const std::byte*>(table_.dynstr_addr), table_.dynstr_size};
}
std::span<const std::byte> link_context::hash() const noexcept {
    return {reinterpret_cast<const std::byte*>(table_.hash_addr), table_.hash_size};
}
std::byte* link_context::got_base() const noexcept { return _sgot; }
std::size_t link_context::got_size() const noexcept { return _esgot - _sgot; }

} // namespace firmware

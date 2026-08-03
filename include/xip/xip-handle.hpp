#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "loader/symbol-resolver.hpp"

namespace xip {

/// Runtime segment placement of a loaded module.
struct xip_map {
    bool           is_xip     = false;
    std::uintptr_t text_base  = 0;
    std::uint32_t  text_size  = 0;
    std::uintptr_t data_base  = 0;
    std::uintptr_t got_base   = 0;
    std::uint32_t  data_memsz = 0;
};

template <typename fallback_t = void*>
struct basic_xip_handle {
    bool          in_use = false;
    bool          xip    = false;
    char          name[16] = {0};
    std::uintptr_t text_base  = 0;
    std::uint32_t  text_vaddr = 0, text_size = 0;
    std::uintptr_t data_base  = 0;
    std::uintptr_t got_base   = 0;
    std::uint32_t  data_vaddr = 0, data_memsz = 0;
    std::uintptr_t flash_data_base = 0;
    loader::symbol_resolver resolver{ {}, {}, {} };
    std::span<const std::byte> reldyn{}, relplt{}, dynsym{}, dynstr{};
    std::byte*    ram = nullptr;
    fallback_t     fb = nullptr;
};

using xip_handle = basic_xip_handle<void*>;

} // namespace xip

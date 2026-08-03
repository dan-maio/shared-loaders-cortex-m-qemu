#pragma once
#include "xip/xip-handle.hpp"
#include "xip/segment-map.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/detail/bytes.hpp"
#include "loader/reloc-applier.hpp"
#include <cstring>

namespace xip::detail {

template <typename handle_t>
inline void* sym_addr(const handle_t* h, std::uint32_t st) noexcept {
    const bool in_d = st >= h->data_vaddr && st < h->data_vaddr + h->data_memsz;
    const bool in_t = st >= h->text_vaddr && st < h->text_vaddr + h->text_size && !in_d;
    return reinterpret_cast<void*>(in_t ? h->text_base + (st - h->text_vaddr)
                                        : h->data_base + (st - h->data_vaddr));
}

template <typename handle_t>
inline void* dlsym_impl(void* handle, const char* name) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    if (!h || !h->in_use) return nullptr;
    auto v = h->resolver.address_of(name);
    return (v && *v != 0) ? sym_addr(h, *v) : nullptr;
}

template <typename handle_t>
inline std::uint32_t get_got_base_impl(void* handle) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    return (h && h->in_use) ? static_cast<std::uint32_t>(h->got_base) : 0;
}

template <typename handle_t>
inline bool is_xip_impl(void* handle) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    return h && h->in_use && h->xip;
}

template <typename handle_t, typename map_t>
inline bool get_xip_map_impl(void* handle, map_t& out) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    if (!h || !h->in_use) return false;
    out.is_xip     = h->xip;
    out.text_base  = h->text_base;
    out.text_size  = h->text_size;
    out.data_base  = h->data_base;
    out.got_base   = h->got_base;
    out.data_memsz = h->data_memsz;
    return true;
}

inline int patch_main_imports_impl(std::span<std::byte> image,
                                    std::uintptr_t trampoline_addr) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::for_each_rel;

    loader::memory_byte_source src(image);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) return -1;
    auto img = *parsed;

    auto sec = [&](const char* nm) -> std::span<const std::byte> {
        auto sh = img.find_section(nm);
        if (!sh || sh->sh_size == 0) return {};
        return std::span<const std::byte>{ image.data() + sh->sh_offset, sh->sh_size };
    };
    auto dynsym = sec(".dynsym");
    if (dynsym.empty()) return 0;

    const std::uint32_t tramp = static_cast<std::uint32_t>(trampoline_addr) | 1u;
    int n = 0;

    auto patcher = [&](std::uint32_t r_offset, std::uint32_t r_info) {
        if (loader::elf::r_type(r_info) !=
            static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) return;
        const std::uint32_t sym = loader::elf::r_sym(r_info);
        const std::uint32_t st_value = load_word(dynsym, sym * sizeof(loader::elf::Sym) + 4);
        if (st_value != 0) return;
        if (r_offset + 4 > image.size()) return;
        store_word(image, r_offset, tramp);
        ++n;
    };

    for (const char* s : { ".rel.plt", ".rel.dyn" }) {
        auto rel = sec(s);
        for_each_rel(rel, patcher);
    }
    return n;
}

} // namespace xip::detail

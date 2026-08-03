#include "xip/dynamic-nolib/adapter.hpp"
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include "xip/xip-handle.hpp"
#include "../adapter-base.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/detail/bytes.hpp"
#include "scl/link-context.hpp"
#include "scl/image-memory.hpp"
#include "scl/pic-trampoline.h"
#include "dynamic-nolib/dl.hpp"
#include <cstring>
#include "scl/log.h"

extern "C" void xip_cross_call(void);
extern "C" std::uint64_t xip_cross_lookup(std::uint32_t) noexcept;
extern "C" void dl_exidx_register(std::uint32_t, std::uint32_t, std::uint32_t, int);
extern "C" void dl_exidx_unregister(std::uint32_t);

namespace xip::nolib {
namespace {

constexpr loader::elf::word SHF_WRITE    = 0x1;
constexpr loader::elf::word SHT_PROGBITS = 1;

xip_handle g_handles[8];

struct main_resolver {
    fw::link_context* main;
    static loader::result<std::uint32_t> thunk(void* ctx, std::string_view n) noexcept {
        return static_cast<main_resolver*>(ctx)->main->resolve(n);
    }
    loader::symbol_source source() noexcept { return { &thunk, this }; }
};

}  // namespace

void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::name_at;
    using loader::detail::for_each_rel;
    using RT = loader::elf::rel_type;

    xip_handle* h = nullptr;
    for (auto& hh : g_handles) if (!hh.in_use) { h = &hh; break; }
    if (!h) return nullptr;

    loader::memory_byte_source parse_src(so_image);
    auto parsed = loader::elf_image::parse(parse_src);
    if (!parsed) return nullptr;
    auto img = *parsed;

    SegmentMap seg;
    const bool can_xip = classify_segments(so_image, seg) && seg.split &&
                         text_is_immutable(so_image, seg);
    scl_log_puts("  [dbg] dlopen(");
    scl_log_puts(name);
    scl_log_puts(") can_xip=");
    scl_log_put_hex("", can_xip);
    scl_log_puts(" split=");
    scl_log_put_hex("", seg.split);
    scl_log_puts("\n");

    if (!can_xip) {
        constexpr loader::elf::word PT_LOAD = 1;
        std::uint32_t min_va = ~0u, max_va = 0;
        for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
            loader::elf::Phdr ph{};
            if (auto r = img.program_header(i, ph); !r) continue;
            if (ph.p_type != PT_LOAD) continue;
            const std::uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (ph.p_vaddr < min_va) min_va = ph.p_vaddr;
            if (end       > max_va) max_va = end;
        }
        if (min_va == ~0u) return nullptr;
        const std::size_t total = max_va - min_va;

        auto memp = fw::image_arena().alloc(total);
        if (!memp) return nullptr;
        auto* ram = static_cast<std::byte*>(*memp);
        std::memset(ram, 0, total);

        for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
            loader::elf::Phdr ph{};
            if (auto r = img.program_header(i, ph); !r) continue;
            if (ph.p_type != PT_LOAD) continue;
            const std::size_t dest_off = ph.p_vaddr - min_va;
            if (ph.p_filesz > 0)
                std::memcpy(ram + dest_off, so_image.data() + ph.p_offset, ph.p_filesz);
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(ram);
        const std::uintptr_t text_base = base + (seg.text_vaddr - min_va);
        const std::uintptr_t data_base = base + (seg.data_vaddr - min_va);

        std::uintptr_t got_base = data_base;
        if (auto g = img.find_section(".got"); g && g->sh_addr >= seg.data_vaddr)
            got_base = data_base + (g->sh_addr - seg.data_vaddr);

        auto flash_sec = [&](const char* nm) -> std::span<const std::byte> {
            auto sh = img.find_section(nm);
            if (!sh || sh->sh_size == 0) return {};
            return { so_image.data() + sh->sh_offset, sh->sh_size };
        };
        auto dynsym = flash_sec(".dynsym");
        auto dynstr = flash_sec(".dynstr");
        auto hashsp = flash_sec(".hash");
        auto reldyn  = flash_sec(".rel.dyn");
        auto relplt  = flash_sec(".rel.plt");

        auto linkr = fw::link_context::from_self();
        if (!linkr) { fw::image_arena().free(ram); return nullptr; }
        main_resolver mr{ &*linkr };
        auto resolve = mr.source();

        constexpr std::size_t ES = sizeof(loader::elf::Sym);
        const std::uint32_t n_sym = static_cast<std::uint32_t>(dynsym.size() / ES);
        std::span<std::byte> all_ram{ ram, total };
        for (const char* s : { ".rel.dyn", ".rel.plt" }) {
            auto rel = flash_sec(s);
            for_each_rel(rel, [&](std::uint32_t r_off, std::uint32_t r_info) {
                const auto type = loader::elf::r_type(r_info);
                const std::size_t slot = r_off - min_va;
                if (slot + 4 > total) return;

                const std::uint32_t sym = loader::elf::r_sym(r_info);
                const std::uint32_t st_val = (sym < n_sym) ? load_word(dynsym, sym * ES + 4) : 0u;
                const std::uint32_t st_nam = (sym < n_sym) ? load_word(dynsym, sym * ES + 0) : 0u;
                auto target = [&]() -> std::uint32_t {
                    if (st_val != 0)
                        return seg.in_text(st_val) ? text_base + (st_val - seg.text_vaddr)
                                                    : data_base + (st_val - seg.data_vaddr);
                    auto rv = resolve(name_at(dynstr, st_nam));
                    return rv.has_value() ? *rv : 0u;
                };

                if (type == static_cast<std::uint8_t>(RT::RELATIVE)) {
                    const std::uint32_t A = load_word(all_ram, slot);
                    store_word(all_ram, slot, A + (seg.in_text(A) ? text_base - seg.text_vaddr
                                                                   : data_base - seg.data_vaddr));
                }
                else if (type == static_cast<std::uint8_t>(RT::GLOB_DAT) ||
                         type == static_cast<std::uint8_t>(RT::JUMP_SLOT)) {
                    store_word(all_ram, slot, target());
                }
                else if (type == static_cast<std::uint8_t>(RT::ABS32)) {
                    store_word(all_ram, slot, load_word(all_ram, slot) + target());
                }
                else if (type == static_cast<std::uint8_t>(RT::REL32)) {
                    const std::uint32_t P = text_base + (r_off - seg.text_vaddr);
                    store_word(all_ram, slot, load_word(all_ram, slot) + target() - P);
                }
            });
        }

        const std::uint32_t tramp =
            reinterpret_cast<std::uint32_t>(&xip_cross_call) | 1u;
        for (const char* s : { ".rel.dyn", ".rel.plt" }) {
            auto rel = flash_sec(s);
            for_each_rel(rel, [&](std::uint32_t r_off, std::uint32_t r_info) {
                if (loader::elf::r_type(r_info) !=
                    static_cast<std::uint8_t>(RT::JUMP_SLOT)) return;
                const std::size_t slot_off = r_off - min_va;
                if (slot_off + 4 <= total)
                    store_word(all_ram, slot_off, tramp);
            });
        }

        h->in_use      = true;
        h->xip         = false;
        h->text_base   = text_base;
        h->text_vaddr  = seg.text_vaddr;
        h->text_size   = seg.text_size;
        h->data_base   = data_base;
        h->got_base    = got_base;
        h->data_vaddr  = seg.data_vaddr;
        h->data_memsz  = seg.data_memsz;
        h->resolver    = loader::symbol_resolver(dynsym, dynstr, hashsp);
        h->reldyn      = reldyn;
        h->relplt      = relplt;
        h->dynsym      = dynsym;
        h->dynstr      = dynstr;
        h->ram         = ram;
        std::strncpy(h->name, name, sizeof(h->name) - 1);

        if (auto sh = img.find_section(".init_array"); sh && sh->sh_size >= 4) {
            const std::size_t off = sh->sh_addr - min_va;
            for (std::size_t o = 0; o + 4 <= sh->sh_size; o += 4) {
                std::uint32_t fn = 0;
                std::memcpy(&fn, ram + off + o, 4);
                if (fn && fn != 0xffffffffu)
                    pic_call_void(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(got_base));
            }
        }

        if (auto ex = img.find_section(".ARM.exidx"); ex && ex->sh_size) {
            const std::uint32_t start = base + (ex->sh_addr - min_va);
            dl_exidx_register(start, start + ex->sh_size, base, 0);
        }

        return h;
    }

    // ---- XIP load ----
    {
        std::uint32_t va_min = 0, off_min = 0, va_max = 0, prog_max = 0;
        bool any = false;
        for (loader::elf::half i = 0; i < img.header().e_shnum; ++i) {
            loader::elf::Shdr sh{};
            if (auto r = img.section_header(i, sh); !r) continue;
            if (!(sh.sh_flags & SHF_WRITE) || sh.sh_size == 0) continue;
            const std::uint32_t end = sh.sh_addr + sh.sh_size;
            if (!any) { va_min = sh.sh_addr; off_min = sh.sh_offset; va_max = end; prog_max = sh.sh_addr; any = true; }
            if (sh.sh_addr   < va_min)  va_min  = sh.sh_addr;
            if (sh.sh_offset < off_min) off_min = sh.sh_offset;
            if (end > va_max) va_max = end;
            if (sh.sh_type == SHT_PROGBITS && end > prog_max) prog_max = end;
        }
        if (any) {
            seg.data_vaddr  = va_min;
            seg.data_off    = off_min;
            seg.data_memsz  = va_max - va_min;
            seg.data_filesz = (prog_max > va_min) ? prog_max - va_min : 0;
        }
    }

    auto memp = fw::image_arena().alloc(seg.data_memsz);
    if (!memp) return nullptr;
    auto* ram = static_cast<std::byte*>(*memp);
    std::memset(ram, 0, seg.data_memsz);
    if (seg.data_filesz)
        std::memcpy(ram, so_image.data() + seg.data_off, seg.data_filesz);

    const std::uintptr_t blob      = reinterpret_cast<std::uintptr_t>(so_image.data());
    const std::uintptr_t text_base = blob + seg.text_off;
    const std::uintptr_t data_base = reinterpret_cast<std::uintptr_t>(ram);
    std::uintptr_t got_base = data_base;
    if (auto g = img.find_section(".got"); g && g->sh_addr >= seg.data_vaddr)
        got_base = data_base + (g->sh_addr - seg.data_vaddr);

    auto flash_sec = [&](const char* nm) -> std::span<const std::byte> {
        auto sh = img.find_section(nm);
        if (!sh || sh->sh_size == 0) return {};
        return { so_image.data() + sh->sh_offset, sh->sh_size };
    };
    auto dynsym = flash_sec(".dynsym");
    auto dynstr = flash_sec(".dynstr");
    auto hashsp = flash_sec(".hash");

    auto linkr = fw::link_context::from_self();
    if (!linkr) { fw::image_arena().free(ram); return nullptr; }
    main_resolver mr{ &*linkr };
    auto resolve = mr.source();

    std::span<std::byte> ram_data{ ram, seg.data_memsz };
    for (const char* s : { ".rel.dyn", ".rel.plt" }) {
        auto rel = flash_sec(s);
        if (rel.empty()) continue;
        if (!xip::apply_rel_xip(seg, text_base, data_base, ram_data, rel, dynsym, dynstr, resolve)) {
            fw::image_arena().free(ram);
            return nullptr;
        }
    }

    if (auto ex = img.find_section(".ARM.exidx"); ex && ex->sh_size) {
        const auto ex_start = text_base + (ex->sh_addr - seg.text_vaddr);
        dl_exidx_register(ex_start, ex_start + ex->sh_size, text_base, 0);
    }

    h->in_use     = true;
    h->xip        = true;
    h->text_base  = text_base;
    h->text_vaddr = seg.text_vaddr;
    h->text_size  = seg.text_size;
    h->data_base  = data_base;
    h->got_base   = got_base;
    h->data_vaddr = seg.data_vaddr;
    h->data_memsz = seg.data_memsz;
    h->resolver   = loader::symbol_resolver(dynsym, dynstr, hashsp);
    h->flash_data_base = blob + seg.data_off;
    h->reldyn     = flash_sec(".rel.dyn");
    h->relplt     = flash_sec(".rel.plt");
    h->dynsym     = dynsym;
    h->dynstr     = dynstr;
    h->ram        = ram;
    std::strncpy(h->name, name, sizeof(h->name) - 1);

    if (auto sh = img.find_section(".init_array"); sh && sh->sh_size >= 4) {
        const std::size_t off = sh->sh_addr - seg.data_vaddr;
        for (std::size_t o = 0; o + 4 <= sh->sh_size; o += 4) {
            std::uint32_t fn = 0;
            std::memcpy(&fn, ram + off + o, 4);
            if (fn && fn != 0xffffffffu)
                pic_call_void(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(got_base));
        }
    }

    return h;
}

void* dlsym(void* handle, const char* name) noexcept {
    return detail::dlsym_impl<xip_handle>(handle, name);
}

std::uint32_t get_got_base(void* handle) noexcept {
    return detail::get_got_base_impl<xip_handle>(handle);
}

bool is_xip(void* handle) noexcept {
    return detail::is_xip_impl<xip_handle>(handle);
}

bool get_xip_map(void* handle, xip_map& out) noexcept {
    return detail::get_xip_map_impl<xip_handle>(handle, out);
}

void dlclose(void* handle) noexcept {
    auto* h = static_cast<xip_handle*>(handle);
    if (!h || !h->in_use) return;
    if (!h->xip) ::dl_exidx_unregister(static_cast<std::uint32_t>(h->data_base));
    if (h->ram) fw::image_arena().free(h->ram);
    *h = xip_handle{};
}

// ---- cross-call resolution ----
extern "C" std::uint64_t xip_cross_lookup(std::uint32_t got_slot_addr) noexcept {
    using loader::detail::load_word;
    using loader::detail::name_at;
    using loader::detail::for_each_rel;

    xip_handle* caller = nullptr;
    for (auto& hh : g_handles) {
        if (!hh.in_use) continue;
        if (hh.xip) {
            if (got_slot_addr >= hh.flash_data_base &&
                got_slot_addr <  hh.flash_data_base + hh.data_memsz) { caller = &hh; break; }
        } else {
            if (got_slot_addr >= hh.data_base &&
                got_slot_addr <  hh.data_base + hh.data_memsz) { caller = &hh; break; }
        }
    }
    if (!caller) return 0;

    const std::uint32_t want = caller->data_vaddr +
        (got_slot_addr - (caller->xip ? caller->flash_data_base : caller->data_base));

    std::string_view name{};
    for (const auto* tbl : { &caller->reldyn, &caller->relplt }) {
        for_each_rel(*tbl, [&](std::uint32_t r_off, std::uint32_t r_info) {
            if (r_off != want) return;
            if (loader::elf::r_type(r_info) !=
                static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) return;
            const std::uint32_t s = loader::elf::r_sym(r_info);
            name = name_at(caller->dynstr, load_word(caller->dynsym, s * sizeof(loader::elf::Sym) + 0));
        });
        if (!name.empty()) break;
    }
    if (name.empty()) return 0;

    std::uint32_t target = 0, got = 0;
    for (auto& lib : g_handles) {
        if (!lib.in_use) continue;
        auto v = lib.resolver.address_of(name);
        if (v.has_value() && *v != 0u) {
            const std::uint32_t st = *v;
            const bool in_data = st >= lib.data_vaddr && st < lib.data_vaddr + lib.data_memsz;
            const bool in_text = st >= lib.text_vaddr && st < lib.text_vaddr + lib.text_size && !in_data;
            target = in_text ? lib.text_base + (st - lib.text_vaddr)
                             : lib.data_base + (st - lib.data_vaddr);
            got = static_cast<std::uint32_t>(lib.got_base);
            break;
        }
    }
    if (target == 0) {
        auto linkr = fw::link_context::from_self();
        if (linkr)
            if (auto v = linkr->resolve(name)) { target = *v; got = 0; }
    }
    if (target == 0) return 0;
    return (static_cast<std::uint64_t>(got) << 32) | static_cast<std::uint64_t>(target | 1u);
}

int patch_main_imports(std::span<std::byte> image) noexcept {
    return detail::patch_main_imports_impl(image,
        reinterpret_cast<std::uintptr_t>(&xip_cross_call));
}

}  // namespace xip::nolib

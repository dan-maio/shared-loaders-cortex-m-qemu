/**
 * @file nopic.cpp
 * @brief Fixed-address XIP/static adapter — see nopic.hpp.
 *
 * The module is linked `-fno-pic -mlong-calls` at fixed VAs (the same VAs as
 * the `_sxip_text_*` / `_sxip_data_*` windows the firmware reserves), so load
 * is a 0-bias copy. R_ARM_RELATIVE words are already absolute (left alone);
 * every R_ARM_ABS32 slot is `value = S + A` and must be applied at load —
 * defined-in-module -> st_value, undefined (an import from another loaded
 * module or from main) -> resolve_any() — written into the RAM target.
 */
#include "xip/static/nopic.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"
#include "scl/link-context.hpp"
#include "scl/log.h"
#include <cstring>
#include <string_view>

extern "C" {
/// Fixed-address windows reserved by the application's linker script. Their
/// ADDRESS is the value (same idiom as _simage_arena_start/_end).
extern std::uint8_t _sxip_text_start[];
extern std::uint8_t _sxip_text_end[];
extern std::uint8_t _sxip_data_start[];
extern std::uint8_t _sxip_data_end[];

/// main's dynamic relocation + symbol tables (address == value), exported by the
/// application linker script; read by patch_main() to fill main's GOT/PLT.
extern std::uint8_t __rel_dyn_start[];
extern std::uint8_t __rel_dyn_end[];
extern std::uint8_t __dynsym_start[];
extern std::uint8_t __dynsym_end[];
extern std::uint8_t __dynstr_start[];
extern std::uint8_t __dynstr_end[];
}

namespace xip::nopic {
namespace {

constexpr loader::elf::word PF_X = 0x1;
constexpr loader::elf::word PF_W = 0x2;

constexpr int kMaxModules = 16;
PModule* g_modules[kMaxModules] = {};

/// A section's bytes as they sit in the blob.
std::span<const std::byte> sec(const loader::elf_image& img,
                               std::span<const std::byte> blob,
                               const char* nm) noexcept {
    auto sh = img.find_section(nm);
    if (!sh || sh->sh_size == 0) return {};
    if (sh->sh_offset + sh->sh_size > blob.size()) return {};
    return { blob.data() + sh->sh_offset, sh->sh_size };
}

}  // namespace

void* resolve_any(const char* name) noexcept {
    for (auto* m : g_modules) {
        if (!m || !m->ok) continue;
        auto v = m->resolver.address_of(name);
        if (v && *v != 0) return reinterpret_cast<void*>(*v);
    }
    auto linkr = fw::link_context::from_self();
    if (linkr)
        if (auto v = linkr->resolve(name))
            if (*v != 0) return reinterpret_cast<void*>(*v);
    return nullptr;
}

bool load(std::span<const std::byte> blob, PModule& out) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::for_each_rel;
    using loader::detail::name_at;

    loader::memory_byte_source src(blob);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) { scl_log_puts("[nopic] ELF parse failed\n"); return false; }
    auto img = *parsed;

    const std::uintptr_t text_win = reinterpret_cast<std::uintptr_t>(_sxip_text_start);
    const std::uintptr_t data_win = reinterpret_cast<std::uintptr_t>(_sxip_data_start);
    const std::uintptr_t text_win_sz = reinterpret_cast<std::uintptr_t>(_sxip_text_end) - text_win;
    const std::uintptr_t data_win_sz = reinterpret_cast<std::uintptr_t>(_sxip_data_end) - data_win;

    // Find the two PT_LOAD segments (R-X and RW) the -fno-pic image is linked with.
    std::uintptr_t text_vaddr = 0, text_off = 0, text_filesz = 0, text_memsz = 0;
    std::uintptr_t data_vaddr = 0, data_off = 0, data_filesz = 0, data_memsz = 0;
    bool have_text = false, have_data = false;
    for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (!img.program_header(i, ph)) return false;
        if (ph.p_type != loader::elf::PT_LOAD) continue;
        const bool exec  = (ph.p_flags & PF_X) != 0;
        const bool write = (ph.p_flags & PF_W) != 0;
        if (exec && !write && !have_text) {
            text_vaddr = ph.p_vaddr; text_off = ph.p_offset;
            text_filesz = ph.p_filesz; text_memsz = ph.p_memsz;
            have_text = true;
        } else if (write && !have_data) {
            data_vaddr = ph.p_vaddr; data_off = ph.p_offset;
            data_filesz = ph.p_filesz; data_memsz = ph.p_memsz;
            have_data = true;
        }
    }
    if (!have_text || !have_data) {
        scl_log_puts("[nopic] need a split R-X | RW image (two PT_LOAD)\n");
        return false;
    }

    // Fixed-address contract: each module is linked at its OWN fixed VA inside
    // the reserved window range; the loader copies it there with a 0 bias. This
    // lets several modules be resident at once (cross-module calls), each in its
    // own sub-slot of the window (see the fixture build script).
    if (text_vaddr < text_win || text_vaddr + text_memsz > text_win + text_win_sz) {
        scl_log_puts("[nopic] R-X VA outside the text window: text=");
        scl_log_put_hex("0x", static_cast<uint32_t>(text_vaddr));
        scl_log_puts("\n");
        return false;
    }
    if (data_vaddr < data_win || data_vaddr + data_memsz > data_win + data_win_sz) {
        scl_log_puts("[nopic] RW VA outside the data window: data=");
        scl_log_put_hex("0x", static_cast<uint32_t>(data_vaddr));
        scl_log_puts("\n");
        return false;
    }

    // Copy to the module's own link-time VAs (0 bias), zero the RW bss.
    auto* text = reinterpret_cast<std::byte*>(text_vaddr);
    auto* data = reinterpret_cast<std::byte*>(data_vaddr);
    std::memcpy(text, blob.data() + text_off, text_filesz);
    std::memcpy(data, blob.data() + data_off, data_filesz);
    std::memset(data + data_filesz, 0, data_memsz - data_filesz);

    // Relocations. In this 0-bias model:
    //   R_ARM_RELATIVE  — the linker already wrote the absolute addend into the
    //                     word (we load at the link VAs, so B(S)=0): leave it.
    //   R_ARM_ABS32     — value = S + A where A is the word's addend (0 for a
    //                     literal-pool slot). The linker does NOT pre-fill these
    //                     even for defined symbols, so EVERY one must be applied:
    //                     defined-in-module -> st_value (absolute), undefined ->
    //                     resolve() against loaded modules then main.
    auto dynsym = sec(img, blob, ".dynsym");
    auto dynstr = sec(img, blob, ".dynstr");
    auto reldyn = sec(img, blob, ".rel.dyn");
    auto relplt = sec(img, blob, ".rel.plt");
    constexpr std::size_t ES = sizeof(loader::elf::Sym);
    int patched = 0;
    for (const auto* rel : { &reldyn, &relplt }) {
        for_each_rel(*rel, [&](std::uint32_t r_off, std::uint32_t r_info) {
            const auto rtype = loader::elf::r_type(r_info);
            if (rtype == static_cast<std::uint8_t>(loader::elf::rel_type::RELATIVE)) return;
            const std::uint32_t sym = loader::elf::r_sym(r_info);
            if (sym == 0) return;
            if (sym * ES + ES > dynsym.size()) return;

            const std::uint32_t st_val = load_word(dynsym, sym * ES + 4);
            // bytes 12..15 are st_info, st_other, st_shndx (LE) — the section
            // index occupies the high half of the word.
            const std::uint16_t shndx = static_cast<std::uint16_t>(load_word(dynsym, sym * ES + 12) >> 16);
            const std::string_view name = name_at(dynstr, load_word(dynsym, sym * ES + 0));

            std::uint32_t value = 0;
            if (shndx == 0) {                        // undefined: an import
                if (name.empty()) return;
                void* resolved = resolve_any(name.data());
                if (!resolved) {
                    scl_log_puts("[nopic] unresolved import '");
                    scl_log_puts(name.data());
                    scl_log_puts("'\n");
                    return;
                }
                value = reinterpret_cast<std::uint32_t>(resolved);
            } else {
                value = st_val;                      // defined in module (0 bias)
            }

            std::byte* target = nullptr;
            if (r_off >= data_vaddr && r_off < data_vaddr + data_memsz)
                target = data + (r_off - data_vaddr);
            else if (r_off >= text_vaddr && r_off < text_vaddr + text_memsz)
                target = text + (r_off - text_vaddr);
            if (!target) {
                scl_log_puts("[nopic] reloc target outside windows\n");
                return;
            }
            const std::uint32_t addend =
                load_word(std::span<const std::byte>{ target, 4 }, 0);
            store_word(std::span<std::byte>{ target, 4 }, 0, value + addend);
            ++patched;
        });
    }

    out.ok         = true;
    out.text_base  = text_vaddr;
    out.data_base  = data_vaddr;
    out.text_vaddr = static_cast<std::uint32_t>(text_vaddr);
    out.text_size  = static_cast<std::uint32_t>(text_memsz);
    out.data_vaddr = static_cast<std::uint32_t>(data_vaddr);
    out.data_memsz = static_cast<std::uint32_t>(data_memsz);
    out.reldyn     = reldyn;
    out.dynsym     = dynsym;
    out.dynstr     = dynstr;
    out.resolver   = loader::symbol_resolver(dynsym, dynstr, sec(img, blob, ".hash"));

    // Register BEFORE constructors so a ctor can call another loaded module.
    for (auto& slot : g_modules)
        if (!slot) { slot = &out; break; }

    // Constructors: .init_array lives in the data window; each entry is an
    // absolute function address (Thumb bit set) already copied into RAM.
    if (auto sh = img.find_section(".init_array");
        sh && sh->sh_size >= 4 &&
        sh->sh_addr >= data_vaddr && sh->sh_addr + sh->sh_size <= data_vaddr + data_memsz) {
        const std::size_t off = sh->sh_addr - data_vaddr;
        for (std::size_t o = 0; o + 4 <= sh->sh_size; o += 4) {
            std::uint32_t fn = 0;
            std::memcpy(&fn, data + off + o, 4);
            if (fn && fn != 0xFFFFFFFFu)
                reinterpret_cast<void (*)()>(fn)();
        }
    }

    scl_log_puts("[nopic] loaded text=");
    scl_log_put_hex("0x", static_cast<uint32_t>(text_vaddr));
    scl_log_puts(" data=");
    scl_log_put_hex("0x", static_cast<uint32_t>(data_vaddr));
    scl_log_put_hex(" imports=0x", static_cast<uint32_t>(patched));
    scl_log_puts("\n");
    return true;
}

void* resolve(const PModule& m, const char* name) noexcept {
    if (!m.ok) return nullptr;
    auto v = m.resolver.address_of(name);
    if (!v || *v == 0) return nullptr;
    return reinterpret_cast<void*>(*v);
}

void resolve_all() noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::for_each_rel;
    using loader::detail::name_at;
    constexpr std::size_t ES = sizeof(loader::elf::Sym);

    for (auto* m : g_modules) {
        if (!m || !m->ok || m->reldyn.empty()) continue;
        const auto& dynsym = m->dynsym;
        const auto& dynstr = m->dynstr;
        auto* text = reinterpret_cast<std::byte*>(m->text_vaddr);
        auto* data = reinterpret_cast<std::byte*>(m->data_vaddr);
        const std::uint32_t text_vaddr = m->text_vaddr, text_memsz = m->text_size;
        const std::uint32_t data_vaddr = m->data_vaddr, data_memsz = m->data_memsz;

        for_each_rel(m->reldyn, [&](std::uint32_t r_off, std::uint32_t r_info) {
            if (loader::elf::r_type(r_info) !=
                static_cast<std::uint8_t>(loader::elf::rel_type::ABS32)) return;
            const std::uint32_t sym = loader::elf::r_sym(r_info);
            if (sym == 0 || sym * ES + ES > dynsym.size()) return;
            // undefined (import)? and still zero (not yet resolved)?
            if ((load_word(dynsym, sym * ES + 12) >> 16) != 0) return;
            const std::string_view name = name_at(dynstr, load_word(dynsym, sym * ES + 0));
            if (name.empty()) return;

            std::byte* target = nullptr;
            if (r_off >= data_vaddr && r_off < data_vaddr + data_memsz)
                target = data + (r_off - data_vaddr);
            else if (r_off >= text_vaddr && r_off < text_vaddr + text_memsz)
                target = text + (r_off - text_vaddr);
            if (!target) return;

            if (load_word(std::span<const std::byte>{ target, 4 }, 0) != 0) return;
            void* resolved = resolve_any(name.data());
            if (resolved)
                store_word(std::span<std::byte>{ target, 4 }, 0,
                           reinterpret_cast<std::uint32_t>(resolved));
        });
    }
}

void patch_main(const PModule& m) noexcept {
    if (!m.ok) return;

    struct Rel { std::uint32_t r_offset; std::uint32_t r_info; };
    struct Sym { std::uint32_t st_name; std::uint32_t st_value; std::uint32_t st_size;
                 std::uint8_t st_info; std::uint8_t st_other; std::uint16_t st_shndx; };

    // main's dynamic relocation + symbol tables, exposed by the app linker
    // script. (The app links as EXEC — no usable _DYNAMIC — so the loader reads
    // these tables directly from flash.)
    const auto* rels = reinterpret_cast<const Rel*>(reinterpret_cast<uintptr_t>(__rel_dyn_start));
    const std::size_t rel_count =
        (reinterpret_cast<uintptr_t>(__rel_dyn_end) - reinterpret_cast<uintptr_t>(__rel_dyn_start))
        / sizeof(Rel);
    const auto* syms = reinterpret_cast<const Sym*>(reinterpret_cast<uintptr_t>(__dynsym_start));
    const std::size_t sym_count =
        (reinterpret_cast<uintptr_t>(__dynsym_end) - reinterpret_cast<uintptr_t>(__dynsym_start))
        / sizeof(Sym);
    const char* strtab = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(__dynstr_start));
    const std::size_t strtab_sz =
        reinterpret_cast<uintptr_t>(__dynstr_end) - reinterpret_cast<uintptr_t>(__dynstr_start);

    // For every GLOB_DAT / JUMP_SLOT slot in main whose symbol THIS module
    // exports, write the module's fixed address into main's GOT/PLT.
    for (std::size_t i = 0; i < rel_count; ++i) {
        const std::uint32_t type = rels[i].r_info & 0xFFu;
        const std::uint32_t sym = rels[i].r_info >> 8;
        if (sym == 0 || (type != 21 && type != 22)) continue;   // GLOB_DAT, JUMP_SLOT
        if (sym >= sym_count) continue;
        const std::uint32_t st_name = syms[sym].st_name;
        if (st_name >= strtab_sz) continue;
        auto v = m.resolver.address_of(strtab + st_name);
        if (v && *v != 0)
            *reinterpret_cast<std::uint32_t*>(rels[i].r_offset) = *v;
    }
}

void unload(PModule& m) noexcept {
    for (auto& slot : g_modules)
        if (slot == &m) slot = nullptr;
    m = PModule{};
}

}  // namespace xip::nopic

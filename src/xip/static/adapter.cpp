/**
 * @file adapter.cpp
 * @brief XIP adapter for the standalone static ELF32 loader. See the header.
 *
 * Built on the shared XIP primitives (xip::classify_segments / text_is_immutable
 * / apply_rel_xip from scl::xip-common) and the common-elf-reader parser.
 * Imports from the application are resolved against main's own dynamic symbol
 * table (fw::link_context::from_self()) — no hand-maintained host table. No
 * dynamic loader is involved.
 */
#include "xip/static/adapter.hpp"
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"
#include "scl/image-memory.hpp"
#include "scl/pic-trampoline.h"
#include "scl/link-context.hpp"
#include "scl/log.h"
#include <cstring>
#include <string_view>

extern "C" {
struct Dyn {
    int32_t d_tag;
    uint32_t d_val;
};
extern struct Dyn _DYNAMIC[];
}

/// The shared cross-library call trampoline (r10 swap; entered from a flash PLT
/// stub whose GOT slot was pre-linked to it by bind_host_imports()).
extern "C" void xip_cross_call(void);

namespace xip::stat {
namespace {

constexpr loader::elf::word SHF_WRITE    = 0x1;
constexpr loader::elf::word SHT_PROGBITS = 1;

/// Registry of loaded modules, for resolving cross-library imports at call time.
/// Holds pointers to the caller-owned SModule objects; load() registers, unload()
/// drops. Bounded (few modules, one test battery at a time).
constexpr int kMaxModules = 8;
SModule* g_modules[kMaxModules] = {};

struct Elf32_Rel {
    std::uint32_t r_offset;
    std::uint32_t r_info;
};
struct Elf32_Sym {
    std::uint32_t st_name;
    std::uint32_t st_value;
    std::uint32_t st_size;
    std::uint8_t  st_info;
    std::uint8_t  st_other;
    std::uint16_t st_shndx;
};

/// Resolver over main's own dynamic symbols (for apply_rel_xip).
struct main_resolver {
    fw::link_context* main;
    static loader::result<std::uint32_t> thunk(void* ctx, std::string_view n) noexcept {
        return static_cast<main_resolver*>(ctx)->main->resolve(n);
    }
    loader::symbol_source source() noexcept { return { &thunk, this }; }
};

/// A section's bytes as they sit in the (flash-resident) blob.
std::span<const std::byte> flash_sec(const loader::elf_image& img,
                                     std::span<const std::byte> blob,
                                     const char* nm) noexcept {
    auto sh = img.find_section(nm);
    if (!sh || sh->sh_size == 0) return {};
    return { blob.data() + sh->sh_offset, sh->sh_size };
}

void patch_main(const SModule& m) noexcept {
    std::uintptr_t rel_addr = 0;
    std::size_t rel_sz = 0;
    std::uintptr_t jmprel_addr = 0;
    std::size_t jmprel_sz = 0;
    std::uintptr_t symtab_addr = 0;
    std::uintptr_t strtab_addr = 0;

    for (int i = 0; _DYNAMIC[i].d_tag != 0; ++i) {
        switch (_DYNAMIC[i].d_tag) {
            case 6:  symtab_addr = _DYNAMIC[i].d_val; break; // DT_SYMTAB = 6
            case 5:  strtab_addr = _DYNAMIC[i].d_val; break; // DT_STRTAB = 5
            case 17: rel_addr    = _DYNAMIC[i].d_val; break; // DT_REL = 17
            case 18: rel_sz      = _DYNAMIC[i].d_val; break; // DT_RELSZ = 18
            case 23: jmprel_addr = _DYNAMIC[i].d_val; break; // DT_JMPREL = 23
            case 2:  jmprel_sz   = _DYNAMIC[i].d_val; break; // DT_PLTRELSZ = 2
        }
    }

    auto patch_relocs = [&](std::uintptr_t addr, std::size_t sz) {
        if (!addr || !sz) return;
        const std::size_t count = sz / sizeof(Elf32_Rel);
        const auto* rels = reinterpret_cast<const Elf32_Rel*>(addr);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t r_info = rels[i].r_info;
            std::uint32_t r_offset = rels[i].r_offset;
            std::uint32_t sym_idx = r_info >> 8;
            std::uint32_t rel_type = r_info & 0xff;

            if (sym_idx != 0 && (rel_type == 20 || rel_type == 21 || rel_type == 22)) { // COPY=20, GLOB_DAT=21, JUMP_SLOT=22
                const auto* syms = reinterpret_cast<const Elf32_Sym*>(symtab_addr);
                const char* strtab = reinterpret_cast<const char*>(strtab_addr);
                const char* name = strtab + syms[sym_idx].st_name;

                void* resolved = xip::stat::resolve(m, name);
                if (resolved) {
                    *reinterpret_cast<std::uint32_t*>(r_offset) = reinterpret_cast<std::uint32_t>(resolved);
                }
            }
        }
    };

    patch_relocs(rel_addr, rel_sz);
    patch_relocs(jmprel_addr, jmprel_sz);
}

}  // namespace

int bind_host_imports(std::span<std::byte> image) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::name_at;

    loader::memory_byte_source src(image);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) return -1;
    auto img = *parsed;

    auto linkr = fw::link_context::from_self();
    if (!linkr) return -1;

    std::span<const std::byte> cimg{ image.data(), image.size() };
    auto dynsym = flash_sec(img, cimg, ".dynsym");
    auto dynstr = flash_sec(img, cimg, ".dynstr");
    if (dynsym.empty()) return 0;

    constexpr std::size_t ES = sizeof(loader::elf::Sym);
    int n = 0;

    // Only external FUNCTION imports are bound here. A defined symbol
    // (st_value != 0) resolves inside the module and must not be redirected at
    // the host even if main happens to export the same name; GLOB_DAT data
    // imports are handled at load time against the RAM GOT, which is where the
    // module's own code reads them from via r10.
    for (const char* s : { ".rel.plt", ".rel.dyn" }) {
        auto rel = flash_sec(img, cimg, s);
        for (std::size_t o = 0; o + sizeof(loader::elf::Rel) <= rel.size();
             o += sizeof(loader::elf::Rel)) {
            const std::uint32_t r_off  = load_word(rel, o + 0);
            const std::uint32_t r_info = load_word(rel, o + 4);
            if (loader::elf::r_type(r_info) !=
                static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) continue;
            const std::uint32_t sym    = loader::elf::r_sym(r_info);
            const std::uint32_t st_val = load_word(dynsym, sym * ES + 4);
            if (st_val != 0) continue;                      // only external functions
            const std::uint32_t st_nam = load_word(dynsym, sym * ES + 0);
            std::string_view name = name_at(dynstr, st_nam);

            if (r_off + 4 > image.size()) continue;         // VA == file_offset
            auto a = linkr->resolve(name);                  // main's .dynsym
            if (a && *a != 0) {
                store_word(image, r_off, *a);               // main is r10-free -> direct
            } else {
                // Not in main: a cross-library import (or a libc symbol main does
                // not export). Pre-link the slot to the cross-call trampoline so
                // the load-time check below passes; at call time xip_cross_lookup
                // resolves it against the loaded-module registry, then main.
                store_word(image, r_off,
                           (reinterpret_cast<std::uint32_t>(&xip_cross_call)) | 1u);
            }
            ++n;
        }
    }
    return n;
}

bool load(std::span<const std::byte> blob, SModule& out, bool patch_host_got) noexcept {
    loader::memory_byte_source src(blob);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) { scl_log_puts("  [xip] ELF parse failed\n"); return false; }
    auto img = *parsed;

    SegmentMap seg;
    if (!classify_segments(blob, seg)) { scl_log_puts("  [xip] classify_segments failed\n"); return false; }
    if (!seg.split) { scl_log_puts("  [xip] segments not split\n"); return false; }
    if (!text_is_immutable(blob, seg)) {
        scl_log_puts("  [xip] text not immutable (text=");
        scl_log_put_hex("0x", seg.text_vaddr);
        scl_log_put_hex("+0x", seg.text_size);
        scl_log_put_hex(" data=0x", seg.data_vaddr);
        scl_log_put_hex("+0x", seg.data_memsz);
        scl_log_puts(")\n");
        return false;
    }

    // A PLT stub reaches its GOT slot PC-relatively (`add ip, pc`). Executing from
    // flash, that lands on the *flash* image of the GOT, never on the RAM copy the
    // relocations below populate. Those slots therefore have to be bound before the
    // blob is programmed — see bind_host_imports(), called from the provisioning
    // path. Here we can only verify it happened: an unbound slot still holds the
    // linker's lazy-binding prefill pointing back into .plt, and would send
    // `ldr pc, [ip]` into junk.
    if (auto plt = img.find_section(".plt"); plt && plt->sh_size) {
        const std::uint32_t plt_lo = plt->sh_addr;
        const std::uint32_t plt_hi = plt->sh_addr + plt->sh_size;
        auto dsym = flash_sec(img, blob, ".dynsym");
        auto dstr = flash_sec(img, blob, ".dynstr");
        for (const char* s : { ".rel.dyn", ".rel.plt" }) {
            auto rel = flash_sec(img, blob, s);
            for (std::size_t i = 0; i + sizeof(Elf32_Rel) <= rel.size(); i += sizeof(Elf32_Rel)) {
                const std::uint32_t r_off  = loader::detail::load_word(rel, i + 0);
                const std::uint32_t r_info = loader::detail::load_word(rel, i + 4);
                if ((r_info & 0xffu) != 22) continue;              // R_ARM_JUMP_SLOT
                if (r_off + 4 > blob.size()) continue;             // VA == file offset
                const std::uint32_t slot = loader::detail::load_word(blob, r_off) & ~1u;
                if (slot < plt_lo || slot >= plt_hi) continue;     // already bound
                const std::uint32_t st_name =
                    loader::detail::load_word(dsym, (r_info >> 8) * sizeof(loader::elf::Sym));
                scl_log_puts("  [xip] unbound PLT import '");
                scl_log_puts(loader::detail::name_at(dstr, st_name).data());
                scl_log_puts("' - prelink missing\n");
                return false;
            }
        }
    }

    // Tighten the data bounds
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
    if (!memp) return false;
    auto* ram = static_cast<std::byte*>(*memp);
    std::memset(ram, 0, seg.data_memsz);
    if (seg.data_filesz)
        std::memcpy(ram, blob.data() + seg.data_off, seg.data_filesz);

    const std::uintptr_t base      = reinterpret_cast<std::uintptr_t>(blob.data());
    const std::uintptr_t text_base = base + seg.text_off;                   // flash
    const std::uintptr_t data_base = reinterpret_cast<std::uintptr_t>(ram); // RAM
    std::uintptr_t got_base = data_base;
    if (auto g = img.find_section(".got"); g && g->sh_addr >= seg.data_vaddr)
        got_base = data_base + (g->sh_addr - seg.data_vaddr);

    auto dynsym = flash_sec(img, blob, ".dynsym");
    auto dynstr = flash_sec(img, blob, ".dynstr");
    auto hashsp = flash_sec(img, blob, ".hash");

    auto linkr = fw::link_context::from_self();
    if (!linkr) { fw::image_arena().free(ram); return false; }
    main_resolver mr{ &*linkr };
    auto resolve_src = mr.source();
    std::span<std::byte> ram_data{ ram, seg.data_memsz };
    for (const char* s : { ".rel.dyn", ".rel.plt" }) {
        auto rel = flash_sec(img, blob, s);
        if (rel.empty()) continue;
        if (!xip::apply_rel_xip(seg, text_base, data_base, ram_data, rel, dynsym, dynstr, resolve_src)) {
            fw::image_arena().free(ram);
            return false;
        }
    }

    out.ok         = true;
    out.text_base  = text_base;
    out.data_base  = data_base;
    out.got_base   = got_base;
    out.text_vaddr = seg.text_vaddr;
    out.text_size  = seg.text_size;
    out.data_vaddr = seg.data_vaddr;
    out.data_memsz = seg.data_memsz;
    out.flash_data_base = base + seg.data_off;
    out.reldyn     = flash_sec(img, blob, ".rel.dyn");
    out.relplt     = flash_sec(img, blob, ".rel.plt");
    out.dynsym     = dynsym;
    out.dynstr     = dynstr;
    out.resolver   = loader::symbol_resolver(dynsym, dynstr, hashsp);
    out.ram        = ram;

    // Register so cross-library calls made by other loaded modules can reach
    // this module's exports through the trampoline.
    for (auto& slot : g_modules)
        if (!slot) { slot = &out; break; }

    // Constructors run from flash with r10 = got_base.
    if (auto sh = img.find_section(".init_array"); sh && sh->sh_size >= 4) {
        const std::size_t off = sh->sh_addr - seg.data_vaddr;
        for (std::size_t o = 0; o + 4 <= sh->sh_size; o += 4) {
            std::uint32_t fn = 0;
            std::memcpy(&fn, ram + off + o, 4);
            if (fn && fn != 0xffffffffu)
                pic_call_void(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(got_base));
        }
    }

    // "Direct" callers additionally point main's own GOT/PLT slots at this
    // module's exports, so the app can call in by name with no trampoline.
    if (patch_host_got) patch_main(out);

    return true;
}

void* resolve(const SModule& m, const char* name) noexcept {
    if (!m.ok) return nullptr;
    auto v = m.resolver.address_of(name);
    if (!v || *v == 0) return nullptr;
    const std::uint32_t st = *v;
    const bool in_d = st >= m.data_vaddr && st < m.data_vaddr + m.data_memsz;
    const bool in_t = st >= m.text_vaddr && st < m.text_vaddr + m.text_size && !in_d;
    return reinterpret_cast<void*>(in_t ? m.text_base + (st - m.text_vaddr)
                                        : m.data_base + (st - m.data_vaddr));
}

void unload(SModule& m) noexcept {
    for (auto& slot : g_modules)
        if (slot == &m) slot = nullptr;
    if (m.ram) fw::image_arena().free(m.ram);
    m = SModule{};
}

// ---- cross-library call resolution ----
//
// Entered from xip_cross_call with r12 = &flash_GOT_slot (the PLT stub loads
// ip = &GOT[n], then `ldr pc, [GOT[n]]` jumps to the trampoline). Identify the
// caller by which loaded module's flash RW image contains that slot, map the
// slot back to its r_offset, recover the symbol name, then find the definer
// among the loaded modules (falling back to main). Returns {callee_got, target}
// packed in a u64; the trampoline swaps r10 and blx's the target.
extern "C" std::uint64_t xip_cross_lookup(std::uint32_t got_slot_addr) noexcept {
    using loader::detail::load_word;
    using loader::detail::name_at;
    using loader::detail::for_each_rel;

    SModule* caller = nullptr;
    for (auto* m : g_modules) {
        if (!m || !m->ok) continue;
        if (got_slot_addr >= m->flash_data_base &&
            got_slot_addr <  m->flash_data_base + m->data_memsz) { caller = m; break; }
    }
    if (!caller) return 0;

    const std::uint32_t want = caller->data_vaddr +
        (got_slot_addr - static_cast<std::uint32_t>(caller->flash_data_base));

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
    for (auto* m : g_modules) {
        if (!m || !m->ok) continue;
        auto v = m->resolver.address_of(name);
        if (v.has_value() && *v != 0u) {
            const std::uint32_t st = *v;
            const bool in_d = st >= m->data_vaddr && st < m->data_vaddr + m->data_memsz;
            const bool in_t = st >= m->text_vaddr && st < m->text_vaddr + m->text_size && !in_d;
            target = in_t ? static_cast<std::uint32_t>(m->text_base + (st - m->text_vaddr))
                          : static_cast<std::uint32_t>(m->data_base + (st - m->data_vaddr));
            got = static_cast<std::uint32_t>(m->got_base);
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

}  // namespace xip::stat

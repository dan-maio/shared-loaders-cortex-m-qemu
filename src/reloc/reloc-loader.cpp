/**
 * @file reloc-loader.cpp
 * @brief scl::reloc — ET_REL (relocatable) module loader implementation.
 * @see reloc/reloc-loader.hpp
 */
#include "reloc/reloc-loader.hpp"
#include "loader/elf-types.hpp"
#include "loader/detail/bytes.hpp"     // load_word / name_at

#include <cstring>

namespace reloc {
namespace {

using loader::error;
using loader::detail::load_word;
using loader::detail::name_at;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using loader::elf::rel_type;

// ---- ELF constants we need (ET_REL side; the rest live in elf-types.hpp) ----
constexpr uint16_t ET_REL   = 1;
constexpr uint16_t EM_ARM   = 40;
constexpr uint32_t SHT_PROGBITS   = 1;
constexpr uint32_t SHT_SYMTAB     = 2;
constexpr uint32_t SHT_NOBITS     = 8;
constexpr uint32_t SHT_REL        = 9;
constexpr uint32_t SHT_INIT_ARRAY = 14;
constexpr uint32_t SHF_ALLOC      = 2;
constexpr uint16_t SHN_UNDEF = 0;
constexpr uint16_t SHN_ABS   = 0xfff1;
constexpr uint8_t  STT_FUNC  = 2;

// R_ARM relocation types this loader implements.
constexpr uint8_t R_ARM_THM_CALL = 10;
constexpr uint8_t R_ARM_TARGET1  = 38;   // == ABS32 on bare-metal

// Elf32 struct field offsets.
constexpr uint32_t EH_TYPE = 16, EH_MACHINE = 18, EH_SHOFF = 32,
                   EH_SHENTSIZE = 46, EH_SHNUM = 48;
constexpr uint32_t SH_NAME = 0, SH_TYPE = 4, SH_FLAGS = 8, SH_OFFSET = 16,
                   SH_SIZE = 20, SH_LINK = 24, SH_INFO = 28, SH_ADDRALIGN = 32,
                   SH_ENTSIZE = 36;
constexpr uint32_t ST_NAME = 0, ST_VALUE = 4, ST_INFO = 12, ST_SHNDX = 14;
constexpr uint32_t REL_OFFSET = 0, REL_INFO = 4;
constexpr uint32_t SYMSZ = 16, RELSZ = 8;

inline uint16_t load16(std::span<const std::byte> s, uint32_t off) noexcept {
    if (off + 2u > s.size()) return 0;
    return static_cast<uint16_t>(std::to_integer<uint8_t>(s[off]) |
                                 (std::to_integer<uint8_t>(s[off + 1]) << 8));
}
inline uint8_t load8(std::span<const std::byte> s, uint32_t off) noexcept {
    return off < s.size() ? std::to_integer<uint8_t>(s[off]) : 0;
}
inline uint32_t align_up(uint32_t v, uint32_t a) noexcept {
    return (a <= 1) ? v : (v + a - 1u) & ~(a - 1u);
}

/// Read one Shdr word field for section @p i.
struct sh_reader {
    std::span<const std::byte> img;
    uint32_t shoff; uint16_t shentsize;
    uint32_t f(uint16_t i, uint32_t field) const {
        return load_word(img, shoff + uint32_t(i) * shentsize + field);
    }
};

// ---- symbol resolution ------------------------------------------------------
// Returns the runtime address of symbol @p si, OR-ing the Thumb bit for locally
// defined STT_FUNC symbols (R_ARM_ABS32 semantics: S | T). External (UND)
// symbols come from the resolver, which already returns Thumb-correct addresses.
struct sym_env {
    const module& m;
    std::span<const std::byte> img;
    uint32_t symtab_off, symtab_sz;
    std::span<const std::byte> strtab;
    loader::symbol_source resolve;

    bool ok = true;

    uint32_t addr_of(uint32_t si) {
        const uint32_t so = symtab_off + si * SYMSZ;
        if (so + SYMSZ > symtab_off + symtab_sz) { ok = false; return 0; }
        const uint16_t shndx = load16(img, so + ST_SHNDX);
        const uint32_t value = load_word(img, so + ST_VALUE);
        if (shndx == SHN_UNDEF) {                     // external -> resolver
            auto nm = name_at(strtab, load_word(img, so + ST_NAME));
            auto r = resolve(nm);
            if (!r) { ok = false; return 0; }
            return *r;                                // already Thumb-correct
        }
        if (shndx == SHN_ABS) return value;           // absolute
        if (shndx >= m.shnum) { ok = false; return 0; }
        uint32_t a = m.sec_base[shndx] + value;       // defined in a section
        if ((load8(img, so + ST_INFO) & 0xf) == STT_FUNC) a |= 1u;  // Thumb bit
        return a;
    }
};

// ---- R_ARM_THM_CALL (Thumb BL, T1) encode with in-place addend --------------
void apply_thm_call(std::byte* p, uint32_t S, uint32_t P) noexcept {
    uint16_t hw1 = static_cast<uint16_t>(std::to_integer<uint8_t>(p[0]) |
                                         (std::to_integer<uint8_t>(p[1]) << 8));
    uint16_t hw2 = static_cast<uint16_t>(std::to_integer<uint8_t>(p[2]) |
                                         (std::to_integer<uint8_t>(p[3]) << 8));
    // decode in-place addend A
    uint32_t s   = (hw1 >> 10) & 1;
    uint32_t j1  = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
    uint32_t i1  = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
    uint32_t A   = (s << 24) | (i1 << 23) | (i2 << 22) |
                   ((hw1 & 0x3ffu) << 12) | ((hw2 & 0x7ffu) << 1);
    if (A & 0x01000000u) A |= 0xfe000000u;            // sign-extend bit24
    // value = (S & ~1) + A - P  (BL to a Thumb target; bit0 not part of offset)
    uint32_t off = ((S & ~1u) + A) - P;
    s = (off >> 24) & 1; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1;
    j1 = (~(i1 ^ s)) & 1; j2 = (~(i2 ^ s)) & 1;
    uint16_t o1 = static_cast<uint16_t>(0xf000u | (s << 10) | ((off >> 12) & 0x3ffu));
    uint16_t o2 = static_cast<uint16_t>(0xd000u | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ffu));
    p[0] = std::byte(o1 & 0xff); p[1] = std::byte(o1 >> 8);
    p[2] = std::byte(o2 & 0xff); p[3] = std::byte(o2 >> 8);
}

// ---- phase helpers (shared by load and load_set) ----------------------------

// Place @p image's SHF_ALLOC sections into @p arena starting at *used (packed,
// so load_set can pack several modules); fill @p m and mark it placed. Does NOT
// apply relocations or run constructors.
error place_module(std::span<const std::byte> image, std::span<std::byte> arena,
                   uint32_t& used, module& m) noexcept {
    if (image.size() < 52) return error::NOT_ELF;
    if (!(load8(image, 0) == 0x7f && load8(image, 1) == 'E' &&
          load8(image, 2) == 'L' && load8(image, 3) == 'F')) return error::NOT_ELF;
    if (load16(image, EH_MACHINE) != EM_ARM) return error::NOT_ARM32;
    if (load16(image, EH_TYPE)    != ET_REL) return error::BAD_SECTION;

    m = module{};
    m.image = image; m.arena = arena;
    m.shoff = load_word(image, EH_SHOFF);
    m.shentsize = load16(image, EH_SHENTSIZE);
    m.shnum = load16(image, EH_SHNUM);
    if (m.shnum == 0 || m.shnum > MAX_SECTIONS) return error::BAD_SECTION;
    sh_reader sh{image, m.shoff, m.shentsize};
    const uint32_t abase = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(arena.data()));
    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) == SHT_SYMTAB) m.symtab_idx = i;
        if (!(sh.f(i, SH_FLAGS) & SHF_ALLOC)) continue;
        const uint32_t sz = sh.f(i, SH_SIZE), align = sh.f(i, SH_ADDRALIGN);
        used = align_up(used, align ? align : 4);
        if (used + sz > arena.size()) return error::OUT_OF_MEMORY;
        m.sec_base[i] = abase + used;
        if (sh.f(i, SH_TYPE) == SHT_NOBITS) {
            std::memset(arena.data() + used, 0, sz);              // .bss
        } else {
            const uint32_t off = sh.f(i, SH_OFFSET);
            if (off + sz > image.size()) return error::BAD_SECTION;
            std::memcpy(arena.data() + used, image.data() + off, sz);
        }
        m.used += sz;
        used   += sz;
    }
    if (m.symtab_idx == 0) return error::BAD_SECTION;
    m.valid = true;   // "placed" — sym() works now (needed for cross-module resolve)
    return error::OK;
}

// Apply @p m's relocations; UND symbols resolved through @p und.
error apply_relocs(module& m, loader::symbol_source und) noexcept {
    sh_reader sh{m.image, m.shoff, m.shentsize};
    const uint32_t symtab_off = sh.f(m.symtab_idx, SH_OFFSET);
    const uint32_t symtab_sz  = sh.f(m.symtab_idx, SH_SIZE);
    const uint16_t strtab_idx = static_cast<uint16_t>(sh.f(m.symtab_idx, SH_LINK));
    auto strtab = m.image.subspan(sh.f(strtab_idx, SH_OFFSET), sh.f(strtab_idx, SH_SIZE));
    sym_env env{m, m.image, symtab_off, symtab_sz, strtab, und};

    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) != SHT_REL) continue;
        const uint16_t tgt = static_cast<uint16_t>(sh.f(i, SH_INFO));
        if (tgt >= m.shnum || m.sec_base[tgt] == 0) continue;   // reloc for a non-loaded section
        const uint32_t tbase = m.sec_base[tgt], roff = sh.f(i, SH_OFFSET), rsz = sh.f(i, SH_SIZE);
        for (uint32_t o = 0; o + RELSZ <= rsz; o += RELSZ) {
            const uint32_t r_offset = load_word(m.image, roff + o + REL_OFFSET);
            const uint32_t r_info   = load_word(m.image, roff + o + REL_INFO);
            const uint8_t  type     = static_cast<uint8_t>(r_info & 0xffu);
            const uint32_t P        = tbase + r_offset;
            std::byte*     pp = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(P));
            const uint32_t S  = env.addr_of(r_info >> 8);
            if (!env.ok) return error::SYMBOL_NOT_FOUND;
            switch (type) {
            case static_cast<std::uint8_t>(rel_type::ABS32):
            case R_ARM_TARGET1: {
                uint32_t A = load_word(std::span<const std::byte>(pp, 4), 0), v = S + A;
                pp[0]=std::byte(v); pp[1]=std::byte(v>>8); pp[2]=std::byte(v>>16); pp[3]=std::byte(v>>24);
                break; }
            case static_cast<std::uint8_t>(rel_type::REL32): {
                uint32_t A = load_word(std::span<const std::byte>(pp, 4), 0), v = S + A - P;
                pp[0]=std::byte(v); pp[1]=std::byte(v>>8); pp[2]=std::byte(v>>16); pp[3]=std::byte(v>>24);
                break; }
            case R_ARM_THM_CALL: apply_thm_call(pp, S, P); break;
            default: return error::UNSUPPORTED_RELOC;
            }
        }
    }
    return error::OK;
}

// Run @p m's .init_array constructors (now ABS32-relocated to runtime addresses).
void run_ctors(module& m) noexcept {
    sh_reader sh{m.image, m.shoff, m.shentsize};
    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) != SHT_INIT_ARRAY || m.sec_base[i] == 0) continue;
        const uint32_t base = m.sec_base[i], sz = sh.f(i, SH_SIZE);
        for (uint32_t o = 0; o + 4 <= sz; o += 4) {
            uint32_t fp = *reinterpret_cast<const uint32_t*>(static_cast<std::uintptr_t>(base + o));
            if (fp) reinterpret_cast<void (*)()>(static_cast<std::uintptr_t>(fp | 1u))();
        }
    }
}

}  // namespace

std::uint32_t sym(const module& m, std::string_view name) noexcept;   // fwd (below)

namespace {
// Cross-module resolver for load_set: each placed module's exports first, then
// the external resolver (main).
struct set_ctx { module* mods; int n; loader::symbol_source ext; };
loader::result<std::uint32_t> set_resolve(void* c, std::string_view nm) noexcept {
    auto* s = static_cast<set_ctx*>(c);
    for (int i = 0; i < s->n; ++i)
        if (std::uint32_t a = sym(s->mods[i], nm)) return a;
    return s->ext(nm);
}
}  // namespace

loader::result<module> load(std::span<const std::byte> image, std::span<std::byte> arena,
                            loader::symbol_source resolve) noexcept {
    module m{};
    uint32_t used = 0;
    if (auto e = place_module(image, arena, used, m); e != error::OK) return loader::unexpected(e);
    if (auto e = apply_relocs(m, resolve);            e != error::OK) return loader::unexpected(e);
    run_ctors(m);
    return m;
}

loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte> arena,
                                    loader::symbol_source resolve) noexcept {
    if (images.size() > MAX_SET) return loader::unexpected(error::OUT_OF_MEMORY);
    module_set set{};
    uint32_t used = 0;
    for (auto img : images) {                              // phase 1: place all
        if (auto e = place_module(img, arena, used, set.mods[set.n]); e != error::OK)
            return loader::unexpected(e);
        ++set.n;
    }
    set_ctx sc{ set.mods, set.n, resolve };                // phase 2: resolve all
    loader::symbol_source ss{ set_resolve, &sc };
    for (int i = 0; i < set.n; ++i)
        if (auto e = apply_relocs(set.mods[i], ss); e != error::OK) return loader::unexpected(e);
    for (int i = 0; i < set.n; ++i) run_ctors(set.mods[i]); // phase 3: run all ctors
    return set;
}

std::uint32_t sym(const module& m, std::string_view name) noexcept {
    if (!m.valid || m.symtab_idx == 0) return 0;
    sh_reader sh{m.image, m.shoff, m.shentsize};
    const uint32_t symtab_off = sh.f(m.symtab_idx, SH_OFFSET);
    const uint32_t symtab_sz  = sh.f(m.symtab_idx, SH_SIZE);
    const uint16_t strtab_idx = static_cast<uint16_t>(sh.f(m.symtab_idx, SH_LINK));
    auto strtab = m.image.subspan(sh.f(strtab_idx, SH_OFFSET), sh.f(strtab_idx, SH_SIZE));
    for (uint32_t o = 0; o + SYMSZ <= symtab_sz; o += SYMSZ) {
        const uint32_t so = symtab_off + o;
        const uint16_t shndx = load16(m.image, so + ST_SHNDX);
        if (shndx == SHN_UNDEF || shndx >= m.shnum || m.sec_base[shndx] == 0) continue;
        if (name_at(strtab, load_word(m.image, so + ST_NAME)) != name) continue;
        uint32_t a = m.sec_base[shndx] + load_word(m.image, so + ST_VALUE);
        if ((load8(m.image, so + ST_INFO) & 0xf) == STT_FUNC) a |= 1u;
        return a;
    }
    return 0;
}

std::uint32_t sym(const module_set& s, std::string_view name) noexcept {
    for (int i = 0; i < s.n; ++i)
        if (std::uint32_t a = sym(s.mods[i], name)) return a;
    return 0;
}

}  // namespace reloc

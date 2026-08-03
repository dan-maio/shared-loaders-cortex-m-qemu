/** @file dl.cpp @brief See dl.hpp. */
#include "dynamic-nolib/dl.hpp"
#include "scl/log.h"
#include "dynamic-nolib/lazy-plt.hpp"


#include "scl/image-memory.hpp"
#include "scl/link-context.hpp"
#include "scl/pic-trampoline.h"
#include "scl/unwind-exidx.h"
#include "loader/elf-image.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/lazy-resolve.hpp"
#include "loader/detail/bytes.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

// P6: the cross-library r10 trampoline (firmware/src/dl-cross-call.c). External
// JUMP_SLOTs route here so a lib->lib call runs with the callee's own got_base.
extern "C" void dl_cross_call(void);

namespace fw {
using loader::detail::for_each_rel;
namespace {
std::array<handle, 4> g_handles{};   ///< Fixed pool of loadable-library handles (max 4 loaded).

/// P6 cross-call state. off by default so the stock firmware/main.cpp is unaffected;
/// main-tests.cpp turns it on.
bool g_cross_call  = false;
int  g_cross_trace = 1;               ///< GOT/PLT translation trace level (0=off,1=hops,2=verbose).

/** @brief Trace "label0xXXXXXXXX" (8 hex digits), no libc. */
#define put_hex scl_log_put_hex

/** @brief Trace a small unsigned decimal (for entry counts), no libc. */
void put_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { scl_log_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) scl_log_putc(b[--i]);
}

/** @brief Human-readable name for an ARM REL relocation type (those this loader handles). */
const char* reloc_name(std::uint8_t t) {
    using rt = loader::elf::rel_type;
    switch (static_cast<rt>(t)) {
        case rt::ABS32:    return "ABS32    ";
        case rt::REL32:    return "REL32    ";
        case rt::COPY:     return "COPY     ";
        case rt::GLOB_DAT:  return "GLOB_DAT ";
        case rt::JUMP_SLOT: return "JUMP_SLOT";
        case rt::RELATIVE: return "RELATIVE ";
    }
    return "type?    ";
}

/** @brief First not-in-use handle in @ref g_handles, or nullptr if all four are taken. */
handle* alloc_handle() {
    for (auto& h : g_handles) if (!h.in_use) return &h;
    return nullptr;
}

/**
 * @brief Section-header cache (read once per dlopen to avoid semihosting round-trips).
 *
 * elf_image::find_section() re-reads the whole section header table AND each section
 * name from the file on every call; over semihosting (a halt/resume per read) and ~8
 * lookups per dlopen that is hundreds of round-trips and tens of seconds. We instead
 * read the section header table and .shstrtab ONCE here (two reads) and match names in
 * RAM. File-scope (bare-metal, single-threaded): ~4.6 KB of .bss, no stack pressure,
 * no function-local-static guard.
 */
struct section_cache {
    static constexpr std::uint32_t kMaxSec = 64;   ///< Max section headers cached.
    static constexpr std::uint32_t kStrCap = 2048; ///< Max .shstrtab bytes cached.
    loader::elf::Shdr sh[kMaxSec];     ///< Cached section header table.
    char              names[kStrCap];  ///< Cached section-name string table (.shstrtab).
    std::uint32_t     count = 0;       ///< Number of valid entries in @ref sh.
    std::uint32_t     names_len = 0;   ///< Valid bytes in @ref names.

    /** @brief Read the section header table + .shstrtab of @p img from @p src into the cache. */
    loader::result<void> load(loader::elf_image& img, loader::byte_source& src) noexcept {
        const auto& h = img.header();
        std::uint32_t n = h.e_shnum;
        if (n > kMaxSec) n = kMaxSec;
        count = n;
        if (auto r = src.read_at(h.e_shoff, std::span<std::byte>{
                reinterpret_cast<std::byte*>(sh), n * sizeof(loader::elf::Shdr)}); !r)
            return loader::unexpected(r.err());
        names_len = 0;
        if (h.e_shstrndx < n) {
            const loader::elf::Shdr& st = sh[h.e_shstrndx];
            std::uint32_t sz = st.sh_size > kStrCap ? kStrCap : st.sh_size;
            if (sz) {
                if (auto r = src.read_at(st.sh_offset, std::span<std::byte>{
                        reinterpret_cast<std::byte*>(names), sz}); !r)
                    return loader::unexpected(r.err());
            }
            names_len = sz;
        }
        return {};
    }

    /** @brief Cached section header whose name equals @p name, or nullptr. */
    const loader::elf::Shdr* find(std::string_view name) const noexcept {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t off = sh[i].sh_name;
            if (off >= names_len) continue;
            const std::uint32_t avail = names_len - off;     // bytes left in buffer
            if (name.size() < avail && names[off + name.size()] == '\0' &&
                std::memcmp(names + off, name.data(), name.size()) == 0)
                return &sh[i];
        }
        return nullptr;
    }

    /**
     * @brief First non-empty cached section of type @p type (sh_type), or nullptr.
     *
     * Necessary because a library's linker script may collect compiler-emitted
     * .init_array input into an output section under a different NAME (the C libs here
     * use ".inits", an SHT_INIT_ARRAY=14 section) -- a name lookup would silently skip
     * the constructors while the type is unambiguous.
     */
    const loader::elf::Shdr* find_by_type(std::uint32_t type) const noexcept {
        for (std::uint32_t i = 0; i < count; ++i)
            if (sh[i].sh_type == type && sh[i].sh_size != 0) return &sh[i];
        return nullptr;
    }
};
constexpr std::uint32_t kShtInitArray = 14;   ///< SHT_INIT_ARRAY (constructor pointer array).
constexpr std::uint32_t kShtFiniArray = 15;   ///< SHT_FINI_ARRAY (destructor pointer array).
section_cache g_sc;                            ///< Per-dlopen section-header cache (single-threaded).

/**
 * @brief A named allocated section as an in-image span; sets @p addr_out to its vaddr.
 *
 * The section must be allocated (in PT_LOAD) -- true for .dynsym, .dynstr, .hash,
 * the .rel sections, .got, .init_array, .fini_array. Returns an empty span (and
 * addr_out=0) if the section is missing or zero-length.
 */
std::span<std::byte> sec_in_image(const section_cache& sc, std::span<std::byte> mem,
                                  const char* name, std::uint32_t& addr_out) {
    const auto* sh = sc.find(name);
    if (!sh || sh->sh_size == 0) { addr_out = 0; return {}; }
    addr_out = sh->sh_addr;
    return std::span<std::byte>{mem.data() + sh->sh_addr, sh->sh_size};
}
/**
 * @brief Symbol resolver used during relocation: other loaded libs first, then main.
 *
 * Resolves an external symbol against already-loaded libraries first (a definition in
 * any in-use handle other than the one loading), then falls back to main. Records a
 * dependency edge on the loading handle so dlclose() can refcount correctly.
 */
struct combined_resolver {
    std::array<handle, 4>* handles;   ///< == &g_handles.
    handle*                self;      ///< The handle being loaded (excluded from the search).
    link_context*           main;      ///< Fallback: main's own exported symbols.

    /** @brief loader::symbol_source thunk: resolve @p name (see the struct brief). */
    static loader::result<std::uint32_t> thunk(void* ctx, std::string_view name) noexcept {
        auto* cr = static_cast<combined_resolver*>(ctx);
        for (std::size_t i = 0; i < cr->handles->size(); ++i) {
            handle& lib = (*cr->handles)[i];
            if (!lib.in_use || &lib == cr->self) continue;
            auto v = lib.resolver.address_of(name);
            if (v.has_value() && *v != 0u) {            // a DEFINITION in this loaded lib
                // Record the dependency self -> lib (dedup), so dlclose can decrement.
                bool known = false;
                for (std::uint32_t d = 0; d < cr->self->dep_count; ++d)
                    if (cr->self->deps[d] == i) { known = true; break; }
                if (!known && cr->self->dep_count < 4)
                    cr->self->deps[cr->self->dep_count++] = static_cast<std::uint8_t>(i);
                return lib.base + *v;
            }
        }
        return cr->main->resolve(name);                 // fall back to main
    }

    /** @brief Package this resolver as a loader::symbol_source (thunk + context). */
    loader::symbol_source source() noexcept { return loader::symbol_source{&thunk, this}; }
};

std::uint32_t g_fixup_count = 0;   ///< 5c: count of lazy PLT fixups performed (on-target proof).
} // namespace

loader::result<handle*> dlopen_source(loader::byte_source& src, const char* rawname) noexcept {
    auto imgr = loader::elf_image::parse(src);
    if (!imgr) return loader::unexpected(imgr.err());
    loader::elf_image img = *imgr;

    auto sizer = img.image_size();
    if (!sizer) return loader::unexpected(sizer.err());

    auto memp = image_arena().alloc(*sizer);
    if (!memp) return loader::unexpected(loader::error::OUT_OF_MEMORY);
    auto* membytes = static_cast<std::byte*>(*memp);
    std::span<std::byte> mem{membytes, *sizer};
    std::memset(membytes, 0, *sizer);
    const std::uint32_t base = reinterpret_cast<std::uint32_t>(membytes);

    // Copy PT_LOAD segments to their vaddr offsets.
    for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
        loader::elf::Phdr ph;
        if (!img.program_header(i, ph)) continue;
        if (ph.p_type != loader::elf::PT_LOAD || ph.p_filesz == 0) continue;
        if (!src.read_at(ph.p_offset,
                std::span<std::byte>{membytes + ph.p_vaddr, ph.p_filesz})) {
            image_arena().free(membytes);
            return loader::unexpected(loader::error::SHORT_READ);
        }
    }

    // Read the section header table + .shstrtab once (two semihosting reads) so the
    // per-section lookups below hit RAM instead of re-reading the file each time.
    if (auto r = g_sc.load(img, src); !r) {
        image_arena().free(membytes);
        return loader::unexpected(r.err());
    }

    // Symbol/relocation sections are ALLOCATED -> already in `mem`. Read as in-image
    // spans: no std::vector for these, and they persist until dlclose.
    std::uint32_t a_sym = 0, a_str = 0, a_hash = 0;
    auto dynsym  = sec_in_image(g_sc, mem, ".dynsym", a_sym);
    auto dynstr  = sec_in_image(g_sc, mem, ".dynstr", a_str);
    auto hash_sp = sec_in_image(g_sc, mem, ".hash",  a_hash);

    auto linkr = link_context::from_self();
    if (!linkr) { image_arena().free(membytes); return loader::unexpected(linkr.err()); }

    handle* h = alloc_handle();
    if (!h) { image_arena().free(membytes); return loader::unexpected(loader::error::OUT_OF_MEMORY); }
    h->base = base; h->size = *sizer; h->in_use = true;
    h->dep_count = 0; h->refcount = 0;
    // P6: short provenance name = basename without directory or extension (e.g. "c1").
    { const char* b = rawname; for (const char* p = rawname; *p; ++p) if (*p == '/') b = p + 1;
      std::size_t n = 0; while (b[n] && b[n] != '.' && n + 1 < sizeof(h->name)) { h->name[n] = b[n]; ++n; }
      h->name[n] = '\0'; }
    // Build this lib's own resolver now so it is excluded correctly and so its defined
    // symbols are available; UND symbols (value 0) are handled by the chain below / reloc_applier.
    h->resolver = loader::symbol_resolver(dynsym, dynstr, hash_sp);

    // P6/test-7: snapshot the .got contents NOW -- after PT_LOAD copy, before the relocator
    // (and the lazy/cross trampoline routing) patches the slots in place. This is the "BEFORE"
    // column of the didactic GOT walkthrough; the live image is the "AFTER".
    {
        std::uint32_t g_addr = 0;
        auto got = sec_in_image(g_sc, mem, ".got", g_addr);
        const std::size_t words = got.size() / 4;
        h->got_off   = g_addr;
        h->got_words = static_cast<std::uint16_t>(words <= K_GOT_SNAP_MAX ? words : 0);
        for (std::uint16_t i = 0; i < h->got_words; ++i)
            std::memcpy(&h->got_raw[i], got.data() + i * 4u, 4);
    }

    combined_resolver cr{&g_handles, h, &*linkr};
    auto resolve = cr.source();

    for (const char* s : {".rel.dyn", ".rel.plt"}) {
        std::uint32_t a_rel = 0;
        auto rel = sec_in_image(g_sc, mem, s, a_rel);
        if (rel.empty()) continue;
        auto ar = loader::reloc_applier::apply_rel(mem, base, rel, dynsym, dynstr, resolve);
        if (!ar) { h->in_use = false; image_arena().free(membytes); return loader::unexpected(ar.err()); }
    }

    // Bump refcounts for every library this one ended up importing from.
    for (std::uint32_t d = 0; d < h->dep_count; ++d) g_handles[h->deps[d]].refcount++;

    // 5c: lazy-bind JUMP_SLOTs. The eager pass above already bound them (and recorded
    // deps/refcounts); now point each JUMP_SLOT GOT slot straight at dl_runtime_resolve so the
    // FIRST call routes through it. We bypass the linker's PLT0 (its GOT-relative offset is
    // ambiguous on Cortex-M) and rely only on PLTn, which puts &GOT[n] in ip before
    // `ldr pc,[GOT[n]]` -- so the resolver still learns which slot it is. dl_fixup patches the
    // slot, so subsequent calls jump straight to the target and bypass the resolver.
    {
        std::uint32_t rd_addr = 0;
        auto reldyn = sec_in_image(g_sc, mem, ".rel.dyn", rd_addr);
        if (!reldyn.empty()) {
            using loader::detail::load_word;
            using loader::detail::store_word;
            using loader::detail::for_each_rel;
            const std::uint32_t resolver = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(&dl_runtime_resolve)) | 1u;
            for_each_rel(reldyn, [&](std::uint32_t r_off, std::uint32_t r_info) {
                if (loader::elf::r_type(r_info) ==
                    static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT))
                    store_word(mem, r_off, resolver);
            });
            // Stash the tables dl_fixup needs (spans into the arena image; live until dlclose).
            h->lazy = loader::lazy_tables{reldyn, dynsym, dynstr};
        }
    }

    // P6: also keep this image's .rel.plt JUMP_SLOT table. C++ libs (-nostdlib, e.g.
    // cpp3) emit their JUMP_SLOTs into .rel.plt, NOT .rel.dyn, so cross-routing and
    // dl_cross_lookup must scan both sections to map a GOT slot back to its symbol.
    {
        std::uint32_t rp_addr = 0;
        auto relplt = sec_in_image(g_sc, mem, ".rel.plt", rp_addr);
        h->lazy_plt = loader::lazy_tables{relplt, dynsym, dynstr};
    }

    // P6: cross-library r10 routing. When enabled, point every EXTERNAL JUMP_SLOT
    // (st_value==0) of this image at dl_cross_call, which resolves the target at call
    // time and runs it with the callee library's own got_base in r10. Self-defined
    // JUMP_SLOTs (st_value!=0) stay as bound -- the caller already holds the right r10.
    // Deferring resolution lets mutual lib<->lib imports bind once both are loaded.
    if (g_cross_call) {
        using loader::detail::load_word;
        using loader::detail::store_word;
        const std::uint32_t tramp =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dl_cross_call)) | 1u;
        const std::span<const std::byte> tabs[2] = { h->lazy.jmprel, h->lazy_plt.jmprel };
        for (auto rel : tabs) {
            for_each_rel(rel, [&](std::uint32_t r_off, std::uint32_t r_info) {
                if (loader::elf::r_type(r_info) !=
                    static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) return;
                const std::uint32_t sym = loader::elf::r_sym(r_info);
                const std::uint32_t st_value =
                    load_word(dynsym, sym * sizeof(loader::elf::Sym) + 4);
                if (st_value != 0) return;
                store_word(mem, r_off, tramp);
            });
        }
    }

    std::uint32_t got_addr = 0;
    (void) sec_in_image(g_sc, mem, ".got", got_addr);
    h->got_base = base + got_addr;

    // 5d: record this image's relocated .ARM.exidx range and register it so the C++
    // unwinder (via __gnu_Unwind_Find_exidx) can find unwind info for a throw whose PC
    // lies inside this library. Each EHABI index entry is 8 bytes.
    std::uint32_t exidx_addr = 0;
    auto exidx = sec_in_image(g_sc, mem, ".ARM.exidx", exidx_addr);
    h->exidx_base  = exidx.empty() ? 0u : base + exidx_addr;
    h->exidx_count = static_cast<std::uint32_t>(exidx.size() / 8);

    // 5d: force the Thumb bit on each generic-model .ARM.extab personality routine.
    // The personality (__gxx_personality_v0) is imported, so the linker routes it through a
    // PLT veneer and writes the extab's PREL31 personality pointer to the veneer WITHOUT the
    // Thumb bit. On Cortex-M (Thumb-only) the unwinder's `blx <even>` to it faults (INVSTATE).
    // Walk the exidx: skip CANTUNWIND (word1==1) and inline-compact entries (word1 bit31 set,
    // personality is unwinder-internal pr0/1/2); for a generic entry (word1 = PREL31 to extab),
    // if the extab word0 is itself a PREL31 personality (bit31 clear), OR in the Thumb bit.
    if (!exidx.empty()) {
        using loader::detail::load_word;
        using loader::detail::store_word;
        auto sx31 = [](std::uint32_t v) -> std::int32_t {            // sign-extend 31-bit offset
            return (v & 0x40000000u) ? static_cast<std::int32_t>(v | 0x80000000u)
                                     : static_cast<std::int32_t>(v & 0x7fffffffu);
        };
        for (std::uint32_t i = 0; i < h->exidx_count; ++i) {
            const std::uint32_t w1_off = exidx_addr + i * 8 + 4;     // image offset of word1
            if (w1_off + 4 > mem.size()) break;
            const std::uint32_t w1 = load_word(mem, w1_off);
            if (w1 == 1u || (w1 & 0x80000000u)) continue;            // cantunwind / inline compact
            const std::int64_t tab_off = static_cast<std::int64_t>(w1_off) + sx31(w1);
            if (tab_off < 0 || tab_off + 4 > static_cast<std::int64_t>(mem.size())) continue;
            const std::uint32_t w0 = load_word(mem, static_cast<std::size_t>(tab_off));
            if (w0 & 0x80000000u) continue;                          // extab uses inline-compact PR
            if (!(w0 & 1u)) store_word(mem, static_cast<std::size_t>(tab_off), w0 | 1u);
        }
    }

    dl_exidx_register(base, h->size, h->exidx_base, static_cast<int>(h->exidx_count));

    // Constructors: find the array by section TYPE (SHT_INIT_ARRAY), not name -- the C
    // libs' linker script emits it as ".inits", so a name lookup would miss it. Entries
    // are already RELATIVE-relocated to absolute.
    std::span<std::byte> ia{};
    if (const auto* sh = g_sc.find_by_type(kShtInitArray))
        ia = std::span<std::byte>{mem.data() + sh->sh_addr, sh->sh_size};
    for (std::size_t off = 0; off + 4 <= ia.size(); off += 4) {
        std::uint32_t fn; std::memcpy(&fn, ia.data() + off, 4);
        if (fn) pic_call_void(reinterpret_cast<void*>(fn),
                              reinterpret_cast<void*>(h->got_base));
    }

    std::span<std::byte> fa{};
    if (const auto* sh = g_sc.find_by_type(kShtFiniArray))
        fa = std::span<std::byte>{mem.data() + sh->sh_addr, sh->sh_size};
    h->fini_array = fa.empty() ? 0 : reinterpret_cast<std::uint32_t>(fa.data());
    h->fini_count = static_cast<std::uint32_t>(fa.size() / 4);
    return h;
}

loader::result<void*> dlsym(handle& h, const char* name) noexcept {
    auto v = h.resolver.address_of(name);
    if (!v) return loader::unexpected(v.err());
    return reinterpret_cast<void*>(h.base + *v);
}

void dlclose(handle& h) noexcept {
    if (!h.in_use) return;
    if (h.refcount != 0) {                    // another loaded library still imports from this one
        scl_log_puts("dlclose refused: library still in use\n");
        return;
    }
    for (std::uint32_t i = 0; i < h.fini_count; ++i) {
        std::uint32_t fn;
        std::memcpy(&fn, reinterpret_cast<void*>(h.fini_array + i * 4), 4);
        if (fn) pic_call_void(reinterpret_cast<void*>(fn),
                              reinterpret_cast<void*>(h.got_base));
    }
    for (std::uint32_t d = 0; d < h.dep_count; ++d)   // release the libs this one imported from
        if (g_handles[h.deps[d]].refcount) g_handles[h.deps[d]].refcount--;
    dl_exidx_unregister(h.base);                      // 5d: drop this image's unwind table
    image_arena().free(reinterpret_cast<void*>(h.base));
    h = handle{};
}

std::uint32_t dl_fixup_count() noexcept { return g_fixup_count; }

/**
 * @brief Lazy-binding resolver callback (C linkage) for the dl_runtime_resolve trampoline.
 *
 * Called on the FIRST call to a lazily-bound import: @p slot is &GOT[n]. Resolve its
 * JUMP_SLOT symbol, patch the slot so later calls bypass the resolver, and return the
 * target (Thumb bit set) for the tail-call.
 */
/**
 * @brief Log a lazy-binding failure before dl_runtime_resolve branches to the
 *        null target (a guaranteed fault): name the slot's symbol so the fault
 *        is attributable. Goes through the scl_log hook — silent when the app
 *        keeps the default no-op hook. (Lesson from on-target triage: a bare
 *        `bx 0` with no message is very expensive to diagnose.)
 */
void log_fixup_miss(const handle* h, std::uint32_t slot_addr, const char* why) noexcept {
    scl_log_puts("[dl] lazy fixup MISS at 0x");
    put_hex("", slot_addr);
    if (h) {
        using loader::detail::load_word;
        using loader::detail::name_at;
        const std::uint32_t off = slot_addr - h->base;
        const std::span<const std::byte> tabs[2] = { h->lazy.jmprel, h->lazy_plt.jmprel };
        for (auto rel : tabs)
            for_each_rel(rel, [&](std::uint32_t r_off, std::uint32_t r_info) {
                if (r_off != off) return;
                const std::uint32_t sym = loader::elf::r_sym(r_info);
                const std::uint32_t st_name =
                    load_word(h->lazy.dynsym, std::size_t(sym) * sizeof(loader::elf::Sym));
                scl_log_puts(" sym=");
                for (char c : name_at(h->lazy.dynstr, st_name)) scl_log_putc(c);
            });
    }
    scl_log_puts(" (");
    scl_log_puts(why);
    scl_log_puts(") -- the pending call will fault\n");
}

extern "C" void* dl_fixup(std::uint32_t* slot) noexcept {
    const std::uint32_t a = reinterpret_cast<std::uint32_t>(slot);
    handle* h = nullptr;
    for (auto& hh : g_handles)
        if (hh.in_use && a >= hh.base && a < hh.base + hh.size) { h = &hh; break; }
    if (!h) { log_fixup_miss(nullptr, a, "no owning image"); return nullptr; }

    auto linkr = link_context::from_self();
    if (!linkr) { log_fixup_miss(h, a, "no link_context"); return nullptr; }

    // Resolution order matches eager binding: a symbol defined in this lib itself binds
    // locally (st_value != 0 -> base + st_value); otherwise other loaded libs, then main.
    struct fixup_ctx { handle* self; std::array<handle, 4>* libs; link_context* main; }
        fc{h, &g_handles, &*linkr};
    loader::symbol_source resolve{
        +[](void* ctx, std::string_view n) -> loader::result<std::uint32_t> {
            auto* f = static_cast<fixup_ctx*>(ctx);
            auto sv = f->self->resolver.address_of(n);
            if (sv.has_value() && *sv != 0u) return f->self->base + *sv;   // self-defined
            for (auto& lib : *f->libs) {
                if (!lib.in_use || &lib == f->self) continue;
                auto v = lib.resolver.address_of(n);
                if (v.has_value() && *v != 0u) return lib.base + *v;       // another loaded lib
            }
            return f->main->resolve(n);                                    // main
        },
        &fc};

    auto r = loader::lazy_resolve_slot(a - h->base, h->lazy, resolve);
    if (!r) { log_fixup_miss(h, a, "symbol unresolved"); return nullptr; }
    *slot = *r;            // patch the GOT slot: later calls jump straight to the target
    ++g_fixup_count;
    return reinterpret_cast<void*>(*r);
}

/**
 * @brief Cross-call resolver callback (C linkage) for the dl_cross_call trampoline (P6).
 *
 * Called on every lib->lib (or lib->main) external call. @p got_slot_addr is &GOT[n] in
 * the CALLER's image. Map it back to the JUMP_SLOT symbol, resolve it (other loaded libs
 * first, then main), and return { target|1 (low 32 bits), callee_got (high 32 bits) }.
 * The trampoline installs callee_got into r10 around the call so the callee reaches its
 * own globals; for a main target callee_got is 0 (main is -fpic, r10-free).
 * @return The packed {target,got} pair, or 0 if the slot is unknown.
 */
extern "C" std::uint64_t dl_cross_lookup(std::uint32_t got_slot_addr) noexcept {
    using loader::detail::load_word;
    using loader::detail::name_at;

    handle* caller = nullptr;
    for (auto& hh : g_handles)
        if (hh.in_use && got_slot_addr >= hh.base && got_slot_addr < hh.base + hh.size) {
            caller = &hh; break;
        }
    if (!caller) return 0;
    const std::uint32_t off = got_slot_addr - caller->base;

    std::string_view name{};
    const loader::lazy_tables* tabs[2] = { &caller->lazy, &caller->lazy_plt };
    for (auto* t : tabs) {
        for_each_rel(t->jmprel, [&](std::uint32_t r_off, std::uint32_t r_info) {
            if (r_off != off) return;
            if (loader::elf::r_type(r_info) !=
                static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) return;
            const std::uint32_t s = loader::elf::r_sym(r_info);
            name = name_at(t->dynstr, load_word(t->dynsym, s * sizeof(loader::elf::Sym) + 0));
        });
        if (!name.empty()) break;
    }
    if (name.empty()) return 0;

    // Resolve: other loaded libs (cross-lib -> swap r10 to its got_base) first, then main.
    std::uint32_t target = 0, got = 0;
    handle* owner = nullptr;
    for (auto& lib : g_handles) {
        if (!lib.in_use || &lib == caller) continue;
        auto v = lib.resolver.address_of(name);
        if (v.has_value() && *v != 0u) {
            target = lib.base + *v; got = lib.got_base; owner = &lib; break;
        }
    }
    if (!owner) {
        if (auto linkr = link_context::from_self())
            if (auto v = linkr->resolve(name)) { target = *v; got = 0; }  // main: r10 irrelevant
    }
    if (target == 0) return 0;

    // Trace the GOT/PLT translation: cross-lib hops always; main targets only when verbose.
    if ((owner && g_cross_trace >= 1) || g_cross_trace >= 2) {
        scl_log_puts("  [xlat] "); scl_log_puts(caller->name); scl_log_puts(" -> ");
        scl_log_puts(owner ? owner->name : "main"); scl_log_puts(" : ");
        for (char c : name) scl_log_putc(c);
        if (g_cross_trace >= 2) {
            put_hex("  slot=0x", got_slot_addr);
            put_hex(" tgt=0x", target | 1u);
            put_hex(" got=0x", got);
        }
        scl_log_putc('\n');
    }
    return (static_cast<std::uint64_t>(got) << 32) | static_cast<std::uint64_t>(target | 1u);
}

void dl_enable_cross_call(bool on) noexcept { g_cross_call = on; }
void dl_cross_trace(int level) noexcept { g_cross_trace = level; }

// P6/debug: dump one image's GOT/PLT relocation tables (.rel.dyn + .rel.plt) entry by
// entry, showing for each: the absolute GOT slot address, the reloc type, the symbol it
// binds, and the slot's CURRENT 32-bit value with an annotation of where it points. This
// is the static snapshot of the dynamic-translation machinery -- read it together with
// the verbose [xlat] lines (dl_cross_trace(2)) to watch a slot go
//   raw file value  ->  dl_cross_call (deferred)  ->  real target (at call time).
void dl_dump_image(const handle& h) noexcept {
    using loader::detail::load_word;
    using loader::detail::name_at;
    const std::uint32_t tramp  =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dl_cross_call))     | 1u;
    const std::uint32_t lazyfx =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dl_runtime_resolve)) | 1u;

    scl_log_puts("  [GOT/PLT] "); scl_log_puts(h.name);
    put_hex(" base=0x", h.base); put_hex(" got_base=0x", h.got_base);
    put_hex(" size=0x", h.size); scl_log_putc('\n');

    const loader::lazy_tables* tabs[2] = { &h.lazy, &h.lazy_plt };
    const char*               tnm [2] = { ".rel.dyn", ".rel.plt" };
    for (int ti = 0; ti < 2; ++ti) {
        const auto rel    = tabs[ti]->jmprel;
        const auto dynsym = tabs[ti]->dynsym;
        const auto dynstr = tabs[ti]->dynstr;
        const std::size_t n = rel.size() / sizeof(loader::elf::Rel);
        scl_log_puts("    "); scl_log_puts(tnm[ti]); scl_log_puts(": ");
        put_dec(static_cast<std::uint32_t>(n)); scl_log_puts(" entr"); scl_log_puts(n == 1 ? "y\n" : "ies\n");
        for_each_rel(rel, [&](std::uint32_t off, std::uint32_t info) {
            const std::uint8_t  ty   = loader::elf::r_type(info);
            const std::uint32_t s    = loader::elf::r_sym(info);
            const std::uint32_t slot = h.base + off;
            std::uint32_t cur = 0; std::memcpy(&cur, reinterpret_cast<const void*>(slot), 4);
            std::string_view nm{};
            if (s) nm = name_at(dynstr, load_word(dynsym, s * sizeof(loader::elf::Sym) + 0));
            put_hex("      slot=0x", slot);
            scl_log_puts(" "); scl_log_puts(reloc_name(ty)); scl_log_puts(" ");
            if (nm.empty()) scl_log_puts("(no sym)");
            else            for (char c : nm) scl_log_putc(c);
            put_hex(" = 0x", cur);
            if      (cur == tramp)  scl_log_puts("  -> dl_cross_call (deferred lib->lib r10-swap)");
            else if (cur == lazyfx) scl_log_puts("  -> dl_runtime_resolve (lazy bind)");
            else if (cur >= h.base && cur < h.base + h.size) scl_log_puts("  (in-image / RELATIVE)");
            else if ((cur & ~1u) >= 0x08000000u && (cur & ~1u) < 0x08080000u) scl_log_puts("  -> main (flash)");
            else if (cur == 0u)     scl_log_puts("  (unbound)");
            scl_log_putc('\n');
        });
    }
}

// P6/debug: resolve `names` against MAIN and print each flash target -- the PROVIDING
// side of every lib->main GOT/PLT translation (got_base is always 0: main is -fpic).
void dl_dump_main_provides(const char* const* names, int n) noexcept {
    auto link = link_context::from_self();
    scl_log_puts("  [main .dynsym] flash targets a lib resolves to (got_base=0, r10-free):\n");
    for (int i = 0; i < n; ++i) {
        std::uint32_t a = 0;
        if (link) if (auto v = link->resolve(names[i])) a = *v;
        scl_log_puts("      "); scl_log_puts(names[i]);
        put_hex(" -> 0x", a); scl_log_putc('\n');
    }
}

/**
 * @brief Find the relocation (in .rel.dyn or .rel.plt) targeting GOT image offset @p off.
 * @param[out] sym  Set to the relocation's symbol name (if any).
 * @return The reloc TYPE, or 0 if no entry targets that slot. Lets the walkthrough say
 *         WHY each slot changed (rebased / bound / routed). (P6/test-7.)
 */
static std::uint8_t got_slot_reloc(const handle& h, std::uint32_t off,
                                    std::string_view& sym) noexcept {
    using loader::detail::load_word;
    using loader::detail::name_at;
    using loader::detail::for_each_rel;
    const loader::lazy_tables* tabs[2] = { &h.lazy, &h.lazy_plt };
    for (auto* t : tabs) {
        std::uint8_t result = 0;
        for_each_rel(t->jmprel, [&](std::uint32_t r_off, std::uint32_t r_info) {
            if (r_off != off) return;
            const std::uint32_t s = loader::elf::r_sym(r_info);
            if (s) sym = name_at(t->dynstr, load_word(t->dynsym, s * sizeof(loader::elf::Sym) + 0));
            result = loader::elf::r_type(r_info);
        });
        if (result) return result;
    }
    return 0;
}

// P6/test-7: the didactic GOT walkthrough. Prints the image's .got BEFORE relocation (the
// snapshot captured at load) and AFTER, side by side, annotating each slot with what the
// loader did to it -- reserved, rebased (RELATIVE), data bound (GLOB_DAT), or function slot
// routed to the cross-call / lazy trampoline (JUMP_SLOT). Closes with a JUMP_SLOT routing
// table: each PLT-backed function slot and where it now points.
void dl_dump_got_walkthrough(const handle& h) noexcept {
    using rt = loader::elf::rel_type;
    const std::uint32_t tramp  =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dl_cross_call))     | 1u;
    const std::uint32_t lazyfx =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dl_runtime_resolve)) | 1u;

    scl_log_puts("\nGOT/PLT DIDACTIC WALKTHROUGH ("); scl_log_puts(h.name); scl_log_puts(")\n");
    scl_log_puts("=============================\n");

    if (h.got_words == 0) {
        scl_log_puts("  (.got not captured for this image -- none, or > snapshot cap)\n");
        return;
    }

    // -- STEP 1: raw .got values as loaded, BEFORE the relocator touched them. --
    scl_log_puts("\n[STEP 1] Reading .so file initial GOT values (BEFORE patching)...\n");
    put_hex("  .got at VA=0x", h.got_off);
    scl_log_puts(": "); put_dec(h.got_words); scl_log_puts(" entries BEFORE relocation:\n");
    for (std::uint16_t i = 0; i < h.got_words; ++i) {
        const std::uint32_t off = h.got_off + i * 4u;
        std::string_view sym{};
        const std::uint8_t ty = got_slot_reloc(h, off, sym);
        scl_log_puts("    ["); if (i < 10) scl_log_putc(' '); put_dec(i); scl_log_puts("] ");
        put_hex("0x", h.got_raw[i]);
        if      (i < 3)                       scl_log_puts("  (reserved)");
        else if (static_cast<rt>(ty) == rt::RELATIVE) scl_log_puts("  (self-relative -> rebase)");
        else if (ty)                          scl_log_puts("  (to be patched)");
        scl_log_putc('\n');
    }

    // -- STEP 3: the same slots AFTER relocation + trampoline routing, with the diff. --
    scl_log_puts("\n[STEP 3] .got AFTER patching ("); put_dec(h.got_words); scl_log_puts(" entries):\n");
    scl_log_puts("  BEFORE -> AFTER  (what the loader did)\n");
    scl_log_puts("  ----------------------------------------\n");
    for (std::uint16_t i = 0; i < h.got_words; ++i) {
        const std::uint32_t off  = h.got_off + i * 4u;
        const std::uint32_t slot = h.got_base + i * 4u;
        std::uint32_t cur = 0; std::memcpy(&cur, reinterpret_cast<const void*>(slot), 4);
        std::string_view sym{};
        const std::uint8_t ty = got_slot_reloc(h, off, sym);
        scl_log_puts("  ["); if (i < 10) scl_log_putc(' '); put_dec(i); scl_log_puts("] ");
        put_hex("0x", h.got_raw[i]); put_hex(" -> 0x", cur);
        if      (i < 3)             scl_log_puts("  (reserved by dynamic linker)");
        else if (cur == tramp)      scl_log_puts("  <- routed to dl_cross_call (deferred lib->lib)");
        else if (cur == lazyfx)     scl_log_puts("  <- routed to dl_runtime_resolve (lazy bind)");
        else if (static_cast<rt>(ty) == rt::GLOB_DAT)  scl_log_puts("  <- PATCHED (data symbol resolved)");
        else if (static_cast<rt>(ty) == rt::RELATIVE) scl_log_puts("  <- rebased (+ load bias)");
        else if (static_cast<rt>(ty) == rt::JUMP_SLOT) scl_log_puts("  <- bound (self-defined fn)");
        else if (cur == h.got_raw[i]) scl_log_puts("  (unchanged)");
        if (!sym.empty()) { scl_log_puts("  "); for (char c : sym) scl_log_putc(c); }
        scl_log_putc('\n');
    }

    // -- PLT routing: every JUMP_SLOT GOT slot a PLT stub jumps through, and its target. --
    scl_log_puts("\n[PLT] function slots reached via the PLT (GOT JUMP_SLOTs):\n");
    using loader::detail::load_word;
    using loader::detail::name_at;
    using loader::detail::for_each_rel;
    const loader::lazy_tables* tabs[2] = { &h.lazy, &h.lazy_plt };
    const char*               tnm [2] = { ".rel.dyn", ".rel.plt" };
    for (int ti = 0; ti < 2; ++ti) {
        for_each_rel(tabs[ti]->jmprel, [&](std::uint32_t off, std::uint32_t info) {
            if (static_cast<rt>(loader::elf::r_type(info)) != rt::JUMP_SLOT) return;
            const std::uint32_t slot = h.base + off;
            std::uint32_t cur = 0; std::memcpy(&cur, reinterpret_cast<const void*>(slot), 4);
            const std::uint32_t s = loader::elf::r_sym(info);
            std::string_view nm = s ? name_at(tabs[ti]->dynstr,
                load_word(tabs[ti]->dynsym, s * sizeof(loader::elf::Sym) + 0)) : std::string_view{};
            scl_log_puts("    "); scl_log_puts(tnm[ti]);
            put_hex(" GOT[0x", off); put_hex("]=0x", cur); scl_log_puts("  ");
            if (nm.empty()) scl_log_puts("(no sym)"); else for (char c : nm) scl_log_putc(c);
            if      (cur == tramp)  scl_log_puts("  (cross-call)");
            else if (cur == lazyfx) scl_log_puts("  (lazy)");
            scl_log_putc('\n');
        });
    }
}

} // namespace fw

namespace {
char g_dl_error_buf[128] = {0};
bool g_dl_error_active = false;

void set_dl_error(const char* msg) {
    if (msg) {
        std::strncpy(g_dl_error_buf, msg, sizeof(g_dl_error_buf) - 1);
        g_dl_error_buf[sizeof(g_dl_error_buf) - 1] = '\0';
        g_dl_error_active = true;
    } else {
        g_dl_error_active = false;
    }
}

const char* error_string(loader::error err) {
    switch (err) {
        case loader::error::OK: return "Ok";
        case loader::error::NOT_ELF: return "Not an ELF file";
        case loader::error::NOT_ARM32: return "Not ARM 32-bit ELF";
        case loader::error::NOT_SHARED_OBJECT: return "Not a shared object (.so)";
        case loader::error::BAD_SECTION: return "Bad or missing section";
        case loader::error::OUT_OF_MEMORY: return "Out of memory";
        case loader::error::SHORT_READ: return "Short read / file corrupted";
        case loader::error::UNSUPPORTED_RELOC: return "Unsupported relocation type";
        case loader::error::SYMBOL_NOT_FOUND: return "Symbol not found";
        case loader::error::FILE_NOT_FOUND: return "File not found";
        default: return "Unknown SCL error";
    }
}
} // namespace

namespace fw {
// Declare weak signatures of platform loaders to support automatic mapping
__attribute__((weak)) loader::result<handle*> dlopen(const char* path) noexcept;
__attribute__((weak)) loader::result<handle*> dlopen_fs(const char* name) noexcept;
}

extern bool g_use_flatfs __attribute__((weak));

extern "C" __attribute__((weak)) loader::result<fw::handle*> scl_impl_dlopen(const char* filename, int flag) noexcept {
    (void)flag;
    bool use_fs = false;
    if (&g_use_flatfs) {
        use_fs = g_use_flatfs;
    }

    if (use_fs && &fw::dlopen_fs) {
        const char* b = filename;
        for (const char* p = filename; *p; ++p) {
            if (*p == '/') b = p + 1;
        }
        return fw::dlopen_fs(b);
    }

    if (&fw::dlopen) {
        return fw::dlopen(filename);
    }

    return loader::unexpected(loader::error::FILE_NOT_FOUND);
}

void* dl::dlopen(const char* filename, int flag) noexcept {
    set_dl_error(nullptr);
    if (!filename) {
        set_dl_error("filename is NULL");
        return nullptr;
    }
    auto res = scl_impl_dlopen(filename, flag);
    if (!res) {
        set_dl_error(error_string(res.err()));
        return nullptr;
    }
    return reinterpret_cast<void*>(*res);
}

void* dl::dlsym(void* handle, const char* symbol) noexcept {
    set_dl_error(nullptr);
    if (!handle || !symbol) {
        set_dl_error("Invalid argument to dlsym");
        return nullptr;
    }
    auto* h = reinterpret_cast<fw::handle*>(handle);
    auto res = fw::dlsym(*h, symbol);
    if (!res) {
        set_dl_error(error_string(res.err()));
        return nullptr;
    }
    return *res;
}

int dl::dlclose(void* handle) noexcept {
    set_dl_error(nullptr);
    if (!handle) {
        set_dl_error("handle is NULL");
        return -1;
    }
    auto* h = reinterpret_cast<fw::handle*>(handle);
    fw::dlclose(*h);
    return 0;
}

char* dl::dlerror() noexcept {
    if (g_dl_error_active) {
        g_dl_error_active = false;
        return g_dl_error_buf;
    }
    return nullptr;
}


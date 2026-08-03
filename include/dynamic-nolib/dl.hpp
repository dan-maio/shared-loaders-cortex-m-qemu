/**
 * @file dl.hpp
 * @brief dlopen/dlsym/dlclose for ELF shared objects over semihosting.
 */
#pragma once
#include "loader/symbol-resolver.hpp"
#include "loader/lazy-resolve.hpp"
#include "loader/byte-source.hpp"
#include "loader/error.hpp"
#include <cstdint>

namespace fw {

/// Max .got entries snapshotted per image for the test-7 didactic walkthrough.
/// cpp3.so has 12, c2.so 18 -- 32 covers every battery library with margin.
inline constexpr int K_GOT_SNAP_MAX = 32;

struct handle {
    std::uint32_t base = 0;        ///< Image base (load bias).
    std::uint32_t size = 0;        ///< Allocated image size.
    std::uint32_t got_base = 0;    ///< r10 value for entering this library.
    std::uint32_t got_off  = 0;    ///< Image offset of .got (slot N lives at got_base + N*4).
    std::uint16_t got_words = 0;   ///< # of 4-byte .got entries captured (0 if none/oversize).
    std::uint32_t got_raw[K_GOT_SNAP_MAX] = {}; ///< .got snapshot taken post-copy, PRE-relocation.
    std::uint32_t fini_array = 0;  ///< Absolute address of fini fn-ptr array (0 if none).
    std::uint32_t fini_count = 0;  ///< Number of fini entries.
    loader::symbol_resolver resolver{ {}, {}, {} };
    bool in_use = false;
    std::uint32_t refcount = 0;     ///< # of currently-loaded libraries importing from this one.
    std::uint32_t dep_count = 0;    ///< # of entries in `deps`.
    std::uint8_t  deps[4] = {0,0,0,0}; ///< Indices (into g_handles) this lib imports from.
    loader::lazy_tables lazy{};      ///< 5c: .rel.dyn JMPREL/dynsym/dynstr spans for JUMP_SLOT fixup.
    loader::lazy_tables lazy_plt{};  ///< P6: .rel.plt JUMP_SLOT spans (cpp3 keeps them here, not .rel.dyn).
    std::uint32_t exidx_base = 0;   ///< 5d: runtime addr of this image's .ARM.exidx (0 if none).
    std::uint32_t exidx_count = 0;  ///< 5d: # of 8-byte EHABI entries in .ARM.exidx.
    char name[12] = {0};            ///< P6: short provenance name (basename, no dir/.so) for tracing.
};

/** @brief Core loader over any byte_source: copy, relocate, resolve against main,
 *         run constructors. @p rawname seeds the handle's short provenance name
 *         (basename without dir/extension). Applications wrap this with their own
 *         storage front-end (semihosting file, flash archive blob, ...). */
[[nodiscard]] loader::result<handle*>
dlopen_source(loader::byte_source& src, const char* rawname) noexcept;
/** @brief Resolve @p name in @p h to an absolute address. */
[[nodiscard]] loader::result<void*> dlsym(handle& h, const char* name) noexcept;
/** @brief Run destructors, free the image, invalidate @p h. */
void dlclose(handle& h) noexcept;

/** @brief P6: enable/disable cross-library r10 routing (off by default; the stock
 *         firmware is unaffected). When on, dlopen points every external JUMP_SLOT at
 *         dl_cross_call so lib->lib calls run with the callee's own got_base. */
void dl_enable_cross_call(bool on) noexcept;
/** @brief P6: GOT/PLT translation trace verbosity. 0=off, 1=cross-lib hops only,
 *         2=verbose (also main targets + slot/target/got addresses, for test 7). */
void dl_cross_trace(int level) noexcept;

/** @brief P6/debug: dump @p h's GOT/PLT relocation tables (.rel.dyn + .rel.plt) entry
 *         by entry -- slot address, reloc type, symbol, current value + annotation. The
 *         static snapshot of the dynamic-translation state (read alongside [xlat]). */
void dl_dump_image(const handle& h) noexcept;
/** @brief P6/debug: print the flash address MAIN provides for each of @p names -- the
 *         target side of a lib->main GOT/PLT hop (got_base is always 0; main is -fpic). */
void dl_dump_main_provides(const char* const* names, int n) noexcept;

/** @brief P6/debug (test 7): didactic GOT walkthrough for @p h. Prints the .got contents
 *         BEFORE relocation (the raw snapshot taken at load) and AFTER, side by side, with a
 *         per-slot annotation of what the loader did (reserved / rebased / data bound /
 *         routed to the cross-call or lazy trampoline), plus a compact .plt summary. */
void dl_dump_got_walkthrough(const handle& h) noexcept;

} // namespace fw

#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 256
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif

class dl {
public:
    static void* dlopen(const char* filename, int flag) noexcept;
    static void* dlsym(void* handle, const char* symbol) noexcept;
    static int dlclose(void* handle) noexcept;
    static char* dlerror() noexcept;
};

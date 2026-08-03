/**
 * @file adapter.hpp
 * @brief XIP adapter for the standalone static ELF32 loader — the two-base model.
 *
 * Unlike the dynamic-nolib loader there is NO dlopen/dlsym service and no
 * call-time lazy binding: every symbol is resolved once, at load, and the GOT
 * is patched in place (the classic static-linker model). The only XIP-specific
 * work is the two-base split — the R-X segment executes in place from flash and
 * only the RW segment (GOT/.data/.bss) is copied to RAM (r10 = got_base).
 *
 * Imports from the application are resolved against **main's own `.dynsym`**
 * (`fw::link_context::from_self()`), exactly like the nolib loader — main is
 * built `-pie -Wl,-E`, so every global it exports is already in its dynamic
 * symbol table. No hand-maintained host table:
 *   - DATA imports (GLOB_DAT) are written into the RAM GOT at load;
 *   - FUNCTION imports (JUMP_SLOT) are written straight into the flash-resident
 *     GOT at provision time by bind_host_imports() — main is r10-free, so the
 *     flash PLT can branch to it directly with no trampoline.
 *
 * Cross-library function imports (lib A calls a function exported by loaded
 * lib B) are supported through the shared xip_cross_call trampoline: an import
 * bind_host_imports() cannot resolve against main is pre-linked to the
 * trampoline, which at call time looks the symbol up in the set of loaded
 * modules (xip_cross_lookup), swaps r10 to the callee's got_base and jumps.
 * Loaded modules are registered by load() and dropped by unload(), so
 * dependencies must be loaded before their consumers are called.
 *
 * That second point is not an optimisation but a correctness requirement: a PLT
 * stub reaches its GOT slot PC-relatively (`add ip, pc`), so executing from
 * flash it reads the flash image of the GOT and never the RAM copy. See
 * doc/xip-plt-prelink.md.
 *
 * Consolidated from five divergent per-test copies; see doc/scl-conclusion.md.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "loader/symbol-resolver.hpp"

namespace xip::stat {

/// A loaded XIP module (two-base model).
struct SModule {
    bool           ok = false;
    std::uintptr_t text_base = 0;   ///< flash address of the R-X segment
    std::uintptr_t data_base = 0;   ///< RAM address of the RW segment
    std::uintptr_t got_base  = 0;   ///< RAM address of .got  (== r10 for pic_call_*)
    std::uint32_t  text_vaddr = 0, text_size = 0, data_vaddr = 0, data_memsz = 0;
    /// Flash address of the RW segment's file image (blob + data_off). A PLT stub
    /// executing from flash reaches its GOT slot PC-relatively, so this is where the
    /// trampoline sees the slot (and what xip_cross_lookup maps back to r_offset).
    std::uintptr_t flash_data_base = 0;
    /// Relocation + symbol tables of the flash image, kept for cross-library calls:
    /// the cross-call trampoline maps a GOT slot back to its symbol via these.
    std::span<const std::byte> reldyn{};
    std::span<const std::byte> relplt{};
    std::span<const std::byte> dynsym{};
    std::span<const std::byte> dynstr{};
    loader::symbol_resolver resolver{ {}, {}, {} };
    std::byte*     ram = nullptr;
};

/**
 * @brief Provision-time prelink: bind external FUNCTION imports into the image.
 *
 * Resolves every external `R_ARM_JUMP_SLOT` (st_value == 0) in `.rel.dyn` and
 * `.rel.plt` against main's dynamic symbols and writes the resolved address into
 * the writable @p image, so once flashed the R-X PLT reaches the app directly.
 * Call this **before** the image is written to flash: afterwards those words are
 * already programmed, and NOR can only clear bits without a sector erase.
 *
 * @return Number of slots patched, or -1 if the image could not be parsed.
 */
int bind_host_imports(std::span<std::byte> image) noexcept;

/**
 * @brief Load from a flash-resident blob.
 *
 * Keeps R-X in flash, copies RW to RAM, applies split-base relocations (imports
 * resolved against main's `.dynsym`), then runs the constructors with
 * r10 = got_base.
 *
 * @param patch_host_got When true, also walk main's own `_DYNAMIC` and point its
 *        GOT/PLT slots at this module's exports, so the app can call into the
 *        module by name with no trampoline. This is what the "direct" variants
 *        use; leave false for resolve() / pic_call_* style callers.
 * @return true on success.
 */
bool load(std::span<const std::byte> blob, SModule& out,
          bool patch_host_got = false) noexcept;

/// Static symbol resolution over the loaded image, used at setup to obtain the
/// addresses the app will call (flash for code, RAM for data). Not a runtime
/// dynamic-linker service. Returns nullptr if not found.
void* resolve(const SModule& m, const char* name) noexcept;

/// Free the RAM segment.
void unload(SModule& m) noexcept;

}  // namespace xip::stat

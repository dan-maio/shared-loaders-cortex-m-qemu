/**
 * @file nopic.hpp
 * @brief Fixed-address XIP/static adapter for the static ELF32 loader — no r10.
 *
 * The r10-free model in one sentence: the module is compiled `-fno-pic
 * -mlong-calls` and LINKED at fixed addresses, so its code contains no GOT, no
 * PLT, no PIC base register — every intra-module reference is an absolute
 * address the linker baked in. main can therefore call the module's exports
 * with a PLAIN function pointer: no pic_call_*, no got_base, no `register
 * void* r10_val`.
 *
 * Two fixed windows in main SRAM (linker symbols `_sxip_text_start/_end`,
 * `_sxip_data_start/_end`) hold the R-X and RW segments at the exact VAs the
 * module was linked at (0-bias load). `load()` is a memcpy + bss-zero + the one
 * relocation the linker did NOT pre-fill (every R_ARM_ABS32, `value = S + A`;
 * imports from main / other modules use `S` = the resolved address) +
 * `.init_array`. Because the windows are RAM, the loader patches those slots at
 * LOAD time — no provision-time flash binding is needed (contrast
 * `bind_host_imports()` in the r10 adapter, which must patch the flash-resident
 * GOT before flashing).
 *
 * Cross-module imports are resolved at load against the registry of already
 * loaded modules (load dependencies first), then main's own `.dynsym`
 * (`fw::link_context::from_self()`).
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "loader/symbol-resolver.hpp"

namespace xip::nopic {

/// A loaded fixed-address module (fixed windows, each at its own link VA).
struct PModule {
    bool           ok = false;
    std::uintptr_t text_base = 0;   ///< == text_vaddr (fixed window, 0 bias)
    std::uintptr_t data_base = 0;   ///< == data_vaddr (fixed window, 0 bias)
    std::uint32_t  text_vaddr = 0, text_size = 0;
    std::uint32_t  data_vaddr = 0, data_memsz = 0;
    /// The blob's relocation + symbol tables (point into the persistent fs
    /// flash image); kept so resolve_all() can re-patch imports once every
    /// module is loaded.
    std::span<const std::byte> reldyn{};
    std::span<const std::byte> dynsym{};
    std::span<const std::byte> dynstr{};
    loader::symbol_resolver resolver{ {}, {}, {} };   ///< over the blob's dynsym
};

/**
 * @brief Load a `-fno-pic -mlong-calls` fixed-address module from a blob.
 *
 * Copies the R-X segment into the `_sxip_text_*` window and the RW segment into
 * the `_sxip_data_*` window, zeroes the RW bss, resolves undefined-symbol
 * R_ARM_ABS32 imports (modules first, then main), runs `.init_array`, and
 * registers the module for cross-module resolution. Fails if the module's
 * linked VAs do not match the reserved windows or the segments overflow them.
 */
bool load(std::span<const std::byte> blob, PModule& out) noexcept;

/// Static symbol lookup: returns the absolute runtime address (0-bias, so the
/// symbol's st_value IS the address). nullptr if not found.
void* resolve(const PModule& m, const char* name) noexcept;

/// Look up a symbol in the loaded-module registry, then main. Used to satisfy
/// cross-module imports at load time.
void* resolve_any(const char* name) noexcept;

/**
 * @brief Re-resolve imports across ALL loaded modules (cyclic dependencies).
 *
 * load() resolves each module's R_ARM_ABS32 imports against whatever is loaded
 * at that moment. Mutually dependent modules (A imports B, B imports A) can
 * therefore leave slots unresolved; call this once after the whole set is
 * loaded to fill every still-zero import slot. Idempotent.
 */
void resolve_all() noexcept;

/**
 * @brief Patch MAIN's own GOT/PLT so the app can call the module BY NAME.
 *
 * Walk main's `_DYNAMIC` (.rel.dyn GLOB_DAT + .rel.plt JUMP_SLOT) and, for every
 * slot whose symbol this module EXPORTS, write the module's (fixed) address into
 * main's GOT. The app must be linked `-pie -Wl,-E` and reference the module's
 * symbols (e.g. via `-l:module.so`) so the linker emits those slots; call this
 * after load() for each module, before the first by-name call. Only slots the
 * module actually defines are touched.
 */
void patch_main(const PModule& m) noexcept;

/// Deregister the module (windows are fixed; nothing else to free).
void unload(PModule& m) noexcept;

}  // namespace xip::nopic

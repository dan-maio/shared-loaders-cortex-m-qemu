/**
 * @file adapter.hpp
 * @brief XIP adapter for the freestanding (nolib) dlopen loader.
 *
 * Loads a module from a flash-resident image span (e.g. an on-chip flat-fs blob,
 * fw::fs_blob). If the module can execute in place (split R-X | RW, immutable
 * text) the R-X segment stays in flash and only the RW segment is copied to RAM
 * (r10 = data_base) and relocated with split bases; otherwise it falls back to
 * the stock copy-whole nolib loader. Built entirely on existing core primitives
 * (elf_image, symbol_resolver, link_context, dlopen_source) — no changes to code
 * outside the XIP tree.
 *
 * The caller supplies the flash span so this stays independent of any particular
 * storage backend (the flat-fs lives in the test tree, not the core).
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "xip/xip-handle.hpp"

namespace xip::nolib {

using xip_map = xip::xip_map;

/// Load `so_image` (flash-resident bytes). `name` seeds the handle's short name.
/// Returns an opaque handle, or nullptr on failure.
void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept;

/// Resolve `name` in `handle` to its runtime address (flash for code, RAM for
/// data). Returns nullptr if not found.
void* dlsym(void* handle, const char* name) noexcept;

/// The r10 / GOT base to pass to pic_call_* when entering this module.
std::uint32_t get_got_base(void* handle) noexcept;

/// True if the module executes in place from flash (false => copy-whole).
bool is_xip(void* handle) noexcept;

/// Fill `out` with `handle`'s segment placement. Returns false if the handle is
/// invalid (or the module is not loaded).
bool get_xip_map(void* handle, xip_map& out) noexcept;

/// Release the handle (frees the RAM segment for XIP modules).
void dlclose(void* handle) noexcept;

/// Pre-resolve external calls for XIP. The linker's PLT stubs read their
/// `.got.plt` slot PC-relative, so once the R-X segment executes from flash the
/// slot must already hold the target: patch each external JUMP_SLOT entry in
/// `image` (a writable copy of the raw .so, VA==file_offset) with the address
/// resolved from main. Call at provision time, before the image is flashed.
/// Returns the number of slots patched, or -1 on parse failure.
int patch_main_imports(std::span<std::byte> image) noexcept;

}  // namespace xip::nolib

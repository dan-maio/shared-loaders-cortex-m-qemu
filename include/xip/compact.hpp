/**
 * @file compact.hpp
 * @brief Blocker C — decide whether a flash-resident .so can execute in place.
 *
 * A freestanding procedure (no host tooling): `so_image` is a module's raw .so
 * bytes *already resident in memory-mapped flash* (e.g. a blob in the on-chip
 * flat-fs — fw::fs_blob). It classifies the segments and scans the relocations;
 * on success it fills `out` with an in-place view — out.text spans the R-X bytes
 * where they already sit in flash (out.text_base = that flash address), out.data
 * spans the RW initializer bytes the loader must copy to RAM. Falls back
 * (out.xip = false) for images that cannot XIP — a fused RWX blob, or any
 * relocation that targets the R-X range.
 *
 * Note (Blocker C, alignment): the flat-fs stores blobs 4-byte aligned, which is
 * enough for Thumb-2 execution on this MMU-less M4 but does not honour the
 * linker's p_align; aligned re-placement is a later refinement of the packer.
 *
 * See doc/xip-flash-execution-analysis.md, "Blocker C".
 */
#pragma once
#include "xip/module.hpp"
#include <cstddef>
#include <span>

namespace xip {

/// Classify `so_image` (flash-resident) for XIP and fill `out` with an in-place
/// view. Returns true and sets out.xip on success; false => caller must load the
/// module the old way (copy whole image to RAM).
bool compact_for_xip(std::span<const std::byte> so_image, Module& out) noexcept;

}  // namespace xip

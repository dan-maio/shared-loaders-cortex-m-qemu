/**
 * @file relocate.hpp
 * @brief Split-base relocation + the R-X immutability safety scan.
 *
 * With one load bias there is one bias to add; with flash-text + RAM-data there
 * are two, and R_ARM_RELATIVE must resolve each addend against the SegmentMap
 * (text -> text_base, data -> data_base). Before that, the safety scan proves no
 * relocation targets the R-X range — otherwise the module cannot XIP.
 */
#pragma once
#include "xip/segment-map.hpp"
#include "loader/error.hpp"
#include "loader/reloc-applier.hpp"   // loader::symbol_source
#include <cstddef>
#include <cstdint>
#include <span>

namespace xip {

/// Safety scan (fail-safe): true if NO relocation's r_offset lands inside the
/// R-X segment, i.e. the text is immutable and may execute from flash. False =>
/// caller must fall back to copy-whole for this module.
bool text_is_immutable(std::span<const std::byte> image, const SegmentMap& seg) noexcept;

/// Apply one .rel(.dyn/.plt) table with SPLIT bases: the R-X segment executes
/// from flash at `text_base`, the RW segment lives in RAM at `data_base`. Only
/// RW words are written (`ram_data` spans the RAM copy, vaddr `seg.data_vaddr`).
/// Each fixup routes to text_base or data_base per the SegmentMap; externals
/// (st_value == 0) go through `resolve`.
loader::result<void> apply_rel_xip(const SegmentMap& seg,
                                   std::uintptr_t text_base,
                                   std::uintptr_t data_base,
                                   std::span<std::byte> ram_data,
                                   std::span<const std::byte> reldyn,
                                   std::span<const std::byte> dynsym,
                                   std::span<const std::byte> dynstr,
                                   loader::symbol_source resolve) noexcept;

}  // namespace xip

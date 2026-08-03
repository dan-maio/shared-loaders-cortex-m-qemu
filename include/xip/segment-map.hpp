/**
 * @file segment-map.hpp
 * @brief Blocker A — classify a split R-X | RW image into its two segments.
 *
 * Pure computation over program headers (no memory written). A module can XIP
 * only if it is linked with two PT_LOAD segments; a single fused RWX blob
 * cannot (code + writable data share a segment), and `split` reports that.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace xip {

struct SegmentMap {
    // R-X (executable, position-independent) — candidate to stay in flash.
    std::uintptr_t text_vaddr = 0, text_off = 0, text_size = 0, text_align = 0;
    // RW (GOT + .data + .bss + .data.rel.ro) — always copied to RAM.
    std::uintptr_t data_vaddr = 0, data_off = 0, data_filesz = 0, data_memsz = 0;

    /// false => single fused RWX segment: cannot XIP, caller copies whole.
    bool split = false;

    /// True if `vaddr` falls inside the RW range (GOT/.data/.bss/.data.rel.ro).
    bool in_data(std::uintptr_t vaddr) const noexcept {
        return vaddr >= data_vaddr && vaddr < data_vaddr + data_memsz;
    }

    /// True if `vaddr` falls inside the R-X range — used by the reloc scan and
    /// by split-base addend resolution to pick text_base vs data_base. When the
    /// linker packs both segments into one page (ALIGN small, not 0x1000) the RW
    /// VAs sit numerically inside [text_vaddr, text_vaddr+text_size); those must
    /// resolve to data, so `in_data` wins and is excluded here.
    bool in_text(std::uintptr_t vaddr) const noexcept {
        return vaddr >= text_vaddr && vaddr < text_vaddr + text_size
            && !in_data(vaddr);
    }
};

/// Parse `image`'s program headers; populate `out`. Returns false if the image
/// is not a clean two-segment split (out.split is set accordingly).
bool classify_segments(std::span<const std::byte> image, SegmentMap& out) noexcept;

}  // namespace xip

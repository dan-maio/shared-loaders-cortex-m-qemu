/**
 * @file compact.cpp
 * @brief Implementation of the XIP flash-compaction decision (Blocker C).
 */
#include "xip/compact.hpp"
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"

namespace xip {

bool compact_for_xip(std::span<const std::byte> so_image, Module& out) noexcept {
    out = Module{};

    SegmentMap seg;
    if (!classify_segments(so_image, seg) || !seg.split)
        return false;                         // fused RWX -> cannot XIP, copy whole
    if (!text_is_immutable(so_image, seg))
        return false;                         // a reloc targets R-X -> copy whole

    // Bounds: the R-X file bytes must lie within the image (subspan is unchecked).
    if (seg.text_off > so_image.size() ||
        seg.text_size > so_image.size() - seg.text_off)
        return false;

    // In-place view: the R-X bytes already sit in flash inside so_image; execute
    // them there. The RW half is handed to the loader to copy into RAM.
    out.text      = so_image.subspan(seg.text_off, seg.text_size);
    out.text_base = reinterpret_cast<std::uintptr_t>(out.text.data());
    // RW initializer bytes (filesz); .bss (memsz-filesz) is zero-filled by the
    // loader when it allocates the RAM segment. data_base is set at load time.
    out.data      = std::span<std::byte>{};   // filled by the loader's RAM copy
    out.data_base = 0;
    out.xip       = true;
    return true;
}

}  // namespace xip

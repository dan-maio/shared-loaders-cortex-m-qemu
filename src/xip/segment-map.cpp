/**
 * @file segment-map.cpp
 * @brief Implementation of split R-X | RW segment classification.
 */
#include "xip/segment-map.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"

namespace xip {
namespace {
// Standard ELF segment permission flags (Phdr::p_flags).
constexpr loader::elf::word PF_X = 0x1;
constexpr loader::elf::word PF_W = 0x2;
}  // namespace

bool classify_segments(std::span<const std::byte> image, SegmentMap& out) noexcept {
    out = SegmentMap{};

    loader::memory_byte_source src(image);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) return false;

    bool have_text = false, have_data = false;
    for (loader::elf::half i = 0; i < parsed->header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (auto r = parsed->program_header(i, ph); !r) return false;
        if (ph.p_type != loader::elf::PT_LOAD) continue;

        const bool exec  = (ph.p_flags & PF_X) != 0;
        const bool write = (ph.p_flags & PF_W) != 0;

        if (exec && !write) {              // R-X: the flash / XIP candidate
            if (have_text) return false;   // >1 exec segment: not our clean split
            out.text_vaddr = ph.p_vaddr;
            out.text_off   = ph.p_offset;
            out.text_size  = ph.p_memsz;
            out.text_align = ph.p_align;
            have_text = true;
        } else if (write) {                // RW: GOT/.data/.bss/.data.rel.ro -> RAM
            if (have_data) return false;   // (covers the fused RWX blob too)
            out.data_vaddr  = ph.p_vaddr;
            out.data_off    = ph.p_offset;
            out.data_filesz = ph.p_filesz;
            out.data_memsz  = ph.p_memsz;
            have_data = true;
        }
        // a pure read-only (R--) segment, if any, is not load-bearing here.
    }

    // A clean split also requires VA == file_offset for both segments:
    // the PLT stubs in flash compute their GOT address PC-relative, and that
    // only matches the RAM copy when VA and file_offset are identical.
    out.split = have_text && have_data &&
                out.text_vaddr == out.text_off &&
                out.data_vaddr  == out.data_off;
    return out.split;
}

}  // namespace xip

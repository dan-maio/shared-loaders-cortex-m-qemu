/**
 * @file image-memory.cpp
 * @brief image_memory implementation: loads PT_LOAD segments into RAM.
 */
#include "dynamic-newlib/image-memory.hpp"
#include <cstring>

namespace firmware {

/** @brief Load PT_LOAD segments from ELF into allocated buffer, BSS-zeroing. */
loader::result<void>
image_memory::load_segments(loader::elf_image& image,
                            std::size_t extra_bytes) noexcept {
    auto base_va = image.image_base();
    if (!base_va) return loader::unexpected(base_va.err());

    auto total = image.image_size();
    if (!total) return loader::unexpected(total.err());

    auto buf = std::make_unique<std::byte[]>(*total + extra_bytes);
    std::size_t written = 0;
    std::size_t seg_end = 0;

    for (loader::elf::half i = 0; i < image.header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (auto r = image.program_header(i, ph); !r)
            return loader::unexpected(r.err());
        if (ph.p_type != loader::elf::PT_LOAD) continue;

        std::size_t offset = ph.p_vaddr - *base_va;
        auto* dst = buf.get() + offset;
        if (ph.p_filesz) {
            if (auto r = image.source().read_at(
                    ph.p_offset,
                    std::span<std::byte>(dst, ph.p_filesz)); !r)
                return loader::unexpected(r.err());
        }
        if (ph.p_memsz > ph.p_filesz)
            std::memset(dst + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);

        written = offset + ph.p_memsz;
        if (written > seg_end) seg_end = written;
    }

    buf_ = std::move(buf);
    size_ = seg_end + extra_bytes;
    return {};
}

} // namespace firmware

/**
 * @file loaded-image.hpp
 * @brief Host helper: load an ELF .so into a flat image and run the pipeline.
 */
#pragma once
#include "loader/elf-image.hpp"
#include "loader/region-allocator.hpp"
#include "loader/relocator.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/symbol-resolver.hpp"
#include "support/memory-byte-source.hpp"
#include <cstring>
#include <vector>

namespace test {

/** @brief Read a whole section's bytes (by name) from the loaded image's file. */
inline std::vector<std::byte>
section_bytes(loader::elf_image& img, const char* name) {
    auto sh = img.find_section(name);
    if (!sh) return {};
    std::vector<std::byte> out(sh->sh_size);
    if (sh->sh_size) (void)img.source().read_at(sh->sh_offset, out);
    return out;
}

} // namespace test

/**
 * @file module.hpp
 * @brief An XIP-loaded module: the two-base model (flash text + RAM data).
 *
 * XIP splits a module across two address spaces instead of one load bias:
 *   - the R-X segment stays in flash and executes in place  (text_base),
 *   - only the RW segment is copied into RAM and relocated   (data_base / r10).
 * See doc/xip-flash-execution-analysis.md.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace xip {

struct Module {
    /// R-X segment, memory-mapped in flash — executed in place (never copied).
    std::span<const std::byte> text{};
    /// RW segment (GOT + .data + .bss + .data.rel.ro), the only RAM allocation.
    std::span<std::byte> data{};

    std::uintptr_t text_base = 0;  ///< flash address of the R-X segment
    std::uintptr_t data_base = 0;  ///< RAM address of the RW segment (== r10 / got_base)

    /// false  => safety scan failed (a reloc targets R-X, or the image is a
    ///           single fused RWX blob): the loader fell back to copy-whole.
    bool xip = false;
};

}  // namespace xip

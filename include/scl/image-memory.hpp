/**
 * @file image-memory.hpp
 * @brief region_allocator over the linker-reserved _simage_arena.
 */
#pragma once
#include "loader/region-allocator.hpp"

namespace fw {

/** @brief The process-wide image arena allocator (over _simage_arena_*). */
loader::region_allocator& image_arena() noexcept;

} // namespace fw

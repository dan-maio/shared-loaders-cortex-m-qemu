/** @file image-memory.cpp @brief See image-memory.hpp. */
#include "scl/image-memory.hpp"

/* Linker-provided bounds of the reserved RAM arena (their ADDRESSES delimit it). */
extern "C" {
extern unsigned char _simage_arena_start;  /**< First byte of the loaded-image arena. */
extern unsigned char _simage_arena_end;    /**< One past the last byte of the arena. */
}

namespace fw {

loader::region_allocator& image_arena() noexcept {
    static loader::region_allocator a(
        &_simage_arena_start,
        static_cast<std::size_t>(&_simage_arena_end - &_simage_arena_start));
    return a;
}

} // namespace fw

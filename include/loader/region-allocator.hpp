/**
 * @file region-allocator.hpp
 * @brief Fixed-capacity first-fit free-list allocator over an external region.
 *
 * No dynamic allocation, no globals. 8-byte aligned. Coalesces adjacent free
 * blocks. Used for the loaded-image arena and for main's C++ heap.
 */
#pragma once
#include "loader/error.hpp"
#include <cstddef>
#include <cstdint>

namespace loader {

class region_allocator {
public:
    static constexpr std::size_t kMaxBlocks = 16; ///< Max live (used + free) spans tracked.
    static constexpr std::size_t kAlign     = 8;  ///< Allocation granularity (bytes); every block is a multiple.

    /** @brief Manage @p size bytes at @p base as one initially-free block. */
    region_allocator(void* base, std::size_t size) noexcept;

    /** @brief Allocate @p n bytes (rounded up to kAlign). */
    [[nodiscard]] result<void*> alloc(std::size_t n) noexcept;

    /** @brief Free a pointer previously returned by alloc(). No-op for nullptr. */
    void free(void* p) noexcept;

private:
    /** @brief One span of the managed region (a slot in the fixed free list). */
    struct Block {
        std::uint32_t off;   ///< Byte offset of this span from @ref base_.
        std::uint32_t len;   ///< Span length in bytes.
        bool          used;  ///< true = handed out by alloc(); false = free.
        bool          live;  ///< true = this slot describes a real span; false = empty/reusable slot.
    };
    std::uintptr_t base_;            ///< Base address of the managed region.
    std::size_t    size_;            ///< Total managed size in bytes.
    Block          blocks_[kMaxBlocks]; ///< Fixed free list (no dynamic allocation).

    /** @brief Index of the first non-live (reusable) slot in @ref blocks_, or -1 if full. */
    int find_free_slot() noexcept;
    /** @brief Merge every pair of adjacent free spans so freed memory becomes reusable. */
    void coalesce() noexcept;
};

} // namespace loader

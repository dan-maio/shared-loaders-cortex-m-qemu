/** @file region-allocator.cpp @brief See region-allocator.hpp. */
#include "loader/region-allocator.hpp"

namespace loader {

/** @brief Round @p n up to the next multiple of region_allocator::kAlign. */
static std::size_t round_up(std::size_t n) noexcept {
    return (n + region_allocator::kAlign - 1) & ~(region_allocator::kAlign - 1);
}

region_allocator::region_allocator(void* base, std::size_t size) noexcept
    : base_(reinterpret_cast<std::uintptr_t>(base)), size_(size), blocks_{} {
    blocks_[0] = Block{0, static_cast<std::uint32_t>(size), false, true};
}

int region_allocator::find_free_slot() noexcept {
    for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i)
        if (!blocks_[i].live) return i;
    return -1;
}

result<void*> region_allocator::alloc(std::size_t n) noexcept {
    const std::uint32_t need = static_cast<std::uint32_t>(round_up(n == 0 ? 1 : n));
    for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i) {
        Block& b = blocks_[i];
        if (!b.live || b.used || b.len < need) continue;
        if (b.len == need) { b.used = true; }
        else {
            int s = find_free_slot();
            if (s < 0) return loader::unexpected(error::OUT_OF_MEMORY);
            blocks_[s] = Block{b.off + need, b.len - need, false, true};
            b.len = need; b.used = true;
        }
        return reinterpret_cast<void*>(base_ + b.off);
    }
    return loader::unexpected(error::OUT_OF_MEMORY);
}

void region_allocator::free(void* p) noexcept {
    if (!p) return;
    const std::uint32_t off = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(p) - base_);
    for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i)
        if (blocks_[i].live && blocks_[i].used && blocks_[i].off == off) {
            blocks_[i].used = false;
            coalesce();
            return;
        }
}

void region_allocator::coalesce() noexcept {
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i) {
            if (!blocks_[i].live || blocks_[i].used) continue;
            for (int j = 0; j < static_cast<int>(kMaxBlocks); ++j) {
                if (i == j || !blocks_[j].live || blocks_[j].used) continue;
                if (blocks_[i].off + blocks_[i].len == blocks_[j].off) {
                    blocks_[i].len += blocks_[j].len;
                    blocks_[j].live = false;
                    merged = true;
                }
            }
        }
    }
}

} // namespace loader

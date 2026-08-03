/**
 * @file patch-journal.cpp
 * @brief patch_journal implementation.
 */
#include "dynamic-newlib/patch-journal.hpp"
#include <cstring>

namespace firmware {

/** @brief Record a pre-patch value so it can be restored on unload. */
loader::result<void>
patch_journal::record(std::byte* address, std::uint32_t original_value) noexcept {
    if (count_ >= capacity_)
        return loader::unexpected(loader::error::OUT_OF_MEMORY);
    entries_[count_] = {address, original_value};
    ++count_;
    return {};
}

/** @brief Record N bytes as consecutive word entries (for R_ARM_COPY). */
loader::result<void>
patch_journal::record_bytes(std::byte* address, std::size_t nbytes) noexcept {
    auto nwords = (nbytes + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < nwords; ++i) {
        auto* addr = address + i * sizeof(std::uint32_t);
        std::uint32_t orig{};
        std::memcpy(&orig, addr, sizeof(orig));
        auto r = record(addr, orig);
        if (!r) return r;
    }
    return {};
}

/** @brief Revert all recorded patches (pop order), restoring original values. */
void patch_journal::revert() noexcept {
    while (count_ > 0) {
        --count_;
        if (entries_[count_].address)
            std::memcpy(entries_[count_].address, &entries_[count_].original_value,
                        sizeof(std::uint32_t));
    }
}

} // namespace firmware

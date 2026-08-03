/**
 * @file patch-journal.hpp
 * @brief Records every word written into main's memory/GOT for safe dlclose revert.
 */
#pragma once
#include "loader/error.hpp"
#include <cstddef>
#include <cstdint>

namespace firmware {

/** @brief Default number of patch entries in the journal buffer. */
static constexpr std::size_t kJournalDefaultCapacity = 1024;

/** @brief Journal that records original values before patching, enabling clean revert on dlclose. */
class patch_journal {
public:
    /** @brief A single patched location: address + the original value before overwrite. */
    struct Entry {
        std::byte* address{};         ///< Address that was patched
        std::uint32_t original_value{}; ///< Value before patching (for revert)
    };

    patch_journal() noexcept = default;

    /** @brief Point the journal at an external buffer (co-allocated with image). */
    void use_buffer(std::byte* buf, std::size_t cap) noexcept {
        if (reinterpret_cast<std::uintptr_t>(buf) % alignof(Entry) != 0)
            return;  // misaligned: silently keep using fixed buffer
        entries_ = reinterpret_cast<Entry*>(buf);
        capacity_ = cap;
        count_ = 0;
    }

    /** @brief Record a pre-patch value so it can be restored on unload. */
    [[nodiscard]] loader::result<void>
    record(std::byte* address, std::uint32_t original_value) noexcept;

    /** @brief Record N bytes as consecutive word entries (for R_ARM_COPY). */
    [[nodiscard]] loader::result<void>
    record_bytes(std::byte* address, std::size_t nbytes) noexcept;

    /** @brief Revert all recorded patches in reverse order. */
    void revert() noexcept;

    /** @brief Number of entries recorded so far. */
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    /** @brief True if no entries recorded. */
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    /** @brief Maximum capacity of the journal buffer. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    Entry*     entries_   = fixed_entries_;  ///< Pointer to journal buffer (co-allocated or fixed)
    std::size_t capacity_ = kJournalDefaultCapacity; ///< Max entries before OutOfMemory
    std::size_t count_    = 0;                      ///< Current number of recorded patches
    Entry fixed_entries_[kJournalDefaultCapacity]{}; ///< Fallback static buffer (unused when co-allocated)
};

} // namespace firmware

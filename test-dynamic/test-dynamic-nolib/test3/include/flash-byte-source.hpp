/**
 * @file flash-byte-source.hpp
 * @brief A loader::byte_source over a span of memory-mapped on-chip flash.
 *
 * flat_fs stores each library as a blob in the FLASH_FS region. Because internal
 * flash is memory-mapped (read via the bus at 0x08xxxxxx), "reading a file" is a
 * plain memcpy from the blob -- no semihosting round-trip. dlopen_fs() wraps a
 * blob in this byte_source and feeds it to the same dlopen_source() path that
 * semihost_file uses, so the loader is unchanged.
 */
#ifndef FLASH_BYTE_SOURCE_HPP_
#define FLASH_BYTE_SOURCE_HPP_

#include "loader/byte-source.hpp"
#include <cstring>

namespace fw {

/// @brief loader::byte_source over a [base, base+size) span of memory-mapped flash.
class Flashbyte_source final : public loader::byte_source {
public:
    /** @brief Wrap the @p size-byte flash blob at @p base. */
    Flashbyte_source(const std::byte* base, std::size_t size) noexcept
        : base_(base), size_(size) {}

    /** @brief Blob length in bytes. */
    [[nodiscard]] std::size_t size() const noexcept override { return size_; }

    /** @brief memcpy @p out.size() bytes from @p offset (bounds-checked -> ShortRead). */
    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > size_ || out.size() > size_ - offset)
            return loader::unexpected(loader::error::SHORT_READ);
        std::memcpy(out.data(), base_ + offset, out.size());
        return {};
    }

private:
    const std::byte* base_;   ///< Start of the blob in memory-mapped flash.
    std::size_t      size_;   ///< Blob length in bytes.
};

} // namespace fw

#endif /* FLASH_BYTE_SOURCE_HPP_ */

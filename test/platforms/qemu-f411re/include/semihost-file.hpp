/**
 * @file semihost-file.hpp
 * @brief byte_source backed by an ARM semihosting file handle (read-only).
 */
#pragma once
#include "loader/byte-source.hpp"

namespace fw {

class semihost_file final : public loader::byte_source {
public:
    /** @brief Open @p path read-only ("rb" == SYS_OPEN mode 1). */
    static loader::result<semihost_file> open(const char* path) noexcept;

    /** @brief Move: steal @p o's handle (leaving it closed). */
    semihost_file(semihost_file&& o) noexcept : handle_(o.handle_), size_(o.size_) { o.handle_ = -1; }
    /** @brief Close the underlying semihosting handle. */
    ~semihost_file() override;

    /** @brief File size in bytes (queried at open()). */
    [[nodiscard]] std::size_t size() const noexcept override { return size_; }
    /** @brief Read @p out.size() bytes from @p offset (bounds-checked). */
    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override;

private:
    /** @brief Wrap an open handle @p h of known size @p sz (used by open()). */
    semihost_file(int h, std::size_t sz) noexcept : handle_(h), size_(sz) {}
    int         handle_;   ///< Semihosting file handle (-1 once moved-from/closed).
    std::size_t size_;     ///< Cached file size in bytes.
};

} // namespace fw

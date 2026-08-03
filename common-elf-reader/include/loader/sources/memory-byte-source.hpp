/**
 * @file memory-byte-source.hpp
 * @brief byte_source over a caller-owned in-memory buffer (no copy).
 *
 * The buffer is referenced by span; the caller must keep it alive for the
 * lifetime of the source. Useful when the whole object is already resident
 * (host tests, or firmware that mmaps/DMAs an image into RAM).
 */
#pragma once
#include "loader/byte-source.hpp"
#include <cstring>

namespace loader {

/// @brief Non-owning random-access byte_source over a byte span.
class memory_byte_source final : public byte_source {
public:
    memory_byte_source() noexcept = default;
    explicit memory_byte_source(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::size_t size() const noexcept override { return data_.size(); }

    [[nodiscard]] result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > data_.size() || out.size() > data_.size() - offset)
            return loader::unexpected(error::SHORT_READ);
        std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    std::span<const std::byte> data_{};
};

} // namespace loader

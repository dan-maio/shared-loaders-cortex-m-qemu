/**
 * @file memory-byte-source.hpp
 * @brief Host-only byte_source backed by a file slurped into memory.
 */
#pragma once
#include "loader/byte-source.hpp"
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace test {

/// @brief In-memory byte_source over a caller-owned buffer.
class Memorybyte_source final : public loader::byte_source {
public:
    /// @brief Load an entire file into memory.
    /// @return The source, or error::FILE_NOT_FOUND if it cannot be opened.
    static loader::result<Memorybyte_source> from_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return loader::unexpected(loader::error::FILE_NOT_FOUND);
        std::vector<char> raw(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        std::memcpy(bytes.data(), raw.data(), raw.size());
        return Memorybyte_source(std::move(bytes));
    }

    [[nodiscard]] std::size_t size() const noexcept override { return data_.size(); }

    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > data_.size() || out.size() > data_.size() - offset)
            return loader::unexpected(loader::error::SHORT_READ);
        std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    explicit Memorybyte_source(std::vector<std::byte> bytes) : data_(std::move(bytes)) {}
    std::vector<std::byte> data_;
};

} // namespace test

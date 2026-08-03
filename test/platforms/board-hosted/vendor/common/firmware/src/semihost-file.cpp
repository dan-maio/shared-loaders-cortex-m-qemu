/**
 * @file semihost-file.cpp
 * @brief semihost_file implementation using newlib FILE* (semihosting via _open).
 */
#include "firmware/semihost-file.hpp"
#include <cstdio>
#include <cstring>

namespace firmware {

/** @brief Open a file via fopen(path, "rb") for reading. */
semihost_file::semihost_file(const char* path) noexcept {
    fp_ = std::fopen(path, "rb");
}

/** @brief Destructor — closes the underlying FILE*. */
semihost_file::~semihost_file() noexcept { close(); }

/** @brief Return file size via ftell(fseeko) (SEEK_END/restore). */
std::size_t semihost_file::size() const noexcept {
    if (!fp_) return 0;
    long pos = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    long sz = std::ftell(fp_);
    std::fseek(fp_, pos, SEEK_SET);
    return sz < 0 ? 0 : static_cast<std::size_t>(sz);
}

/** @brief Read bytes at offset via fseek + fread (semihosting). */
loader::result<void>
semihost_file::read_at(std::size_t offset, std::span<std::byte> out) noexcept {
    if (!fp_ || out.empty()) return {};
    if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0)
        return loader::unexpected(loader::error::SHORT_READ);
    if (std::fread(out.data(), 1, out.size(), fp_) != out.size())
        return loader::unexpected(loader::error::SHORT_READ);
    return {};
}

/** @brief Close the underlying FILE* if open. */
void semihost_file::close() noexcept {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

} // namespace firmware

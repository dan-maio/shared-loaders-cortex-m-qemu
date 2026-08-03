/**
 * @file semihost-file.hpp
 * @brief RAII byte_source over std::fopen (semihosting file I/O via newlib).
 *
 * Uses newlib's fopen/fread/fseek/fclose which internally calls the
 * semihosting SYS_OPEN/SYS_READ/SYS_SEEK/SYS_CLOSE. This matches
 * the original f411re-sh_ project's approach and avoids direct
 * call_host() inline assembly which caused HardFaults.
 */
#pragma once
#include "loader/byte-source.hpp"
#include <cstdint>
#include <cstdio>

namespace firmware {

/** @brief A file opened via semihosting (newlib fopen), providing read_at and size for ELF loading. */
class semihost_file final : public loader::byte_source {
public:
    /** @brief Default constructor — no file opened. */
    semihost_file() noexcept = default;

    /** @brief Open a file via fopen(path, "rb") for reading. */
    explicit semihost_file(const char* path) noexcept;

    /** @brief Destructor — closes the underlying FILE*. */
    ~semihost_file() noexcept override;

    semihost_file(const semihost_file&) = delete;
    semihost_file& operator=(const semihost_file&) = delete;

    /** @brief True if the file was successfully opened. */
    [[nodiscard]] bool is_open() const noexcept { return fp_ != nullptr; }

    /** @brief Return file size via ftell + fseek. */
    [[nodiscard]] std::size_t size() const noexcept override;

    /** @brief Read bytes at a given offset via fseek + fread. */
    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override;

    /** @brief Close the underlying FILE* if open. */
    void close() noexcept;

private:
    std::FILE* fp_ = nullptr; ///< Newlib file pointer
};

} // namespace firmware

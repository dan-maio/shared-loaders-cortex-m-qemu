/**
 * @file byte-source.hpp
 * @brief Abstract random-access byte reader decoupling the core from I/O.
 *
 * On the target this is backed by semihosting file reads; in host tests it is
 * backed by an in-memory buffer. The core never assumes the whole object is
 * resident — it reads exactly the bytes it needs.
 */
#pragma once
#include "loader/error.hpp"
#include <cstddef>
#include <span>

namespace loader {

/// @brief Interface for reading bytes from an ELF object at arbitrary offsets.
class byte_source {
public:
    virtual ~byte_source() = default;

    /// @brief Total number of bytes available.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;

    /**
     * @brief Read exactly @p out.size() bytes starting at @p offset.
     * @param offset Byte offset from the start of the object.
     * @param out Destination span; fully filled on success.
     * @return Empty result on success.
     * @retval error::SHORT_READ The requested range exceeds the object.
     */
    [[nodiscard]] virtual result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};

} // namespace loader

#ifndef SPI_BYTE_SOURCE_HPP_
#define SPI_BYTE_SOURCE_HPP_

#include "loader/byte-source.hpp"
#include "spi-flash.h"
#include <cstring>

namespace fw {

class SpiByteSource final : public loader::byte_source {
public:
    SpiByteSource(std::uint32_t spi_addr, std::size_t size) noexcept
        : spi_addr_(spi_addr), size_(size) {}

    [[nodiscard]] std::size_t size() const noexcept override { return size_; }

    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (offset > size_ || out.size() > size_ - offset)
            return loader::unexpected(loader::error::SHORT_READ);
        std::uint32_t len = static_cast<std::uint32_t>(out.size());
        spi_flash_read(spi_addr_ + static_cast<std::uint32_t>(offset),
                       reinterpret_cast<std::uint8_t*>(out.data()),
                       static_cast<std::uint16_t>(len));
        return {};
    }

private:
    std::uint32_t spi_addr_;
    std::size_t   size_;
};

} // namespace fw

#endif

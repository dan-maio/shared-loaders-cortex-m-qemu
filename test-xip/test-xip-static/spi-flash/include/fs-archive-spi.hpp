/**
 * @file fs-archive-spi.hpp
 * @brief SPI flash flatfs archive with function pointers for runtime resolution.
 */
#ifndef FS_ARCHIVE_SPI_HPP_
#define FS_ARCHIVE_SPI_HPP_

#include "loader/error.hpp"
#include <cstddef>
#include <cstdint>

namespace fw_spi {

inline constexpr std::uint32_t kFsMagic    = 0x46414C31u;
inline constexpr std::uint32_t kFsVersion  = 1u;
inline constexpr std::uint32_t kFsTocCap   = 64u;
inline constexpr std::uint32_t kFsNameMax  = 16u;

inline constexpr std::uint32_t kFsStateFree = 0xFFFFFFFFu;
inline constexpr std::uint32_t kFsStateLive = 0x4C495645u;

struct FsEntry {
    char          name[kFsNameMax];
    std::uint32_t off;
    std::uint32_t size;
    std::uint32_t crc32;
    std::uint32_t state;
};
static_assert(sizeof(FsEntry) == 32);

struct FsSuper {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t toc_off;
    std::uint32_t toc_cap;
    std::uint32_t blob_off;
    std::uint32_t reserved[3];
};
static_assert(sizeof(FsSuper) == 32);

// Function pointer types for SPI flash operations
using fn_spi_flash_sector_erase = void(*)(std::uint32_t addr);
using fn_spi_flash_write = int(*)(std::uint32_t addr, const std::uint8_t* data, std::uint32_t len);
using fn_spi_flash_read = void(*)(std::uint32_t addr, std::uint8_t* data, std::uint16_t len);
using fn_spi_flash_page_program = int(*)(std::uint32_t addr, const std::uint8_t* data, std::uint16_t len);

// Initialize the SPI flash function pointers (must be called before any fs_* functions)
void fs_init(fn_spi_flash_sector_erase erase,
             fn_spi_flash_write write,
             fn_spi_flash_read read,
             fn_spi_flash_page_program program) noexcept;

[[nodiscard]] bool fs_valid() noexcept;
[[nodiscard]] loader::result<void> fs_format() noexcept;
[[nodiscard]] loader::result<void> fs_add(const char* name, const void* data, std::uint32_t len) noexcept;
[[nodiscard]] const FsEntry* fs_find(const char* name) noexcept;
[[nodiscard]] const FsEntry* fs_entry(std::uint32_t index) noexcept;
[[nodiscard]] const std::byte* fs_blob(const FsEntry& e) noexcept;
[[nodiscard]] std::uint32_t fs_count() noexcept;
[[nodiscard]] std::uint32_t fs_used() noexcept;
[[nodiscard]] std::uint32_t crc32_calc(const void* data, std::uint32_t len) noexcept;

} // namespace fw_spi

#endif

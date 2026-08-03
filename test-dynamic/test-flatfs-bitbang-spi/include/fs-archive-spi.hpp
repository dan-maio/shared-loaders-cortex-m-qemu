#ifndef FS_ARCHIVE_SPI_HPP_
#define FS_ARCHIVE_SPI_HPP_

#include "loader/error.hpp"
#include <cstddef>
#include <cstdint>

namespace fw {

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

[[nodiscard]] bool fs_valid() noexcept;
[[nodiscard]] loader::result<void> fs_format() noexcept;
[[nodiscard]] loader::result<void> fs_add(const char* name, const void* data, std::uint32_t len) noexcept;
[[nodiscard]] const FsEntry* fs_find(const char* name) noexcept;
[[nodiscard]] const FsEntry* fs_entry(std::uint32_t index) noexcept;
[[nodiscard]] const std::byte* fs_blob(const FsEntry& e) noexcept;
[[nodiscard]] std::uint32_t fs_count() noexcept;
[[nodiscard]] std::uint32_t fs_used() noexcept;
[[nodiscard]] std::uint32_t crc32_calc(const void* data, std::uint32_t len) noexcept;

} // namespace fw

#endif

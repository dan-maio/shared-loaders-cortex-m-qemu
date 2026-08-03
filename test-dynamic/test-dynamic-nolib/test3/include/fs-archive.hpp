/**
 * @file fs-archive.hpp
 * @brief flat_fs: an append-log flat archive in the on-chip flash filesystem
 *        region (FLASH_FS, sectors 6..7, 256K @ 0x08040000).
 *
 * "Flat archive" = a small superblock + a table of contents (TOC) + the file
 * bodies (blobs) concatenated, all in one flash region. No directories, no free
 * lists -- the minimal structure that maps a name to a span of bytes.
 *
 * "Append-log" = files are ADDED without erasing: a new blob is programmed into
 * the still-erased (0xFF) frontier, and one fresh TOC slot is programmed (its
 * `state` word written LAST as a commit marker). Because NOR flash only flips
 * bits 1->0 on a write, this needs no erase until the region fills. Replacing or
 * deleting a file is NOT supported in place (that needs 0->1, i.e. a sector
 * erase); re-`fs_format()` to start over. See docs/FLAT_FS_DESIGN.md.
 *
 * On-flash layout (all little-endian, 4-byte aligned):
 *
 *   _fs_base ─► [ FsSuper (32B) ][ FsEntry x toc_cap ][ blob 0 ][ blob 1 ] ...
 *               ^superblock      ^TOC (append slots)   ^blob region (frontier)
 */
#ifndef FS_ARCHIVE_HPP_
#define FS_ARCHIVE_HPP_

#include "loader/error.hpp"
#include <cstddef>
#include <cstdint>

namespace fw {

inline constexpr std::uint32_t kFsMagic    = 0x46414C31u;  ///< 'FAL1' archive magic.
inline constexpr std::uint32_t kFsVersion  = 1u;           ///< on-flash format version.
inline constexpr std::uint32_t kFsTocCap   = 64u;          ///< max files (append slots).
inline constexpr std::uint32_t kFsNameMax  = 16u;          ///< max name length incl. NUL.

inline constexpr std::uint32_t kFsStateFree = 0xFFFFFFFFu; ///< erased slot (unused).
inline constexpr std::uint32_t kFsStateLive = 0x4C495645u; ///< 'LIVE' committed slot.

/// @brief One TOC slot. `state` is the LAST word programmed (the commit marker),
///        so a torn write leaves it 0xFFFFFFFF (free) and the entry is ignored.
struct FsEntry {
    char          name[kFsNameMax]; ///< NUL-terminated file name (e.g. "c1.so").
    std::uint32_t off;              ///< byte offset from _fs_base to the blob.
    std::uint32_t size;             ///< blob length in bytes.
    std::uint32_t crc32;            ///< CRC-32 of the blob bytes.
    std::uint32_t state;            ///< kFsStateLive once committed.
};
static_assert(sizeof(FsEntry) == 32, "FsEntry must be 32 bytes (word-aligned slots)");

/// @brief Superblock at _fs_base, written once by fs_format().
struct FsSuper {
    std::uint32_t magic;       ///< kFsMagic.
    std::uint32_t version;     ///< kFsVersion.
    std::uint32_t toc_off;     ///< = sizeof(FsSuper).
    std::uint32_t toc_cap;     ///< = kFsTocCap.
    std::uint32_t blob_off;    ///< first blob offset (= toc_off + toc_cap*32).
    std::uint32_t reserved[3]; ///< pad to 32 bytes.
};
static_assert(sizeof(FsSuper) == 32, "FsSuper must be 32 bytes");

/// @brief True if the FS region holds a valid, current-version superblock.
[[nodiscard]] bool fs_valid() noexcept;

/// @brief Erase the FS sectors and write a fresh (empty) superblock.
[[nodiscard]] loader::result<void> fs_format() noexcept;

/// @brief Append one file. @p data must point into RAM (the flash driver reads
///        it while programming). Returns OutOfMemory if the TOC or region is full.
[[nodiscard]] loader::result<void>
fs_add(const char* name, const void* data, std::uint32_t len) noexcept;

/// @brief Find a live entry by name, or nullptr.
[[nodiscard]] const FsEntry* fs_find(const char* name) noexcept;

/// @brief The @p index-th live entry (0-based, in TOC order), or nullptr if out
///        of range. Lets callers enumerate the archive (e.g. the -l listing).
[[nodiscard]] const FsEntry* fs_entry(std::uint32_t index) noexcept;

/// @brief Pointer to an entry's blob bytes (directly in memory-mapped flash).
[[nodiscard]] const std::byte* fs_blob(const FsEntry& e) noexcept;

/// @brief Number of live files.
[[nodiscard]] std::uint32_t fs_count() noexcept;

/// @brief Bytes consumed so far (the blob write frontier, from _fs_base).
[[nodiscard]] std::uint32_t fs_used() noexcept;

/// @brief Reflected CRC-32 (poly 0xEDB88320) over @p len bytes of @p data.
[[nodiscard]] std::uint32_t crc32_calc(const void* data, std::uint32_t len) noexcept;

} // namespace fw

#endif /* FS_ARCHIVE_HPP_ */

/** @file fs-archive-internal.cpp @brief Internal flash flatfs archive. */
#include "fs-archive-internal.hpp"
#include "flash-prog.h"
#include <cstring>

// Linker-provided absolute symbols (their ADDRESS is the value): FS region base
// and size. MUST be at global scope with C linkage -- declared inside an anonymous
// namespace they would acquire internal linkage + a mangled name and fail to bind
// to the linker's `_fs_base`/`_fs_size` (resolving to 0 under --unresolved=ignore).
extern "C" {
extern std::uint8_t _fs_base[];
extern std::uint8_t _fs_size[];
}

namespace fw_int {
namespace {

/** @brief Base address of the FS region (the linker symbol's value). */
inline std::uint32_t fs_base() noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_fs_base));
}
/** @brief Size of the FS region in bytes (the linker symbol's value). */
inline std::uint32_t fs_size() noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_fs_size));
}
/** @brief Round @p v up to the next 4-byte boundary. */
inline std::uint32_t align4(std::uint32_t v) noexcept { return (v + 3u) & ~3u; }

/** @brief The superblock, memory-mapped at the FS base. */
inline const FsSuper* super() noexcept {
    return reinterpret_cast<const FsSuper*>(static_cast<std::uintptr_t>(fs_base()));
}
/** @brief The TOC (FsEntry array), memory-mapped at fs_base + toc_off. */
inline const FsEntry* toc() noexcept {
    return reinterpret_cast<const FsEntry*>(
        static_cast<std::uintptr_t>(fs_base() + super()->toc_off));
}

/** @brief Compare names bounded to kFsNameMax (both NUL-terminated within the field). */
bool name_eq(const char* a, const char* b) noexcept {
    for (std::uint32_t i = 0; i < kFsNameMax; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return true;   // both filled the field with no NUL: treat as equal
}

} // namespace

std::uint32_t crc32_calc(const void* data, std::uint32_t len) noexcept {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u)); // mask = (crc&1)?0xEDB..:0
    }
    return ~crc;
}

bool fs_valid() noexcept {
    const FsSuper* s = super();
    return s->magic == kFsMagic && s->version == kFsVersion
        && s->toc_off == sizeof(FsSuper) && s->toc_cap == kFsTocCap;
}

loader::result<void> fs_format() noexcept {
    flash_unlock();
    int rc = flash_erase_sector(FLASH_FS_SECTOR_LO);
    if (!rc) rc = flash_erase_sector(FLASH_FS_SECTOR_HI);
    if (rc) { flash_lock(); return loader::unexpected(loader::error::BAD_SECTION); }

    FsSuper s{};
    s.magic    = kFsMagic;
    s.version  = kFsVersion;
    s.toc_off  = sizeof(FsSuper);
    s.toc_cap  = kFsTocCap;
    s.blob_off = align4(sizeof(FsSuper) + kFsTocCap * sizeof(FsEntry));
    rc = flash_program(fs_base(), &s, sizeof(s));
    flash_cache_flush();
    flash_lock();
    if (rc) return loader::unexpected(loader::error::BAD_SECTION);
    return {};
}

const FsEntry* fs_find(const char* name) noexcept {
    if (!fs_valid()) return nullptr;
    const FsEntry* t = toc();
    const std::uint32_t cap = super()->toc_cap;
    for (std::uint32_t i = 0; i < cap; ++i)
        if (t[i].state == kFsStateLive && name_eq(t[i].name, name)) return &t[i];
    return nullptr;
}

const FsEntry* fs_entry(std::uint32_t index) noexcept {
    if (!fs_valid()) return nullptr;
    const FsEntry* t = toc();
    const std::uint32_t cap = super()->toc_cap;
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < cap; ++i)
        if (t[i].state == kFsStateLive) { if (seen == index) return &t[i]; ++seen; }
    return nullptr;
}

const std::byte* fs_blob(const FsEntry& e) noexcept {
    return reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(fs_base() + e.off));
}

std::uint32_t fs_count() noexcept {
    if (!fs_valid()) return 0;
    const FsEntry* t = toc();
    const std::uint32_t cap = super()->toc_cap;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < cap; ++i) if (t[i].state == kFsStateLive) ++n;
    return n;
}

std::uint32_t fs_used() noexcept {
    if (!fs_valid()) return 0;
    const FsEntry* t = toc();
    const std::uint32_t cap = super()->toc_cap;
    std::uint32_t frontier = super()->blob_off;
    for (std::uint32_t i = 0; i < cap; ++i)
        if (t[i].state == kFsStateLive) {
            std::uint32_t end = align4(t[i].off + t[i].size);
            if (end > frontier) frontier = end;
        }
    return frontier;
}

loader::result<void>
fs_add(const char* name, const void* data, std::uint32_t len) noexcept {
    if (!fs_valid()) return loader::unexpected(loader::error::BAD_SECTION);
    const FsSuper* s = super();
    const FsEntry* t = toc();

    // First free TOC slot + current blob frontier (max end of live blobs).
    std::uint32_t slot = s->toc_cap;
    std::uint32_t frontier = s->blob_off;
    for (std::uint32_t i = 0; i < s->toc_cap; ++i) {
        if (t[i].state == kFsStateFree) { if (slot == s->toc_cap) slot = i; }
        else if (t[i].state == kFsStateLive) {
            std::uint32_t end = align4(t[i].off + t[i].size);
            if (end > frontier) frontier = end;
        }
    }
    if (slot == s->toc_cap) return loader::unexpected(loader::error::OUT_OF_MEMORY); // TOC full
    if (frontier + len > fs_size()) return loader::unexpected(loader::error::OUT_OF_MEMORY); // no room

    flash_unlock();
    // 1) program the blob bytes into the erased frontier.
    if (flash_program(fs_base() + frontier, data, len)) {
        flash_lock(); return loader::unexpected(loader::error::BAD_SECTION);
    }
    // 2) build the TOC entry in RAM and program it: fields first (28B), then
    //    `state` (4B) LAST as the atomic commit marker.
    FsEntry e{};
    std::uint32_t n = 0;
    while (name[n] && n < kFsNameMax - 1u) { e.name[n] = name[n]; ++n; }
    e.name[n] = '\0';
    e.off   = frontier;
    e.size  = len;
    e.crc32 = crc32_calc(data, len);
    e.state = kFsStateLive;
    const std::uint32_t slot_addr = fs_base() + s->toc_off + slot * sizeof(FsEntry);
    if (flash_program(slot_addr, &e, sizeof(FsEntry) - 4u)) {
        flash_lock(); return loader::unexpected(loader::error::BAD_SECTION);
    }
    if (flash_program(slot_addr + (sizeof(FsEntry) - 4u), &e.state, 4u)) {
        flash_lock(); return loader::unexpected(loader::error::BAD_SECTION);
    }
    flash_cache_flush();
    flash_lock();
    return {};
}

} // namespace fw_int

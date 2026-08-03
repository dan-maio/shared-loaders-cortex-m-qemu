/**
 * @file fs-archive-spi.cpp
 * @brief SPI flash flatfs archive with function pointers for runtime resolution.
 */
#include "fs-archive-spi.hpp"
#include <cstring>

#define SPI_FS_BASE  0u
#define SPI_FS_SIZE  (1024u * 1024u)

namespace fw_spi {
namespace {

// Function pointers for SPI flash operations (set by fs_init)
fn_spi_flash_sector_erase g_erase = nullptr;
fn_spi_flash_write g_write = nullptr;
fn_spi_flash_read g_read = nullptr;
fn_spi_flash_page_program g_program = nullptr;

constexpr std::uint32_t kBlobOff = [](){
    return (sizeof(FsSuper) + kFsTocCap * sizeof(FsEntry) + 3u) & ~3u;
}();

std::uint32_t align4(std::uint32_t v) { return (v + 3u) & ~3u; }

FsSuper g_super;
FsEntry  g_toc[kFsTocCap];
bool     g_valid = false;

bool name_eq(const char* a, const char* b) {
    for (std::uint32_t i = 0; i < kFsNameMax; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return true;
}

void read_super(void) {
    g_read(SPI_FS_BASE, (uint8_t*)&g_super, sizeof(g_super));
}

void read_toc(void) {
    g_read(SPI_FS_BASE + g_super.toc_off, (uint8_t*)g_toc, kFsTocCap * sizeof(FsEntry));
}

} // namespace

void fs_init(fn_spi_flash_sector_erase erase,
             fn_spi_flash_write write,
             fn_spi_flash_read read,
             fn_spi_flash_page_program program) noexcept {
    g_erase = erase;
    g_write = write;
    g_read = read;
    g_program = program;
}

std::uint32_t crc32_calc(const void* data, std::uint32_t len) noexcept {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

bool fs_valid() noexcept {
    return g_valid && g_super.magic == kFsMagic && g_super.version == kFsVersion
        && g_super.toc_off == sizeof(FsSuper) && g_super.toc_cap == kFsTocCap;
}

loader::result<void> fs_format() noexcept {
    if (!g_erase || !g_write) return loader::unexpected(loader::error::BAD_SECTION);

    g_erase(0);
    for (std::uint32_t a = 0; a < SPI_FS_SIZE; a += 4096)
        g_erase(a);

    g_super.magic    = kFsMagic;
    g_super.version  = kFsVersion;
    g_super.toc_off  = sizeof(FsSuper);
    g_super.toc_cap  = kFsTocCap;
    g_super.blob_off = kBlobOff;
    memset(g_toc, 0xFF, sizeof(g_toc));

    if (g_write(SPI_FS_BASE, (const uint8_t*)&g_super, sizeof(g_super)))
        return loader::unexpected(loader::error::BAD_SECTION);
    if (g_write(SPI_FS_BASE + g_super.toc_off, (const uint8_t*)g_toc, sizeof(g_toc)))
        return loader::unexpected(loader::error::BAD_SECTION);

    g_valid = true;
    return {};
}

const FsEntry* fs_find(const char* name) noexcept {
    if (!fs_valid()) return nullptr;
    for (std::uint32_t i = 0; i < kFsTocCap; ++i)
        if (g_toc[i].state == kFsStateLive && name_eq(g_toc[i].name, name))
            return &g_toc[i];
    return nullptr;
}

const FsEntry* fs_entry(std::uint32_t index) noexcept {
    if (!fs_valid()) return nullptr;
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < kFsTocCap; ++i)
        if (g_toc[i].state == kFsStateLive) { if (seen == index) return &g_toc[i]; ++seen; }
    return nullptr;
}

const std::byte* fs_blob(const FsEntry& e) noexcept {
    (void)e;
    return nullptr;
}

std::uint32_t fs_count() noexcept {
    if (!fs_valid()) return 0;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < kFsTocCap; ++i) if (g_toc[i].state == kFsStateLive) ++n;
    return n;
}

std::uint32_t fs_used() noexcept {
    if (!fs_valid()) return 0;
    std::uint32_t frontier = g_super.blob_off;
    for (std::uint32_t i = 0; i < kFsTocCap; ++i)
        if (g_toc[i].state == kFsStateLive) {
            std::uint32_t end = align4(g_toc[i].off + g_toc[i].size);
            if (end > frontier) frontier = end;
        }
    return frontier;
}

loader::result<void> fs_add(const char* name, const void* data, std::uint32_t len) noexcept {
    if (!fs_valid()) return loader::unexpected(loader::error::BAD_SECTION);
    if (!g_write || !g_program) return loader::unexpected(loader::error::BAD_SECTION);

    std::uint32_t slot = kFsTocCap;
    std::uint32_t frontier = g_super.blob_off;
    for (std::uint32_t i = 0; i < kFsTocCap; ++i) {
        if (g_toc[i].state == kFsStateFree) { if (slot == kFsTocCap) slot = i; }
        else if (g_toc[i].state == kFsStateLive) {
            std::uint32_t end = align4(g_toc[i].off + g_toc[i].size);
            if (end > frontier) frontier = end;
        }
    }
    if (slot == kFsTocCap) return loader::unexpected(loader::error::OUT_OF_MEMORY);
    if (frontier + len > SPI_FS_SIZE) return loader::unexpected(loader::error::OUT_OF_MEMORY);

    if (g_write(SPI_FS_BASE + frontier, (const uint8_t*)data, len))
        return loader::unexpected(loader::error::BAD_SECTION);

    FsEntry e{};
    std::uint32_t n = 0;
    while (name[n] && n < kFsNameMax - 1u) { e.name[n] = name[n]; ++n; }
    e.name[n] = '\0';
    e.off   = frontier;
    e.size  = len;
    e.crc32 = crc32_calc(data, len);
    e.state = kFsStateLive;
    g_toc[slot] = e;

    if (g_program(SPI_FS_BASE + g_super.toc_off + slot * sizeof(FsEntry),
                                (const uint8_t*)&e, sizeof(FsEntry)))
        return loader::unexpected(loader::error::BAD_SECTION);

    return {};
}

} // namespace fw_spi

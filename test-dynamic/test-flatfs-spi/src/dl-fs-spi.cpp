#include "dl.hpp"
#include "dl-fs.hpp"
#include "fs-archive-spi.hpp"
#include "spi-byte-source.hpp"
#include "spi-flash.h"

namespace fw {

loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);
    if (!e) return loader::unexpected(loader::error::FILE_NOT_FOUND);
    spi_flash_reinit();
    SpiByteSource src(e->off, e->size);
    return dlopen_source(src, name);
}

} // namespace fw

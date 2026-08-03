/**
 * @file dl-fs.cpp
 * @brief flat_fs: dlopen_fs() -- load a library by name from the on-chip flat
 *        archive, reusing the loader's dlopen_source() over a Flashbyte_source.
 *
 * This translation unit is linked ONLY into the flat_fs firmware (Makefile_FLATFS).
 * It provides the strong definition of fw::dlopen_fs that overrides the weak stub
 * in dl.cpp, so the stock firmware and the semihosting battery never drag in the
 * filesystem/flash code.
 */
#include "dl.hpp"
#include "dl-fs.hpp"
#include "fs-archive.hpp"
#include "flash-byte-source.hpp"

namespace fw {

loader::result<handle*> dlopen_fs(const char* name) noexcept {
    const FsEntry* e = fs_find(name);
    if (!e) return loader::unexpected(loader::error::FILE_NOT_FOUND);
    Flashbyte_source src(fs_blob(*e), e->size);
    return dlopen_source(src, name);   // src outlives the call
}

} // namespace fw

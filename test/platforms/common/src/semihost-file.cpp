/** @file semihost-file.cpp @brief See semihost-file.hpp. */
#include "semihost-file.hpp"
#include "semihost.h"

namespace fw {

loader::result<semihost_file> semihost_file::open(const char* path) noexcept {
    int h = semihost_open(path, 1);              // mode 1 == "rb"
    if (h < 0) return loader::unexpected(loader::error::FILE_NOT_FOUND);
    long n = semihost_flen(h);
    if (n < 0) { semihost_close(h); return loader::unexpected(loader::error::FILE_NOT_FOUND); }
    return semihost_file(h, static_cast<std::size_t>(n));
}

semihost_file::~semihost_file() { if (handle_ >= 0) semihost_close(handle_); }

loader::result<void>
semihost_file::read_at(std::size_t offset, std::span<std::byte> out) noexcept {
    if (offset > size_ || out.size() > size_ - offset)
        return loader::unexpected(loader::error::SHORT_READ);
    if (out.empty()) return {};
    if (semihost_seek(handle_, static_cast<long>(offset)) != 0)
        return loader::unexpected(loader::error::SHORT_READ);
    int left = semihost_read(handle_, out.data(), static_cast<int>(out.size()));
    if (left != 0) return loader::unexpected(loader::error::SHORT_READ);
    return {};
}

} // namespace fw

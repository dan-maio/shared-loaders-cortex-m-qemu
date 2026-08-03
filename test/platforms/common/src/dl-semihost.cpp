/** @file dl-semihost.cpp @brief See dl-semihost.hpp. */
#include "dl-semihost.hpp"
#include "semihost-file.hpp"
#include <utility>

namespace fw {
loader::result<handle*> dlopen(const char* path) noexcept {
    auto srcr = semihost_file::open(path);
    if (!srcr) return loader::unexpected(srcr.err());
    semihost_file src = std::move(*srcr);
    return dlopen_source(src, path);   // src outlives the call
}
} // namespace fw

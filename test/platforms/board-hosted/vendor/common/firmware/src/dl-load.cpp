/** @file dl-load.cpp @brief See dl-load.hpp. */
#include "firmware/dl-load.hpp"
#include "firmware/semihost-file.hpp"
#include <cstring>
#include <vector>

namespace firmware {

loader::result<library*>
load_semihost(dynamic_linker& linker, const char* path) noexcept {
    semihost_file file(path);
    if (!file.is_open())
        return loader::unexpected(loader::error::FILE_NOT_FOUND);

    auto sz = file.size();
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    if (auto r = file.read_at(0, {buf.data(), buf.size()}); !r)
        return loader::unexpected(r.err());

    /* MemSrc reads from the slurped buffer at CPU speed (no per-read
     * semihosting round-trips through the debug probe). */
    struct MemSrc final : loader::byte_source {
        explicit MemSrc(std::vector<std::byte> d) : data(std::move(d)) {}
        std::vector<std::byte> data;
        std::size_t size() const noexcept override { return data.size(); }
        loader::result<void>
        read_at(std::size_t off, std::span<std::byte> out) noexcept override {
            if (off > data.size() || out.size() > data.size() - off)
                return loader::unexpected(loader::error::SHORT_READ);
            std::memcpy(out.data(), data.data() + off, out.size());
            return {};
        }
    };
    MemSrc mem{std::move(buf)};
    return linker.load_from_source(mem);
}

} // namespace firmware

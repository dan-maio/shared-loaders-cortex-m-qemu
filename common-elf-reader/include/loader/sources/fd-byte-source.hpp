/**
 * @file fd-byte-source.hpp
 * @brief byte_source backed by a POSIX / semihosting file descriptor.
 *
 * Suitable for the Cortex-M target when files are served over semihosting, and
 * for any host that provides open()/read()/lseek(). The whole object is never
 * resident: each read_at() seeks and reads exactly the bytes requested.
 *
 * Header-only and optional — a consumer that has its own I/O backing does not
 * need it; it just implements loader::byte_source directly.
 */
#pragma once
#include "loader/byte-source.hpp"
#include <fcntl.h>
#include <unistd.h>

namespace loader {

/// @brief Random-access byte_source over a file descriptor (RAII).
class fd_byte_source final : public byte_source {
public:
    fd_byte_source() noexcept = default;
    explicit fd_byte_source(int fd) noexcept : fd_(fd) { cache_size(); }

    fd_byte_source(const fd_byte_source&) = delete;
    fd_byte_source& operator=(const fd_byte_source&) = delete;

    fd_byte_source(fd_byte_source&& o) noexcept : fd_(o.fd_), size_(o.size_) {
        o.fd_ = -1;
        o.size_ = 0;
    }
    fd_byte_source& operator=(fd_byte_source&& o) noexcept {
        if (this != &o) {
            close_fd();
            fd_ = o.fd_;
            size_ = o.size_;
            o.fd_ = -1;
            o.size_ = 0;
        }
        return *this;
    }

    ~fd_byte_source() override { close_fd(); }

    /// @brief Open @p path read-only. Returns FILE_NOT_FOUND if it cannot open.
    static result<fd_byte_source> open_file(const char* path) noexcept {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return loader::unexpected(error::FILE_NOT_FOUND);
        return fd_byte_source(fd);
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    [[nodiscard]] std::size_t size() const noexcept override { return size_; }

    [[nodiscard]] result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (fd_ < 0) return loader::unexpected(error::FILE_NOT_FOUND);
        if (offset > size_ || out.size() > size_ - offset)
            return loader::unexpected(error::SHORT_READ);
        if (::lseek(fd_, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(-1))
            return loader::unexpected(error::SHORT_READ);
        std::size_t got = 0;
        auto* p = reinterpret_cast<unsigned char*>(out.data());
        while (got < out.size()) {
            ssize_t r = ::read(fd_, p + got, out.size() - got);
            if (r <= 0) return loader::unexpected(error::SHORT_READ);
            got += static_cast<std::size_t>(r);
        }
        return {};
    }

private:
    void cache_size() noexcept {
        if (fd_ < 0) { size_ = 0; return; }
        off_t end = ::lseek(fd_, 0, SEEK_END);
        size_ = (end > 0) ? static_cast<std::size_t>(end) : 0;
        ::lseek(fd_, 0, SEEK_SET);
    }
    void close_fd() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int fd_{-1};
    std::size_t size_{0};
};

} // namespace loader

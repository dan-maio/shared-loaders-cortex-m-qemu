#include "firmware/semihost-file.hpp"
#include <cstdint>
#include <cstring>

namespace {
// ARM semihosting operation numbers
enum { SYS_OPEN = 0x01, SYS_CLOSE = 0x02, SYS_READ = 0x06, SYS_SEEK = 0x0A, SYS_FLEN = 0x0C };

static inline std::uintptr_t semihost_call(std::uintptr_t op, std::uintptr_t arg) {
    register std::uintptr_t r0 __asm__("r0") = op;
    register std::uintptr_t r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

int semihost_open(const char* path, int mode) {
    std::uint32_t args[3] = { (std::uint32_t) path, (std::uint32_t) mode,
                              (std::uint32_t) __builtin_strlen(path) };
    return (int) semihost_call(SYS_OPEN, (std::uintptr_t) args);
}

int semihost_close(int handle) {
    std::uint32_t a = (std::uint32_t) handle;
    return (int) semihost_call(SYS_CLOSE, (std::uintptr_t) &a);
}

long semihost_flen(int handle) {
    std::uint32_t a = (std::uint32_t) handle;
    return (long) semihost_call(SYS_FLEN, (std::uintptr_t) &a);
}

int semihost_seek(int handle, long pos) {
    std::uint32_t args[2] = { (std::uint32_t) handle, (std::uint32_t) pos };
    return (int) semihost_call(SYS_SEEK, (std::uintptr_t) args);
}

int semihost_read(int handle, void* buf, int len) {
    std::uint32_t args[3] = { (std::uint32_t) handle, (std::uint32_t) buf, (std::uint32_t) len };
    return (int) semihost_call(SYS_READ, (std::uintptr_t) args);
}
} // namespace

namespace firmware {

semihost_file::semihost_file(const char* path) noexcept {
    int h = semihost_open(path, 1); // 1 = "rb"
    if (h >= 0) {
        fp_ = reinterpret_cast<std::FILE*>(static_cast<std::uintptr_t>(h + 1));
    } else {
        fp_ = nullptr;
    }
}

semihost_file::~semihost_file() noexcept {
    close();
}

std::size_t semihost_file::size() const noexcept {
    if (!fp_) return 0;
    int h = static_cast<int>(reinterpret_cast<std::uintptr_t>(fp_)) - 1;
    long sz = semihost_flen(h);
    return sz < 0 ? 0 : static_cast<std::size_t>(sz);
}

loader::result<void>
semihost_file::read_at(std::size_t offset, std::span<std::byte> out) noexcept {
    if (!fp_ || out.empty()) return {};
    int h = static_cast<int>(reinterpret_cast<std::uintptr_t>(fp_)) - 1;
    if (semihost_seek(h, static_cast<long>(offset)) != 0)
        return loader::unexpected(loader::error::SHORT_READ);
    if (semihost_read(h, out.data(), static_cast<int>(out.size())) != 0)
        return loader::unexpected(loader::error::SHORT_READ);
    return {};
}

void semihost_file::close() noexcept {
    if (fp_) {
        int h = static_cast<int>(reinterpret_cast<std::uintptr_t>(fp_)) - 1;
        semihost_close(h);
        fp_ = nullptr;
    }
}

} // namespace firmware

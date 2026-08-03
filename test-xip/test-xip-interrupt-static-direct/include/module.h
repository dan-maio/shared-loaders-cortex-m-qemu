#ifndef SCL_STATIC_MODULE_H_
#define SCL_STATIC_MODULE_H_

#include <cstdint>
#include <cstring>
#include <span>
#include "xip/static/adapter.hpp"
#include "fs-archive.hpp"

namespace elf_loader { class symbol_table; }

namespace scl {

enum class module_error {
    OK,
    FILE_NOT_FOUND,
    NOT_AN_ELF,
    NO_DYNSYM,
    LOAD_FAILED
};

class Module {
public:
    Module() = default;
    ~Module() {
        if (m_.ok) {
            xip::stat::unload(m_);
        }
    }

    // Movable but not copyable
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& o) noexcept : m_(o.m_), err_(o.err_) { o.m_ = xip::stat::SModule{}; }
    Module& operator=(Module&& o) noexcept {
        if (m_.ok) xip::stat::unload(m_);
        m_ = o.m_;
        err_ = o.err_;
        o.m_ = xip::stat::SModule{};
        return *this;
    }

    bool load_file(const char* path, uint8_t* exec_buf, uint32_t exec_sz,
                   elf_loader::symbol_table* syms) {
        // Strip leading folders or "lib" prefix to find in flatfs
        const char* bn = path;
        if (const char* slash = std::strrchr(path, '/')) {
            bn = slash + 1;
        }
        const fw::FsEntry* e = fw::fs_find(bn);
        if (!e) {
            err_ = module_error::FILE_NOT_FOUND;
            return false;
        }
        if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m_, /*patch_host_got=*/true)) {
            err_ = module_error::LOAD_FAILED;
            return false;
        }
        err_ = module_error::OK;
        return true;
    }

    void* sym(const char* name) const {
        return xip::stat::resolve(m_, name);
    }

    template<typename T> T sym(const char* name) const {
        return (T)(uintptr_t)sym(name);
    }

    module_error last_error() const { return err_; }
    const xip::stat::SModule& get_smodule() const { return m_; }

private:
    xip::stat::SModule m_{};
    module_error err_{module_error::OK};
};

} // namespace scl

#endif // SCL_STATIC_MODULE_H_

#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include "xip/static/nopic.hpp"
#include "fs-archive.hpp"

namespace scl {

enum class module_error {
    OK,
    FILE_NOT_FOUND,
    NOT_AN_ELF,
    NO_DYNSYM,
    LOAD_FAILED
};

/// RAII wrapper over a loaded no-pic module (fixed-address windows).
class Module {
public:
    Module() = default;
    ~Module() {
        if (m_.ok) xip::nopic::unload(m_);
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& o) noexcept : m_(o.m_), err_(o.err_) { o.m_ = xip::nopic::PModule{}; }
    Module& operator=(Module&& o) noexcept {
        if (m_.ok) xip::nopic::unload(m_);
        m_ = o.m_;
        err_ = o.err_;
        o.m_ = xip::nopic::PModule{};
        return *this;
    }

    /// Load @p path (basename looked up in the flat-fs archive).
    bool load_file(const char* path) {
        const char* bn = path;
        if (const char* slash = std::strrchr(path, '/')) {
            bn = slash + 1;
        }
        const fw::FsEntry* e = fw::fs_find(bn);
        if (!e) {
            err_ = module_error::FILE_NOT_FOUND;
            return false;
        }
        if (!xip::nopic::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m_)) {
            err_ = module_error::LOAD_FAILED;
            return false;
        }
        err_ = module_error::OK;
        return true;
    }

    /// Absolute address of an exported symbol (data pointer or function entry).
    void* sym(const char* name) const { return xip::nopic::resolve(m_, name); }

    template<typename T> T sym(const char* name) const { return (T)(uintptr_t)sym(name); }

    module_error last_error() const { return err_; }
    const xip::nopic::PModule& get() const { return m_; }

private:
    xip::nopic::PModule m_{};
    module_error err_{module_error::OK};
};

} // namespace scl

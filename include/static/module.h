#ifndef SCL_STATIC_MODULE_H_
#define SCL_STATIC_MODULE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "static/elf-reader.h"

namespace scl {

enum class module_error {
    OK,
    FILE_NOT_FOUND,
    NOT_AN_ELF,
    NO_DYNSYM,
    LOAD_FAILED
};

/// Symbol visibility of a loaded module (mirrors dlopen's RTLD_LOCAL/RTLD_GLOBAL).
enum class scope {
    GLOBAL,   ///< exports are published to the shared symbol_table (default)
    LOCAL     ///< exports stay private to this module
};

/// Dependency hook: load the dependency named @p soname (the app owns its storage
/// and lifetime, and is responsible for cycle/dedup handling). Return true on
/// success. May be nullptr to disable auto-loading.
using dep_loader = bool (*)(const char* soname);

extern "C" {
/// Overridable reentrancy hooks (weak no-ops by default). An RTOS application
/// defines strong versions to serialize load/unload against calls from other
/// tasks. scl::Module calls scl_loader_lock()/scl_loader_unlock() around every
/// mutating operation.
void scl_loader_lock();
void scl_loader_unlock();
}

/**
 * @brief High-level facade over elf_reader + shlib_loader: load a PIC .so into a
 *        caller-provided RAM buffer, patch its GOT and main's GOT, run its
 *        constructors, and expose its symbols for direct calls.
 *
 * After load_file()/load_buf() the app may call the library's functions directly
 * (build-time linking) or via sym()/operator[] (runtime lookup). unload() runs
 * the library's destructors, restores every word it wrote into main (byte-exact),
 * unpublishes its exports and releases the buffer.
 */
class Module {
public:
    /// Teardown if still loaded (prevents leaking the TLS block on destruction).
    ~Module();

    // --- load -------------------------------------------------------------

    /// Load a .so from a file (semihosting) into @p exec_buf, resolve against
    /// main and @p syms, run constructors, then auto-load any DT_NEEDED deps via
    /// @p load_dep. Returns true on success.
    bool load_file(const char* path, uint8_t* exec_buf, uint32_t exec_sz,
                   elf_loader::symbol_table* syms,
                   scope s = scope::GLOBAL, dep_loader load_dep = nullptr);

    /// Load a .so from an in-memory buffer (no file I/O).
    bool load_buf(const uint8_t* raw, uint32_t rawsz,
                  uint8_t* exec_buf, uint32_t exec_sz,
                  elf_loader::symbol_table* syms,
                  scope s = scope::GLOBAL, dep_loader load_dep = nullptr);

    // --- size query -------------------------------------------------------

    /// Number of bytes load_file() needs for its buffer (highest PT_LOAD end),
    /// or 0 if the file cannot be opened/parsed. Lets the caller size the
    /// exec buffer instead of guessing a fixed 32 KiB.
    static std::uint32_t image_size(const char* path);

    // --- teardown / reference counting -----------------------------------

    /// Add one reference. After load the refcount is 1.
    void retain() { ++refs_; }

    /// Drop one reference; tear the module down when the refcount reaches 0.
    void release() { if (refs_ > 0 && --refs_ == 0) teardown(); }

    /// Convenience: release (tear down at 0). Idempotent.
    void unload() { release(); }

    /// Current reference count.
    uint32_t refcount() const { return refs_; }

    // --- symbol access ----------------------------------------------------

    /// Image-relative symbol value -> runtime address, or nullptr.
    void* sym(const char* name) const;
    template<typename T> T sym(const char* name) const {
        return (T)(uintptr_t)sym(name);
    }

    class Call {
        Module* m_; const char* n_;
    public:
        Call(Module* m, const char* n) : m_(m), n_(n) {}
        template<typename... A> void operator()(A&&... a) const {
            auto fp = (void(*)(A...))m_->sym(n_);
            if (fp) fp((A&&)a...);
        }
    };
    Call operator[](const char* name) { return Call(this, name); }

    // --- status -----------------------------------------------------------

    module_error last_error() const { return err_; }
    /// The underlying loader_error detail (elf_loader::loader_error as int).
    int last_detail() const { return err_detail_; }
    /// Human-readable detail of the last failure ("InvalidSegment", ...).
    const char* last_error_string() const;

    /// True once a library is loaded and usable.
    bool loaded() const { return exec_buf_ != nullptr; }

    /// DT_NEEDED sonames this library declares.
    const std::vector<std::string>& dependencies() const { return deps_; }

    /// Base of the module's TLS block (== thread pointer), or nullptr if none.
    uint8_t* tls_base() const { return tls_base_; }

private:
    bool build(elf_loader::elf_reader& efr,
               uint8_t* buf, uint32_t bufsz,
               elf_loader::symbol_table* syms,
               scope s, dep_loader load_dep);
    void teardown();
    void reset();

    module_error err_{module_error::OK};
    int           err_detail_{0};     // elf_loader::loader_error underlying value
    uint8_t* exec_buf_{nullptr};
    uint32_t exec_sz_{0};
    uint32_t dynsym_off_{0}, dynstr_off_{0}, hash_off_{0}, fini_off_{0};
    uint32_t dynsym_sz_{0},  dynstr_sz_{0},  hash_sz_{0},  fini_sz_{0};
    elf_loader::symbol_table* syms_{nullptr};
    std::vector<std::string> exports_;   // names published into syms_ (for unload)
    std::vector<std::string> deps_;      // DT_NEEDED sonames
    std::vector<elf_loader::patch_record> journal_;  // words written into main (for unload)
    uint8_t* tls_base_{nullptr};
    uint32_t tls_size_{0};
    uint32_t refs_{0};
};

} // namespace scl

#endif // SCL_STATIC_MODULE_H_

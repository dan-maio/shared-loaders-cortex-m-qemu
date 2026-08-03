#ifndef SCL_STATIC_MODULE_H_
#define SCL_STATIC_MODULE_H_

#include <cstdint>

namespace elf_loader { class elf_reader; class symbol_table; }

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
    bool load_file(const char* path, uint8_t* exec_buf, uint32_t exec_sz,
                   elf_loader::symbol_table* syms);
    bool load_buf(const uint8_t* raw, uint32_t rawsz,
                  uint8_t* exec_buf, uint32_t exec_sz,
                  elf_loader::symbol_table* syms);

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

    module_error last_error() const { return err_; }

private:
    bool build(elf_loader::elf_reader& efr,
               uint8_t* buf, uint32_t bufsz,
               elf_loader::symbol_table* syms);
    module_error err_{module_error::OK};
    uint8_t* exec_buf_{nullptr};
    uint32_t exec_sz_{0};
    uint32_t dynsym_off_{0}, dynstr_off_{0};
    uint32_t dynsym_sz_{0}, dynstr_sz_{0};
};

} // namespace scl

#endif // SCL_STATIC_MODULE_H_

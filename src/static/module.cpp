#include "static/module.h"
#include "static/elf-reader.h"
#include "static/shlib-loader.h"
#include "loader/detail/elf-hash.hpp"
#include "loader/elf-types.hpp"
#include <cstring>
#include <new>

namespace scl {

bool Module::load_file(const char* path, uint8_t* buf, uint32_t bufsz,
                       elf_loader::symbol_table* syms) {
    elf_loader::elf_reader efr;
    if (!efr.open_elf(path)) {
        err_ = module_error::FILE_NOT_FOUND;
        err_detail_ = static_cast<int>(efr.error);
        return false;
    }
    return build(efr, buf, bufsz, syms);
}

bool Module::load_buf(const uint8_t* raw, uint32_t rawsz,
                      uint8_t* buf, uint32_t bufsz,
                      elf_loader::symbol_table* syms) {
    elf_loader::elf_reader efr;
    if (!efr.open_buf(raw, rawsz)) {
        err_ = module_error::NOT_AN_ELF;
        err_detail_ = static_cast<int>(efr.error);
        return false;
    }
    return build(efr, buf, bufsz, syms);
}

std::uint32_t Module::image_size(const char* path) {
    elf_loader::elf_reader efr;
    if (!efr.open_elf(path)) return 0;
    return efr.get_binary_size();
}

bool Module::build(elf_loader::elf_reader& efr,
                   uint8_t* buf, uint32_t bufsz,
                   elf_loader::symbol_table* syms) {
    uint32_t sym_sec = efr.find_section_by_name(".dynsym");
    uint32_t str_sec = efr.find_section_by_name(".dynstr");
    if (sym_sec >= 0xFFFF || str_sec >= 0xFFFF) {
        err_ = module_error::NO_DYNSYM;
        return false;
    }

    Elf32_Shdr sym_shdr, str_shdr;
    efr.read_section_header(sym_sec, sym_shdr);
    efr.read_section_header(str_sec, str_shdr);

    // Optional sections: .hash (accelerates sym()) and .fini_array (unload()).
    Elf32_Shdr hash_shdr{}, fini_shdr{};
    const bool have_hash = efr.find_section_by_name(".hash") != 0xFFFFFFFF;
    const bool have_fini = efr.find_section_by_name(".fini_array") != 0xFFFFFFFF;
    if (have_hash) efr.read_section_header(efr.find_section_by_name(".hash"), hash_shdr);
    if (have_fini) efr.read_section_header(efr.find_section_by_name(".fini_array"), fini_shdr);

    elf_loader::shlib_loader shl((elf_loader::elf_reader&&)efr);
    auto load_err = shl.load_bin(buf, bufsz);
    if (load_err != elf_loader::loader_error::OK) {
        err_ = module_error::LOAD_FAILED;
        err_detail_ = static_cast<int>(load_err);
        return false;
    }

    // Full patching: library side + main side (for direct GOT calls).
    elf_loader::symbol_patch* sp_s = nullptr;
    elf_loader::symbol_patch* sp_m = nullptr;
    if (shl.populate_helper_sections() == elf_loader::loader_error::OK) {
        uint32_t ns = shl.get_shlib_patch_count();
        uint32_t nm = shl.get_main_patch_count();
        sp_s = ns ? new elf_loader::symbol_patch[ns] : nullptr;
        sp_m = nm ? new elf_loader::symbol_patch[nm] : nullptr;
        shl.allocate_patch_arrays({sp_s, ns}, {sp_m, nm});
        shl.find_shlib_symbols_to_patch({sp_s, ns});
        shl.find_main_symbols_to_patch({sp_m, nm});
    }

    shl.patch_shlib(syms);
    shl.patch_main(syms);
    delete[] sp_s;   // patch arrays live until patch_* consumed them
    delete[] sp_m;
    shl.execute_constructors();

    exec_buf_ = buf;
    exec_sz_ = bufsz;
    dynsym_off_ = sym_shdr.sh_addr;
    dynstr_off_ = str_shdr.sh_addr;
    dynsym_sz_  = sym_shdr.sh_size;
    dynstr_sz_  = str_shdr.sh_size;
    hash_off_ = have_hash ? hash_shdr.sh_addr : 0;
    hash_sz_  = have_hash ? hash_shdr.sh_size : 0;
    fini_off_ = have_fini ? fini_shdr.sh_addr : 0;
    fini_sz_  = have_fini ? fini_shdr.sh_size : 0;
    syms_ = syms;
    deps_ = shl.dependencies();

    // Record which exports we published so unload() can unpublish them. This
    // matches what patch_shlib() registered: GLOBAL/WEAK defined symbols.
    exports_.clear();
    if (syms_) {
        const char* strtab = reinterpret_cast<const char*>(exec_buf_ + dynstr_off_);
        auto* entries = reinterpret_cast<const Elf32_Sym*>(exec_buf_ + dynsym_off_);
        uint32_t n = dynsym_sz_ / static_cast<uint32_t>(sizeof(Elf32_Sym));
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t bind = ELF32_ST_BIND(entries[i].st_info);
            if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
                entries[i].st_value && entries[i].st_name)
                exports_.emplace_back(strtab + entries[i].st_name);
        }
    }

    err_ = module_error::OK;
    err_detail_ = 0;
    return true;
}

void Module::reset() {
    exec_buf_ = nullptr;
    exec_sz_ = 0;
    dynsym_off_ = dynstr_off_ = hash_off_ = fini_off_ = 0;
    dynsym_sz_ = dynstr_sz_ = hash_sz_ = fini_sz_ = 0;
    syms_ = nullptr;
    exports_.clear();
    deps_.clear();
}

bool Module::unload() {
    if (!exec_buf_) return false;

    // 1. Destructors first: the image and symbol table are still intact.
    if (fini_off_ && fini_sz_) {
        auto* arr = reinterpret_cast<const uint32_t*>(exec_buf_ + fini_off_);
        uint32_t n = fini_sz_ / static_cast<uint32_t>(sizeof(uint32_t));
        for (uint32_t i = n; i-- > 0;) {   // reverse order (C++ dtors)
            uint32_t fn = arr[i];
            if (fn && fn != 0xffffffffu)
                reinterpret_cast<void (*)()>(fn)();
        }
    }

    // 2. Unpublish the exports this module registered.
    if (syms_)
        for (const auto& n : exports_)
            syms_->remove(n);

    // 3. Release the buffer.
    std::memset(exec_buf_, 0, exec_sz_);
    reset();
    err_ = module_error::OK;
    err_detail_ = 0;
    return true;
}

void* Module::sym(const char* name) const {
    if (!exec_buf_ || !dynsym_off_ || !dynstr_off_) return nullptr;

    const char* strtab = reinterpret_cast<const char*>(exec_buf_ + dynstr_off_);
    auto* entries = reinterpret_cast<const Elf32_Sym*>(exec_buf_ + dynsym_off_);
    uint32_t n = dynsym_sz_ / static_cast<uint32_t>(sizeof(Elf32_Sym));

    // Hash-accelerated path (SysV .hash), same semantics as the linear fallback:
    // GLOBAL binding only, defined (st_value != 0), non-empty name.
    if (hash_off_ && hash_sz_ >= 2 * sizeof(uint32_t)) {
        const uint32_t* hdr = reinterpret_cast<const uint32_t*>(exec_buf_ + hash_off_);
        uint32_t nwords = hash_sz_ / static_cast<uint32_t>(sizeof(uint32_t));
        uint32_t nbucket = hdr[0], nchain = hdr[1];
        if (nbucket && nchain && 2 + nbucket + nchain <= nwords) {
            const uint32_t* bucket = hdr + 2;
            const uint32_t* chain  = bucket + nbucket;
            uint32_t idx = bucket[loader::detail::elf_hash(name) % nbucket];
            for (uint32_t steps = 0; idx != 0 && idx < nchain && steps < nchain; ++steps) {
                if (idx < n && entries[idx].st_name && entries[idx].st_value) {
                    if (ELF32_ST_BIND(entries[idx].st_info) == STB_GLOBAL &&
                        std::strcmp(strtab + entries[idx].st_name, name) == 0)
                        return reinterpret_cast<void*>(exec_buf_ + entries[idx].st_value);
                }
                idx = chain[idx];
            }
            return nullptr;   // walked the hash, not found
        }
    }

    // Linear fallback (original behaviour; also used when .hash is absent).
    for (uint32_t i = 0; i < n; ++i) {
        if (ELF32_ST_BIND(entries[i].st_info) == STB_GLOBAL &&
            entries[i].st_value && entries[i].st_name) {
            if (!std::strcmp(strtab + entries[i].st_name, name))
                return reinterpret_cast<void*>(exec_buf_ + entries[i].st_value);
        }
    }
    return nullptr;
}

const char* Module::last_error_string() const {
    return elf_loader::loader_error_string(
        static_cast<elf_loader::loader_error>(err_detail_));
}

} // namespace scl

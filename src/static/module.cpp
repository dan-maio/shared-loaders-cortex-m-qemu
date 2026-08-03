#include "static/module.h"
#include "static/elf-reader.h"
#include "static/shlib-loader.h"
#include "loader/elf-types.hpp"
#include <cstring>
#include <new>
#include <new>

namespace scl {

bool Module::load_file(const char* path, uint8_t* buf, uint32_t bufsz,
                        elf_loader::symbol_table* syms) {
    elf_loader::elf_reader efr;
    if (!efr.open_elf(path)) { err_ = module_error::FILE_NOT_FOUND; return false; }
    return build(efr, buf, bufsz, syms);
}

bool Module::load_buf(const uint8_t* raw, uint32_t rawsz,
                       uint8_t* buf, uint32_t bufsz,
                       elf_loader::symbol_table* syms) {
    elf_loader::elf_reader efr;
    if (!efr.open_buf(raw, rawsz)) { err_ = module_error::NOT_AN_ELF; return false; }
    return build(efr, buf, bufsz, syms);
}

bool Module::build(elf_loader::elf_reader& efr,
                    uint8_t* buf, uint32_t bufsz,
                    elf_loader::symbol_table* syms) {
    uint32_t sym_sec = efr.find_section_by_name(".dynsym");
    uint32_t str_sec = efr.find_section_by_name(".dynstr");
    if (sym_sec >= 0xFFFF || str_sec >= 0xFFFF) {
        err_ = module_error::NO_DYNSYM; return false;
    }

    Elf32_Shdr sym_shdr, str_shdr;
    efr.read_section_header(sym_sec, sym_shdr);
    efr.read_section_header(str_sec, str_shdr);

    elf_loader::shlib_loader shl((elf_loader::elf_reader&&)efr);
    auto load_err = shl.load_bin(buf, bufsz);
    if (load_err != elf_loader::loader_error::OK) {
        err_ = module_error::LOAD_FAILED; return false;
    }

    // Full patching: library side + main side (for direct GOT calls)
    if (shl.populate_helper_sections() == elf_loader::loader_error::OK) {
        uint32_t ns = shl.get_shlib_patch_count();
        uint32_t nm = shl.get_main_patch_count();
        // Use stack buffers for small counts, heap otherwise
        elf_loader::symbol_patch* sp_s = ns ? new elf_loader::symbol_patch[ns] : nullptr;
        elf_loader::symbol_patch* sp_m = nm ? new elf_loader::symbol_patch[nm] : nullptr;
        shl.allocate_patch_arrays({sp_s, ns}, {sp_m, nm});
        shl.find_shlib_symbols_to_patch({sp_s, ns});
        shl.find_main_symbols_to_patch({sp_m, nm});
    }

    shl.patch_shlib(syms);
    shl.patch_main(syms);
    shl.execute_constructors();

    exec_buf_ = buf;
    exec_sz_ = bufsz;
    dynsym_off_ = sym_shdr.sh_addr;
    dynstr_off_ = str_shdr.sh_addr;
    dynsym_sz_  = sym_shdr.sh_size;
    dynstr_sz_  = str_shdr.sh_size;
    err_ = module_error::OK;
    return true;
}

void* Module::sym(const char* name) const {
    if (!exec_buf_ || !dynsym_off_ || !dynstr_off_) return 0;
    const char* strtab = (const char*)(exec_buf_ + dynstr_off_);
    auto* entries = (const Elf32_Sym*)(exec_buf_ + dynsym_off_);
    uint32_t n = dynsym_sz_ / (uint32_t)sizeof(Elf32_Sym);

    for (uint32_t i = 0; i < n; i++) {
        if ((entries[i].st_info >> 4) == 1 && entries[i].st_value && entries[i].st_name) {
            if (!std::strcmp(strtab + entries[i].st_name, name))
                return (void*)(uintptr_t)(exec_buf_ + entries[i].st_value);
        }
    }
    return 0;
}

} // namespace scl

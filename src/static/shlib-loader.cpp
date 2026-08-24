/**
 * @file shlib-loader.cpp
 * @brief Shared-library loader: load, patch, and execute constructors.
 *
 * Core loading pipeline:
 *   1. load_bin()        — copy PT_LOAD segments into a RAM buffer
 *   2. populate_helper_sections() — locate .rel.dyn, .dynsym, .dynstr, .hash, .inits
 *   3. find_*_symbols_to_patch() — build list of symbols needing fixup
 *   4. patch_shlib()     — fix up library GOT entries (R_ARM_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32)
 *   5. patch_main()      — fix up main executable references to library symbols
 *   6. execute_constructors() — run .init_array from the loaded library
 *
 * All diagnostic output uses elf_loader::log for centralised formatting/filtering.
 */

#include "static/shlib-loader.h"
#include "scl/barrier.h"
#include "static/logger.h"
#include "static/tls.h"
#include <cstdio>
#include <cstring>
#include <new>

extern uint32_t _reloc;

namespace elf_loader {

/// Names of the six helper sections needed at runtime.
static const char* helper_sect[] = {
    ".inits",    // helper_section_idx::INITS   = 0
    ".rel.dyn",  // helper_section_idx::REL_DYN = 1
    ".rel.plt",  // helper_section_idx::REL_PLT = 2
    ".dynsym",   // helper_section_idx::DYN_SYM = 3
    ".dynstr",   // helper_section_idx::DYN_STR = 4
    ".hash",     // helper_section_idx::HASH    = 5
};

/**
 * @brief Layout of the _reloc initialisation block emitted by the linker script.
 *
 * The linker script in ldscripts/sections-sh.ld emits this struct into the
 * .inits section so the loader can locate the main executable's dynamic sections.
 */
struct init_lds {
    uint32_t reldyn;       uint32_t reldyn_size;
    uint32_t dynsym;       uint32_t dynsym_size;
    uint32_t dynstr;       uint32_t dynstr_size;
    uint32_t hash;         uint32_t hash_size;
    uint32_t inits;        uint32_t inits_size;
};



// ═══════════════════════════════════════════════════════════════════════════
//  load_bin — copy ELF segments into a RAM buffer
// ═══════════════════════════════════════════════════════════════════════════

loader_error shlib_loader::load_bin(uint8_t* buffer, uint32_t bufsize) {
    efr_.get_header();
    ram_shlib_address_ = buffer;

    uint32_t needed = efr_.get_binary_size();
    if (needed > bufsize) {
        log_error("library needs %u bytes, buffer has %u", needed, bufsize);
        return loader_error::INVALID_SEGMENT;
    }

    log_info("loading %u segments into buffer[%u]",
              unsigned(efr_.get_program_header_count()), unsigned(needed));

    for (uint8_t i = 0; i < efr_.get_program_header_count(); ++i) {
        Elf32_Phdr ph;
        efr_.read_program_header(i, ph);
        auto perm  = elf_reader::flags_to_string(ph.p_flags);
        auto ptype = (ph.p_type == PT_LOAD)     ? "LOAD" :
                     (ph.p_type == PT_DYNAMIC)  ? "DYNAMIC" :
                     (ph.p_type == PT_GNU_STACK) ? "GNU_STACK" : "OTHER";

        log_debug("  [%02u] %s vaddr=%s filesz=%u memsz=%u perm=%s",
                   unsigned(i), ptype, log::hex(ph.p_vaddr),
                   unsigned(ph.p_filesz), unsigned(ph.p_memsz), perm);

        if (ph.p_type == PT_TLS) {
            has_tls_      = true;
            tls_size_     = ph.p_memsz;
            tls_filesz_   = ph.p_filesz;
            tls_data_off_ = ph.p_offset;
        }

        loader_error err = efr_.load_segments({buffer, needed}, ph);
        if (err != loader_error::OK) {
            log_error("segment %u load failed", unsigned(i));
            return err;
        }
    }

    // Scan DT_NEEDED dependencies.
    dependencies_.clear();
    efr_.scan_dependencies(dependencies_);

    return loader_error::OK;
}

loader_error shlib_loader::allocate_tls() {
    if (!has_tls_ || tls_size_ == 0) return loader_error::OK;
    if (tls_offset_) return loader_error::OK;   // already registered

    // Copy the .tdata image out of the file/buffer into a heap template.
    std::vector<uint8_t> tdata(tls_filesz_);
    if (tls_filesz_) {
        if (efr_.read_image_data(tls_data_off_,
                                 {tdata.data(), tls_filesz_}) != loader_error::OK)
            return loader_error::INVALID_SEGMENT;
    }

    uint32_t off = scl_tls_register(tdata.empty() ? nullptr : tdata.data(),
                                    tls_filesz_, tls_size_);
    if (off == UINT32_MAX) {
        log_error("TLS register failed (no slots / OOM)");
        return loader_error::OUT_OF_MEMORY;
    }
    tls_offset_ = off;
    log_info("registered TLS at area offset %s (+%u bytes)",
             log::hex(off), unsigned(tls_size_));
    return loader_error::OK;
}

elf_reader& shlib_loader::get_elf() { return efr_; }
const elf_reader& shlib_loader::get_elf() const { return efr_; }

loader_error shlib_loader::read_elf(std::string_view file) {
    return efr_.open_elf(file) ? loader_error::OK : efr_.error;
}

// ═══════════════════════════════════════════════════════════════════════════
//  populate_helper_sections — locate runtime section descriptors
// ═══════════════════════════════════════════════════════════════════════════

loader_error shlib_loader::populate_helper_sections() {
    // 1. Shared library sections — loaded into RAM, base-relative.
    for (uint32_t k = 0; k < uint32_t(helper_section_idx::COUNT); ++k) {
        helper_section hs;
        efr_.read_helper_sections(helper_sect[k], hs);
        m_shlib[k] = hs;
        if (m_shlib[k].address)
            m_shlib[k].address += reinterpret_cast<uintptr_t>(get_ram_shlib());
    }

    // Fallback: libraries compiled without a custom linker script have
    // .init_array / .fini_array instead of the custom .inits section.
    if (!m_shlib[uint32_t(helper_section_idx::INITS)].address) {
        helper_section hs;
        efr_.read_helper_sections(".init_array", hs);
        if (hs.address) {
            hs.address = hs.address + reinterpret_cast<uintptr_t>(get_ram_shlib());
            m_shlib[uint32_t(helper_section_idx::INITS)] = hs;
        }
    }

    // 2. Main executable sections — located via the _reloc block in FLASH.
    auto* init = reinterpret_cast<const init_lds*>(&_reloc);

    auto set = [this](helper_section_idx idx, uint32_t addr, uint32_t sz, uint32_t es) {
        uint32_t k = uint32_t(idx);
        m_main[k] = {reinterpret_cast<uint8_t*>(uintptr_t(addr)), sz, es, helper_sect[k]};
        if (m_main[k].address)
            m_main[k].address += reinterpret_cast<uintptr_t>(get_ram_main());
    };

    set(helper_section_idx::INITS,  init->inits,  init->inits_size,  sizeof(uint32_t));
    set(helper_section_idx::REL_DYN, init->reldyn, init->reldyn_size, sizeof(Elf32_Rel));
    set(helper_section_idx::REL_PLT, 0, 0, sizeof(Elf32_Rel));
    set(helper_section_idx::DYN_SYM, init->dynsym, init->dynsym_size, sizeof(Elf32_Sym));
    set(helper_section_idx::DYN_STR, init->dynstr, init->dynstr_size, 0);
    set(helper_section_idx::HASH,   init->hash,   init->hash_size,   sizeof(uint32_t));

    if (!m_shlib[uint32_t(helper_section_idx::DYN_SYM)].address)
        log_warn("library has no dynamic symbol info (stripped?)");

    return loader_error::OK;
}

uint8_t* shlib_loader::get_ram_shlib() { return ram_shlib_address_; }
uint8_t* shlib_loader::get_ram_main()  { return ram_main_address_; }

// ═══════════════════════════════════════════════════════════════════════════
//  Patch-count helpers
// ═══════════════════════════════════════════════════════════════════════════

uint32_t shlib_loader::get_shlib_patch_count() {
    auto ri = uint32_t(helper_section_idx::REL_DYN);
    auto si = uint32_t(helper_section_idx::DYN_SYM);
    auto st = uint32_t(helper_section_idx::DYN_STR);
    auto hi = uint32_t(helper_section_idx::HASH);

    pass_args pa {
        m_shlib[ri].address, m_shlib[ri].size, m_shlib[ri].entry_size,
        m_shlib[si].address, m_shlib[si].size, m_shlib[si].entry_size,
        m_shlib[st].address, m_shlib[st].size,
        m_shlib[hi].address, m_shlib[hi].size,
    };
    uint32_t count = efr_.calculate_patch_array_size(pa);

    // Add .rel.plt entries
    auto pi = uint32_t(helper_section_idx::REL_PLT);
    if (m_shlib[pi].address && m_shlib[pi].entry_size)
        count += m_shlib[pi].size / m_shlib[pi].entry_size;

    return sh_patch_nbr = count;
}

uint32_t shlib_loader::get_main_patch_count() {
    pass_args pa {
        m_main[uint32_t(helper_section_idx::REL_DYN)].address,
        m_main[uint32_t(helper_section_idx::REL_DYN)].size,
        m_main[uint32_t(helper_section_idx::REL_DYN)].entry_size,
        m_main[uint32_t(helper_section_idx::DYN_SYM)].address,
        m_main[uint32_t(helper_section_idx::DYN_SYM)].size,
        m_main[uint32_t(helper_section_idx::DYN_SYM)].entry_size,
        m_main[uint32_t(helper_section_idx::DYN_STR)].address,
        m_main[uint32_t(helper_section_idx::DYN_STR)].size,
        m_main[uint32_t(helper_section_idx::HASH)].address,
        m_main[uint32_t(helper_section_idx::HASH)].size,
    };
    return main_patch_nbr = efr_.calculate_patch_array_size(pa);
}

// ═══════════════════════════════════════════════════════════════════════════
//  find_*_symbols_to_patch — build the patch arrays
// ═══════════════════════════════════════════════════════════════════════════

loader_error shlib_loader::find_shlib_symbols_to_patch(std::span<symbol_patch> ssp) {
    auto ri = uint32_t(helper_section_idx::REL_DYN);
    auto si = uint32_t(helper_section_idx::DYN_SYM);
    auto st = uint32_t(helper_section_idx::DYN_STR);
    auto hi = uint32_t(helper_section_idx::HASH);

    pass_args pa {
        m_shlib[ri].address, m_shlib[ri].size, m_shlib[ri].entry_size,
        m_shlib[si].address, m_shlib[si].size, m_shlib[si].entry_size,
        m_shlib[st].address, m_shlib[st].size,
        m_shlib[hi].address, m_shlib[hi].size,
    };

    uint32_t rc = 0, dc = 0;
    loader_error e = efr_.read_relocation_symbols(pa, ssp, rc);
    if (e != loader_error::OK) return e;

    // Also process .rel.plt entries (JUMP_SLOT) that follow after .rel.dyn.
    auto pi = uint32_t(helper_section_idx::REL_PLT);
    if (m_shlib[pi].address) {
        pass_args pa_plt {
            m_shlib[pi].address, m_shlib[pi].size, m_shlib[pi].entry_size,
            m_shlib[si].address, m_shlib[si].size, m_shlib[si].entry_size,
            m_shlib[st].address, m_shlib[st].size,
            m_shlib[hi].address, m_shlib[hi].size,
        };
        uint32_t rc2 = 0;
        e = efr_.read_relocation_symbols(pa_plt, ssp.subspan(rc), rc2);
        if (e != loader_error::OK) return e;
        rc += rc2;
    }

    efr_.read_dynamic_symbols(pa, ssp, rc, dc);
    sh_patch_nbr = rc + dc;
    return loader_error::OK;
}

loader_error shlib_loader::find_main_symbols_to_patch(std::span<symbol_patch> msp) {
    auto ri = uint32_t(helper_section_idx::REL_DYN);
    auto si = uint32_t(helper_section_idx::DYN_SYM);
    auto st = uint32_t(helper_section_idx::DYN_STR);
    auto hi = uint32_t(helper_section_idx::HASH);

    pass_args pa {
        m_main[ri].address, m_main[ri].size, m_main[ri].entry_size,
        m_main[si].address, m_main[si].size, m_main[si].entry_size,
        m_main[st].address, m_main[st].size,
        m_main[hi].address, m_main[hi].size,
    };

    uint32_t rc = 0, dc = 0;
    loader_error e = efr_.read_relocation_symbols(pa, msp, rc);
    if (e != loader_error::OK) return e;
    efr_.read_dynamic_symbols(pa, msp, rc, dc);
    main_patch_nbr = rc + dc;
    return loader_error::OK;
}

symbol_patch shlib_loader::find_symbol_by_name(lib_type type, std::string_view name) {
    uint32_t n  = (type == lib_type::SHLIB) ? sh_patch_nbr : main_patch_nbr;
    auto*    sp = (type == lib_type::SHLIB) ? sh_sp : main_sp;
    for (uint32_t i = 0; i < n; ++i)
        if (sp[i].name == name) return sp[i];
    return {};
}

void shlib_loader::allocate_patch_arrays(std::span<symbol_patch> s, std::span<symbol_patch> m) {
    sh_sp   = s.data();
    main_sp = m.data();
}

// ═══════════════════════════════════════════════════════════════════════════
//  resolve_dependencies — DT_NEEDED auto-loader (callback-based)
// ═══════════════════════════════════════════════════════════════════════════

loader_error shlib_loader::resolve_dependencies(symbol_table* syms,
                                               loader_error (*load_dep)(std::string_view, shlib_loader&)) {
    if (!load_dep) return loader_error::OK;
    for (const auto& dep : dependencies_) {
        if (syms && syms->has(dep)) {
            log_info("  dep %s already loaded (via sym table)", dep.c_str());
            continue;
        }
        // Keep the dependency's loader alive: it owns the parsed ELF and the
        // helper-section maps that later lookups rely on. A local temporary
        // would drop them the moment this iteration ends.
        auto holder = std::make_unique<shlib_loader>();
        loader_error e = load_dep(dep, *holder);
        if (e != loader_error::OK) {
            log_error("failed to load dependency: %s", dep.c_str());
            return loader_error::DEP_NOT_FOUND;
        }
        loaded_deps_.push_back(std::move(holder));
        log_info("auto-loaded dependency: %s", dep.c_str());
    }
    return loader_error::OK;
}

// ═══════════════════════════════════════════════════════════════════════════
//  patch_shlib — fix up the library's own GOT
// ═══════════════════════════════════════════════════════════════════════════

uint32_t shlib_loader::patch_shlib(symbol_table* global_syms) {
    log_info("PATCH in LIBs");
    auto* buffer = get_ram_shlib();
    const uint32_t image_size = efr_.get_binary_size();
    uint32_t patched = 0, skipped = 0;

    // A relocation offset must land a full 32-bit word inside the loaded image.
    // .rel.dyn comes from the (untrusted) library, so every write is bounded.
    auto in_image = [&](uint32_t off) {
        return uint64_t(off) + sizeof(uint32_t) <= image_size;
    };

    for (uint32_t j = 0; j < sh_patch_nbr; ++j) {
        const auto& s = sh_sp[j];

        const uint32_t go = s.got_address;

        // ── R_ARM_RELATIVE — add the image base to the in-place value ──────
        // Must be checked before the empty-name guard: R_ARM_RELATIVE
        // relocations have sym_idx=0 (STN_UNDEF) so name is always empty.
        if (s.type == R_ARM_RELATIVE) {
            if (!in_image(go)) {
                log_warn("  %02u REL offset %s outside image", j, log::hex(go));
                ++skipped; continue;
            }
            auto* addr = reinterpret_cast<uint32_t*>(&buffer[go]);
            *addr += uintptr_t(buffer);
            log_info("  %02u REL    %s %s",
                      j, log::hex(uintptr_t(addr)), log::hex(*addr));
            ++patched;
            continue;
        }

        // Registration-only entries come from the dynamic symbol table, not a
        // relocation, and carry type R_ARM_NONE with got_address 0. They exist
        // solely to be published for interposition / weak dispatch — they have
        // NO GOT slot, so they must never write into the image (writing at
        // got_address 0 would clobber offset 0 of the loaded library).
        const bool has_slot = (s.type != R_ARM_NONE);

        if (s.name.empty()) { ++skipped; continue; }

        // ── TLS (thread-local) relocations — multi-thread area model ─────────
        // st_value is the symbol's offset within its module's TLS block; the GOT
        // slot carries the addend. The thread pointer points at the per-thread
        // area base, so the absolute offset is tls_offset + st_value.
        if (s.type == R_ARM_TLS_TPOFF32 ||
            s.type == R_ARM_TLS_DTPOFF32 ||
            s.type == R_ARM_TLS_DTPMOD32) {
            if (!in_image(go)) {
                log_warn("  %02u TLS offset %s outside image", j, log::hex(go));
                ++skipped; continue;
            }
            auto* slot = reinterpret_cast<uint32_t*>(&buffer[go]);
            if (s.type == R_ARM_TLS_DTPMOD32) {
                *slot = 0;                          // module id (GD not supported)
            } else {
                *slot += tls_offset_ + s.virtual_address;   // DTPOFF32 / TPOFF32
            }
            log_info("  %02u TLS    %s -> %s  %.*s",
                      j, log::hex(uintptr_t(slot)), log::hex(*slot),
                      int(s.name.size()), s.name.data());
            ++patched;
            continue;
        }

        // ── R_ARM_ABS32 — absolute pointer (e.g. .init_array entries) ──────
        // Handled for any binding (a local ABS32 was previously dropped).
        if (s.type == R_ARM_ABS32) {
            uint32_t val = 0;
            if (s.virtual_address) {
                val = s.virtual_address + uintptr_t(buffer);   // self-defined
            } else {
                if (global_syms) val = global_syms->find(s.name);
                if (!val)        val = efr_.find_symbol_address(s.name, m_main);
                if (!val) {
                    log_warn("  %02u ABS32 %.*s NOT FOUND", j,
                              int(s.name.size()), s.name.data());
                    ++skipped; continue;
                }
            }
            if (!in_image(go)) {
                log_warn("  %02u ABS32 offset %s outside image", j, log::hex(go));
                ++skipped; continue;
            }
            auto* addr = reinterpret_cast<uint32_t*>(&buffer[go]);
            *addr = val;
            log_info("  %02u ABS32  %s %s -> %s  %.*s",
                      j, log::binding_str(s.binding),
                      log::hex(uintptr_t(addr)), log::hex(*addr),
                      int(s.name.size()), s.name.data());
            ++patched;
            continue;
        }

        // ── STB_GLOBAL / STB_WEAK — GLOB_DAT / JUMP_SLOT slots ─────────────
        uint32_t val = 0;
        bool resolved = false;
        if (s.binding == STB_GLOBAL) {
            if (s.virtual_address == 0) {
                // External: look up in the global table, then main's symbols.
                if (global_syms) val = global_syms->find(s.name);
                if (!val)        val = efr_.find_symbol_address(s.name, m_main);
                if (!val)
                    log_warn("  %02u external %.*s NOT FOUND", j,
                              int(s.name.size()), s.name.data());
                else resolved = true;
            } else {
                val = s.virtual_address + uintptr_t(buffer);   // self-defined
                resolved = true;
            }
        } else if (s.binding == STB_WEAK) {
            if (global_syms) val = global_syms->find(s.name);
            if (!val)        val = efr_.find_symbol_address(s.name, m_main);
            if (!val && s.virtual_address)
                val = s.virtual_address + uintptr_t(buffer);
            resolved = true;   // an undefined weak legally resolves to 0
        } else {
            log_warn("  %02u unknown bind=%d  %.*s", j, s.binding,
                      int(s.name.size()), s.name.data());
        }

        // Apply the GOT slot only for real relocations (has_slot) whose offset
        // is inside the image; registration-only entries fall straight through.
        if (resolved && has_slot) {
            if (in_image(go)) {
                auto* addr = reinterpret_cast<uint32_t*>(&buffer[go]);
                *addr = val;
                log_info("  %02u %-6s %s %s %s  %.*s", j,
                          s.binding == STB_WEAK ? "WEAK" : "GLOBAL",
                          log::binding_str(s.binding),
                          log::hex(uintptr_t(addr)), log::hex(val),
                          int(s.name.size()), s.name.data());
                ++patched;
            } else {
                log_warn("  %02u %.*s offset %s outside image", j,
                          int(s.name.size()), s.name.data(), log::hex(go));
                ++skipped;
            }
        } else if (!resolved) {
            ++skipped;
        }

        // Register exported (self-defined) symbols for cross-module interposition.
        if (publish_ && global_syms && (s.binding == STB_GLOBAL || s.binding == STB_WEAK) && s.virtual_address) {
            uint32_t ra = s.virtual_address + uintptr_t(buffer);
            if (s.binding == STB_WEAK)
                global_syms->add_weak(s.name, ra);
            else
                global_syms->set_global(s.name, ra);
        }
    }

    // If no exports were added from .rel.dyn (e.g. simple .so without relocations),
    // scan .dynsym directly to register exported symbols.
    if (publish_ && global_syms && patched == 0) {
        auto si = uint32_t(helper_section_idx::DYN_SYM);
        auto st = uint32_t(helper_section_idx::DYN_STR);
        auto* sym_data = m_shlib[si].address;
        uint32_t sym_size = m_shlib[si].size;
        uint32_t sym_count = sym_size / sizeof(Elf32_Sym);
        auto* str_data = m_shlib[st].address;

        for (uint32_t i = 1; i < sym_count; ++i) {
            auto* sym = reinterpret_cast<Elf32_Sym*>(sym_data) + i;
            uint8_t bind = ELF32_ST_BIND(sym->st_info);
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && sym->st_value) {
                const char* name = reinterpret_cast<const char*>(str_data + sym->st_name);
                uint32_t ra = sym->st_value + uintptr_t(buffer);
                if (bind == STB_WEAK)
                    global_syms->add_weak(name, ra);
                else
                    global_syms->set_global(name, ra);
            }
        }
    }

    scl_memory_barrier();
    log_info("patched %u shlib symbols (%u skipped)", patched, skipped);
    return patched;
}

// ═══════════════════════════════════════════════════════════════════════════
//  patch_main — fix up the main executable's references to library symbols
// ═══════════════════════════════════════════════════════════════════════════

uint32_t shlib_loader::patch_main(symbol_table* global_syms) {
    log_info("PATCH in MAIN");
    auto* buffer = get_ram_shlib();
    uint32_t patched = 0, skipped = 0;

    // Lambda: resolve address from global_syms (runtime addr) or from the
    // library's raw symbol table (adds buffer for raw offsets).
    auto resolve = [&](std::string_view name) -> uint32_t {
        uint32_t a = 0;
        if (global_syms) a = global_syms->find(name);
        if (!a) {
            a = efr_.find_symbol_address(name, m_shlib);
            if (a) a += uintptr_t(buffer);
        }
        return a;
    };

    // main's .rel.dyn r_offset entries are absolute addresses in the (trusted)
    // firmware image; still refuse a null or misaligned slot before writing.
    auto valid_slot = [](uint32_t a) { return a != 0 && (a & 3u) == 0; };

    // Snapshot a word before it is overwritten, so unload can restore it.
    auto journal = [&](uint32_t addr) {
        main_journal_.push_back(
            {addr, *reinterpret_cast<const uint32_t*>(addr)});
    };

    for (uint32_t j = 0; j < main_patch_nbr; ++j) {
        const auto& m = main_sp[j];
        if (m.name.empty()) { ++skipped; continue; }

        // ── Non-weak symbols ──────────────────────────────────────────
        if (m.binding != STB_WEAK) {
            if (m.type == R_ARM_COPY) {
                // NOTE: this is NOT standard R_ARM_COPY data-copy semantics
                // (which would memcpy st_size bytes of the library's variable
                // into main's BSS). It is a deliberate address-injection: the
                // library symbol's runtime address is stored into main's slot,
                // so main dereferences the library's own storage. The demo in
                // main.cpp depends on this (`*(uint32_t*)aa = 20`). st_size is
                // intentionally unused.
                uint32_t val = resolve(m.name);
                if (val && valid_slot(m.got_address)) {
                    auto* slot = reinterpret_cast<uint32_t*>(m.got_address);
                    journal(m.got_address);
                    *slot = val;
                    if (publish_ && global_syms) global_syms->set_global(m.name, val);
                    log_info("  %02u R_ARM_COPY %s -> %s  %.*s",
                              j, log::hex(m.got_address), log::hex(val),
                              int(m.name.size()), m.name.data());
                    ++patched;
                } else if (!val) {
                    log_warn("  %02u R_ARM_COPY %.*s NOT FOUND", j,
                              int(m.name.size()), m.name.data());
                    ++skipped;
                } else {
                    log_warn("  %02u R_ARM_COPY %.*s bad slot %s", j,
                              int(m.name.size()), m.name.data(),
                              log::hex(m.got_address));
                    ++skipped;
                }
            } else {
                // Standard global symbol: write resolved address to GOT slot.
                uint32_t val = resolve(m.name);
                if (val && valid_slot(m.got_address)) {
                    auto* slot = reinterpret_cast<uint32_t*>(m.got_address);
                    journal(m.got_address);
                    *slot = val;
                    if (publish_ && global_syms) global_syms->set_global(m.name, val);
                    log_info("  %02u GLOBAL %s -> %s  %.*s",
                              j, log::hex(m.got_address), log::hex(val),
                              int(m.name.size()), m.name.data());
                    ++patched;
                } else if (!val) {
                    log_warn("  %02u global %.*s NOT FOUND", j,
                              int(m.name.size()), m.name.data());
                    ++skipped;
                } else {
                    ++skipped;
                }
            }
        }
        // ── Weak symbols ───────────────────────────────────────────────
        else {
            uint32_t val = resolve(m.name);
            if (val && valid_slot(m.got_address)) {
                auto* slot = reinterpret_cast<uint32_t*>(m.got_address);
                journal(m.got_address);
                *slot = val;
                if (global_syms) global_syms->set_global(m.name, val);
                log_info("  %02u WEAK   %s -> %s  %.*s",
                          j, log::hex(m.got_address), log::hex(val),
                          int(m.name.size()), m.name.data());
                ++patched;
            } else if (val) {
                // No GOT slot — cache for late resolution.
                weak_sym_cache_.push_back({m.name, val, m.virtual_address});
                log_debug("  %02u WEAK cached %s %s  %.*s",
                           j, log::hex(val), log::hex(m.virtual_address),
                           int(m.name.size()), m.name.data());
                ++skipped;
            } else {
                log_info("  %02u WEAK   %.*s — using main's default", j,
                          int(m.name.size()), m.name.data());
                ++skipped;
            }
        }
    }

    scl_memory_barrier();
    log_info("patched %u main symbols (%u skipped)", patched, skipped);
    return patched;
}

// ═══════════════════════════════════════════════════════════════════════════
//  execute_constructors — run .init_array from the loaded library
// ═══════════════════════════════════════════════════════════════════════════

loader_error shlib_loader::execute_constructors() {
    auto ii = uint32_t(helper_section_idx::INITS);
    auto* addr = reinterpret_cast<uint32_t*>(m_shlib[ii].address);
    auto  size = m_shlib[ii].size;

    if (!addr || !size) {
        log_info("no constructors in this library");
        return loader_error::OK;
    }

    log_info("running constructors (%u bytes):", size);
    for (uint32_t i = 0; i < size; i += sizeof(uint32_t)) {
        auto pc = reinterpret_cast<pctor>(addr[i / sizeof(uint32_t)]);
        if (pc) {
            log_debug("  calling constructor at %s", log::hex(uintptr_t(pc)));
            pc();
        }
    }
    return loader_error::OK;
}

// ═══════════════════════════════════════════════════════════════════════════
//  resolve_weak_function — lazy weak-function dispatch
// ═══════════════════════════════════════════════════════════════════════════

unsigned int shlib_loader::resolve_weak_function(funcm f) {
    uint32_t ad = reinterpret_cast<uint32_t>(f);
    for (const auto& wm : weak_sym_cache_)
        if (ad == wm.old_addr)
            return reinterpret_cast<funcm>(wm.addr)();
    return f();
}

} // namespace elf_loader

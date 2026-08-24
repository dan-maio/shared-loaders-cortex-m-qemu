/**
 * @file elf-reader.cpp
 * @brief ELF32 file parser and symbol-table manager for the bare-metal loader.
 *
 * Provides file-level reading of ELF headers, program headers, section headers,
 * string tables, symbol tables, hash tables and relocation entries.  The public
 * API is split between elf_reader (file I/O) and symbol_table (runtime registry).
 *
 * All diagnostic output goes through elf_loader::log so that format and
 * filtering are centralised.
 */

#include "static/elf-reader.h"
#include "loader/detail/bytes.hpp"
#include "loader/detail/elf-hash.hpp"
#include "static/logger.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace elf_loader {

// ═══════════════════════════════════════════════════════════════════════════
//  symbol_table — cross-library symbol registry with first-loaded-wins
// ═══════════════════════════════════════════════════════════════════════════

/// Initial capacity of the symbol table (grows by doubling).
constexpr uint32_t ST_INIT_CAPACITY = 64;

void symbol_table::ensure_capacity() {
    if (!entries_) {
        capacity_ = ST_INIT_CAPACITY;
        entries_  = new Entry[capacity_];
    } else if (count_ >= capacity_) {
        uint32_t new_cap = capacity_ * 2;
        auto* new_entries = new Entry[new_cap];
        for (uint32_t i = 0; i < count_; ++i)
            new_entries[i] = entries_[i];
        delete[] entries_;
        entries_  = new_entries;
        capacity_ = new_cap;
    }
}

void symbol_table::add_global(std::string_view name, uint32_t address) {
    if (name.empty() || address == 0) return;
    // First-loaded-wins: silently ignore if a non-weak entry already exists.
    for (uint32_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name && !entries_[i].is_weak)
            return;
    }
    ensure_capacity();
    entries_[count_] = {name, address, false};
    ++count_;
}

void symbol_table::set_global(std::string_view name, uint32_t address) {
    if (name.empty() || address == 0) return;
    // Last-loaded-wins: update existing entry or append.
    for (uint32_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name && !entries_[i].is_weak) {
            entries_[i].address = address;
            return;
        }
    }
    ensure_capacity();
    entries_[count_] = {name, address, false};
    ++count_;
}

void symbol_table::add_weak(std::string_view name, uint32_t address) {
    if (name.empty() || address == 0) return;
    // Weak entries are appended regardless; they are searched last.
    ensure_capacity();
    entries_[count_] = {name, address, true};
    ++count_;
}

uint32_t symbol_table::find(std::string_view name) const {
    // Search non-weak entries first (first-loaded-wins).
    for (uint32_t i = 0; i < count_; ++i)
        if (entries_[i].name == name && !entries_[i].is_weak)
            return entries_[i].address;
    // Fall back to weak entries.
    for (uint32_t i = 0; i < count_; ++i)
        if (entries_[i].name == name)
            return entries_[i].address;
    return 0;
}

bool symbol_table::has(std::string_view name) const {
    for (uint32_t i = 0; i < count_; ++i)
        if (entries_[i].name == name && !entries_[i].is_weak)
            return true;
    return false;
}

bool symbol_table::remove(std::string_view name) {
    bool removed = false;
    for (uint32_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name) {
            // Shift the tail down; keep the array compact.
            for (uint32_t j = i; j + 1 < count_; ++j)
                entries_[j] = entries_[j + 1];
            --count_;
            --i;              // re-check the slot that just slid down
            removed = true;
        }
    }
    return removed;
}

void symbol_table::clear() {
    delete[] entries_;
    entries_  = nullptr;
    count_    = 0;
    capacity_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ELF hash function (SysV ABI)
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
//  elf_reader — file I/O and ELF parsing
// ═══════════════════════════════════════════════════════════════════════════

/// Sentinel returned by find_section_by_name() when a section is absent.
/// 0 is a valid index (the null section), so it cannot double as "not found".
constexpr uint32_t SECTION_NOT_FOUND = 0xFFFFFFFF;

// The public API exposes the <elf.h> Elf32_* structs, while the shared parser
// speaks loader::elf::*. Both mirror the ELF32 ABI layout exactly, so the
// adapter methods below copy across with memcpy — guarded by these asserts.
static_assert(sizeof(Elf32_Ehdr) == sizeof(loader::elf::Ehdr), "Ehdr layout mismatch");
static_assert(sizeof(Elf32_Phdr) == sizeof(loader::elf::Phdr), "Phdr layout mismatch");
static_assert(sizeof(Elf32_Shdr) == sizeof(loader::elf::Shdr), "Shdr layout mismatch");

void elf_reader::close_file() {
    img_.reset();
    src_.reset();
    elf_size_ = 0;
}

bool elf_reader::open_elf(std::string_view file_path) {
    close_file();

    char path_buf[256];
    if (file_path.size() >= sizeof(path_buf)) {
        log_error("file path too long (%zu bytes)", file_path.size());
        error = loader_error::FILE_NOT_FOUND;
        return false;
    }
    memcpy(path_buf, file_path.data(), file_path.size());
    path_buf[file_path.size()] = '\0';

    file_name_ = file_path;

    auto opened = loader::fd_byte_source::open_file(path_buf);
    if (!opened) {
        log_error("cannot open '%s'", path_buf);
        error = loader_error::FILE_NOT_FOUND;
        return false;
    }
    src_.reset(new loader::fd_byte_source(std::move(*opened)));

    error = loader_error::OK;
    verify_elf();
    return error == loader_error::OK;
}

bool elf_reader::open_buf(const uint8_t* data, uint32_t size) {
    close_file();
    if (!data || !size) {
        error = loader_error::FILE_NOT_FOUND;
        return false;
    }
    src_.reset(new loader::memory_byte_source(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size)));
    error = loader_error::OK;
    verify_elf();
    return error == loader_error::OK;
}

loader_error elf_reader::verify_elf() {
    if (error != loader_error::OK) return error;
    if (!src_) { error = loader_error::FILE_NOT_FOUND; return error; }

    // elf_image::parse validates magic, ELFCLASS32, ELFDATA2LSB, EM_ARM and
    // ET_DYN in one shot; map its error onto our loader_error vocabulary.
    auto parsed = loader::elf_image::parse(*src_);
    if (!parsed) {
        switch (parsed.err()) {
            case loader::error::SHORT_READ:
                log_error("short read on ELF header"); break;
            case loader::error::NOT_ELF:
                log_error("bad ELF magic"); break;
            case loader::error::NOT_ARM32:
                log_error("not a little-endian 32-bit ARM object"); break;
            case loader::error::NOT_SHARED_OBJECT:
                log_error("not a shared library (ET_DYN)"); break;
            default:
                log_error("ELF parse failed"); break;
        }
        error = loader_error::NOT_AN_ELF;
        return error;
    }
    img_.emplace(std::move(*parsed));
    std::memcpy(&elf_header_, &img_->header(), sizeof(elf_header_));
    error = loader_error::OK;
    return error;
}

const Elf32_Ehdr& elf_reader::get_header() {
    // Populated by verify_elf() from the parsed image; no I/O needed here.
    if (img_) std::memcpy(&elf_header_, &img_->header(), sizeof(elf_header_));
    return elf_header_;
}

void elf_reader::read_program_header(uint32_t nbr, Elf32_Phdr& phdr) {
    loader::elf::Phdr p{};
    if (img_ && img_->program_header(static_cast<loader::elf::half>(nbr), p)) {
        std::memcpy(&phdr, &p, sizeof(phdr));
    } else {
        log_error("cannot read program header %u", nbr);
        error = loader_error::NOT_AN_ELF;
        phdr = {};
    }
}

void elf_reader::read_section_header(uint32_t nbr, Elf32_Shdr& shdr) {
    loader::elf::Shdr s{};
    if (img_ && img_->section_header(static_cast<loader::elf::half>(nbr), s)) {
        std::memcpy(&shdr, &s, sizeof(shdr));
    } else {
        log_error("cannot read section header %u", nbr);
        error = loader_error::NOT_AN_ELF;
        shdr = {};
    }
}

loader_error elf_reader::read_section_content(uint32_t nbr, std::span<uint8_t> dest) {
    Elf32_Shdr shdr;
    read_section_header(nbr, shdr);
    uint32_t n = std::min(shdr.sh_size, static_cast<uint32_t>(dest.size()));
    if (n) {
        read_at_offset({dest.data(), n}, shdr.sh_offset);
        if (error != loader_error::OK) {
            log_error("short read on section %u content", nbr);
            return loader_error::SECTION_NOT_FOUND;
        }
    }
    return loader_error::OK;
}

void elf_reader::read_at_offset(std::span<uint8_t> dest, uint32_t offset) {
    if (dest.empty()) return;
    if (!src_ || !src_->read_at(offset, std::as_writable_bytes(dest))) {
        log_error("short read at offset %u (%zu bytes)", offset, dest.size());
        error = loader_error::INVALID_SEGMENT;
    }
}

uint32_t elf_reader::get_binary_size() {
    if (elf_size_) return elf_size_;
    if (!img_) return 0;
    auto sz = img_->image_size();
    elf_size_ = sz ? *sz : 0;
    return elf_size_;
}

uint32_t elf_reader::find_section_by_name(std::string_view name) {
    if (!img_) return SECTION_NOT_FOUND;

    char name_buf[64];
    if (name.size() >= sizeof(name_buf)) return SECTION_NOT_FOUND;
    memcpy(name_buf, name.data(), name.size());
    name_buf[name.size()] = '\0';

    Elf32_Shdr shst;
    read_section_header(elf_header_.e_shstrndx, shst);
    auto tab = std::make_unique<char[]>(shst.sh_size);
    error = loader_error::OK;
    read_at_offset({reinterpret_cast<uint8_t*>(tab.get()), shst.sh_size}, shst.sh_offset);
    if (error != loader_error::OK) return SECTION_NOT_FOUND;

    Elf32_Shdr sh;
    for (uint32_t i = 0; i < elf_header_.e_shnum; ++i) {
        read_section_header(i, sh);
        if (sh.sh_name < shst.sh_size && strcmp(name_buf, tab.get() + sh.sh_name) == 0)
            return i;
    }
    return SECTION_NOT_FOUND;
}

uint32_t elf_reader::find_program(uint16_t type) {
    Elf32_Phdr ph;
    for (uint32_t i = 0; i < elf_header_.e_phnum; ++i) {
        read_program_header(i, ph);
        if (ph.p_type == type) return i;
    }
    return 0xFFFFFFFF;
}

uint32_t elf_reader::get_program_header_count() const {
    return elf_header_.e_phnum;
}

void elf_reader::get_segment_info(uint32_t nbr, load_segment& seg) {
    Elf32_Phdr ph;
    read_program_header(nbr, ph);
    seg.type   = ph.p_type;
    seg.vaddr  = ph.p_vaddr;
    seg.memsz  = ph.p_memsz;
    seg.filesz = ph.p_filesz;
    seg.offset = ph.p_offset;
    seg.flags  = ph.p_flags;
    seg.align  = ph.p_align;
}

const char* elf_reader::flags_to_string(uint32_t flags) {
    static char buf[4];
    buf[0] = (flags & PF_R) ? 'R' : '-';
    buf[1] = (flags & PF_W) ? 'W' : '-';
    buf[2] = (flags & PF_X) ? 'X' : '-';
    buf[3] = '\0';
    return buf;
}

std::string_view elf_reader::get_elf_name() const { return file_name_; }

loader_error elf_reader::load_segments(std::span<uint8_t> ram, const Elf32_Phdr& ph) {
    if (ph.p_type != PT_LOAD) return loader_error::OK;

    if (ph.p_vaddr + ph.p_memsz > ram.size()) {
        log_error("LOAD segment vaddr=%s memsz=%u exceeds buffer (%zu)",
                   log::hex(ph.p_vaddr), ph.p_memsz, ram.size());
        return loader_error::INVALID_SEGMENT;
    }
    read_at_offset({ram.data() + ph.p_vaddr, ph.p_filesz}, ph.p_offset);
    if (ph.p_memsz > ph.p_filesz)
        memset(ram.data() + ph.p_vaddr + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
    return loader_error::OK;
}

void elf_reader::read_helper_sections(const char* name, helper_section& hs) {
    uint32_t idx = find_section_by_name(name);
    if (idx != SECTION_NOT_FOUND) {
        Elf32_Shdr sh;
        read_section_header(idx, sh);
        hs = {reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(sh.sh_addr)),
              sh.sh_size, sh.sh_entsize, name};
    } else {
        hs = {nullptr, 0, 0, name};
    }
}

// ── Relocation / symbol helpers ─────────────────────────────────────────

/// Bounds-checked NUL-terminated string read from a string table.
/// Thin wrapper over the shared bounds-checked string reader
/// (loader::detail::name_at). Adapts this file's (ptr, size) callers to the
/// span-based helper; a null table yields an empty view. The actual "never scan
/// past the table" logic lives once, in common-elf-reader.
static std::string_view name_at(const char* tab, uint32_t size, uint32_t off) noexcept {
    if (!tab) return {};
    return loader::detail::name_at(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(tab), size}, off);
}

loader_error elf_reader::read_relocation_symbols(const pass_args& pa,
                                                std::span<symbol_patch> ssp,
                                                uint32_t& count_out) {
    uint32_t k = 0;
    uint32_t num_rels = pa.reldyn_size / pa.reldyn_entry_size;
    uint32_t num_syms = pa.dynsym_size / pa.dynsym_entry_size;

    if (!pa.reldyn_start || !pa.dynsym_start || !pa.dynstr_start) {
        log_error("missing relocation/dynsym/dynstr data");
        return loader_error::INVALID_RELOCATION;
    }

    auto* rels   = reinterpret_cast<const Elf32_Rel*>(pa.reldyn_start);
    auto* syms   = reinterpret_cast<const Elf32_Sym*>(pa.dynsym_start);
    auto* dynstr = reinterpret_cast<const char*>(pa.dynstr_start);

    for (uint32_t i = 0; i < num_rels; ++i) {
        if (k >= ssp.size()) { log_warn("patch array full at reloc %u", i); break; }

        uint32_t sym_idx = ELF32_R_SYM(rels[i].r_info);
        if (sym_idx >= num_syms) {
            log_warn("reloc %u sym idx %u out of range", i, sym_idx);
            continue;
        }

        const Elf32_Sym& sym = syms[sym_idx];

        ssp[k].got_address      = rels[i].r_offset;
        ssp[k].type             = ELF32_R_TYPE(rels[i].r_info);
        ssp[k].index            = static_cast<uint16_t>(sym_idx);
        ssp[k].binding          = ELF32_ST_BIND(sym.st_info);
        ssp[k].virtual_address  = sym.st_value;
        ssp[k].name             = sym.st_name ? name_at(dynstr, pa.dynstr_size, sym.st_name)
                                              : std::string_view{};
        ++k;
    }
    count_out = k;
    return loader_error::OK;
}

uint32_t elf_reader::read_dynamic_symbols(const pass_args& pa,
                                          std::span<symbol_patch> ssp,
                                          uint32_t start_k,
                                          uint32_t& added_out) {
    uint32_t k = start_k;
    uint32_t num_syms = pa.dynsym_size / pa.dynsym_entry_size;
    if (!pa.dynsym_start || !pa.dynstr_start) { added_out = 0; return k; }

    auto* syms   = reinterpret_cast<const Elf32_Sym*>(pa.dynsym_start);
    auto* dynstr = reinterpret_cast<const char*>(pa.dynstr_start);

    for (uint32_t i = 0; i < num_syms; ++i) {
        uint8_t bind = ELF32_ST_BIND(syms[i].st_info);
        if (bind != STB_WEAK || syms[i].st_value == 0) continue;

        auto sym_name = name_at(dynstr, pa.dynstr_size, syms[i].st_name);
        if (sym_name.empty()) continue;

        bool already = false;
        for (uint32_t l = 0; l < k; ++l)
            if (ssp[l].name == sym_name) { already = true; break; }
        if (already || k >= ssp.size()) continue;

        ssp[k] = {sym_name, 0, syms[i].st_value,
                  static_cast<uint8_t>(ELF32_R_TYPE(0)),
                  static_cast<uint16_t>(i), bind};
        ++k;
    }
    added_out = k - start_k;
    return k;
}

uint32_t elf_reader::calculate_patch_array_size(const pass_args& pa) {
    uint32_t k = pa.reldyn_size / pa.reldyn_entry_size;
    if (!pa.dynsym_start) return k;
    auto* syms = reinterpret_cast<const Elf32_Sym*>(pa.dynsym_start);
    uint32_t n = pa.dynsym_size / pa.dynsym_entry_size;
    for (uint32_t i = 0; i < n; ++i) {
        if (ELF32_ST_BIND(syms[i].st_info) == STB_WEAK && syms[i].st_value)
            ++k;
    }
    return k;
}

uint32_t elf_reader::find_symbol_address(std::string_view name,
                                         const section_map& ms) {
    const auto& hs = ms[uint32_t(helper_section_idx::HASH)];
    const auto& ds = ms[uint32_t(helper_section_idx::DYN_SYM)];
    const auto& ss = ms[uint32_t(helper_section_idx::DYN_STR)];
    auto* hash_start = hs.address;
    auto* dsym       = ds.address;
    auto* dstr       = ss.address;
    if (!hash_start || !dsym || !dstr) return 0;

    auto* syms   = reinterpret_cast<const Elf32_Sym*>(dsym);
    auto* dynstr = reinterpret_cast<const char*>(dstr);
    auto* hdr    = reinterpret_cast<const uint32_t*>(hash_start);

    // A valid SysV hash table is at least [nbucket, nchain] + nbucket buckets
    // + nchain chain entries. Bound every access against the section sizes so a
    // malformed/truncated .hash or .dynsym in an untrusted object can neither
    // read out of bounds nor spin forever on a cyclic chain.
    uint32_t nwords = hs.size / sizeof(uint32_t);
    if (nwords < 2) return 0;
    uint32_t nbucket = hdr[0];
    uint32_t nchain  = hdr[1];
    if (nbucket == 0 || nchain == 0) return 0;
    if (uint64_t(2) + nbucket + nchain > nwords) return 0;  // table truncated

    uint32_t num_syms = ds.entry_size ? ds.size / ds.entry_size : 0;
    const uint32_t* bucket = hdr + 2;
    const uint32_t* chain  = bucket + nbucket;

    uint32_t idx = bucket[loader::detail::elf_hash(name) % nbucket];
    // The chain can visit at most nchain nodes; cap the walk to defend against
    // a crafted cycle.
    for (uint32_t steps = 0; idx != 0 && idx < nchain && steps < nchain; ++steps) {
        if (idx < num_syms && syms[idx].st_name) {
            auto sn = name_at(dynstr, ss.size, syms[idx].st_name);
            if (sn == name) return syms[idx].st_value;
        }
        idx = chain[idx];
    }
    return 0;
}

// ── DT_NEEDED dependency scan ──────────────────────────────────────────

loader_error elf_reader::scan_dependencies(std::vector<std::string>& deps) {
    uint32_t dyn_idx = find_program(PT_DYNAMIC);
    if (dyn_idx == 0xFFFFFFFF) return loader_error::OK;

    Elf32_Phdr dp;
    read_program_header(dyn_idx, dp);

    uint32_t ndyn = dp.p_filesz / sizeof(Elf32_Dyn);
    auto buf = std::make_unique<Elf32_Dyn[]>(ndyn);
    read_at_offset({reinterpret_cast<uint8_t*>(buf.get()), ndyn * sizeof(Elf32_Dyn)},
                   dp.p_offset);

    // Need .dynstr to resolve DT_NEEDED names.
    uint32_t sidx = find_section_by_name(".dynstr");
    if (sidx == SECTION_NOT_FOUND) return loader_error::OK;

    Elf32_Shdr ss;
    read_section_header(sidx, ss);
    auto strbuf = std::make_unique<char[]>(ss.sh_size);
    error = loader_error::OK;
    read_at_offset({reinterpret_cast<uint8_t*>(strbuf.get()), ss.sh_size}, ss.sh_offset);
    if (error != loader_error::OK)
        return loader_error::OK;

    for (uint32_t i = 0; i < ndyn; ++i) {
        if (buf[i].d_tag == DT_NEEDED) {
            uint32_t off = buf[i].d_un.d_val;
            if (off < ss.sh_size) {
                log_info("  dependency: %s", strbuf.get() + off);
                deps.emplace_back(strbuf.get() + off);
            } else {
                log_warn("DT_NEEDED string offset %u out of range", off);
            }
        }
    }
    return loader_error::OK;
}

} // namespace elf_loader

/**
 * @file dynamic-linker.cpp
 * @brief Orchestrates shared library loading, relocation, and unloading.
 *
 * Symbol resolution order (fully automatic, no hardcoded symbols):
 *   1. Main's .symtab (read from firmware ELF via semihosting) — linear scan
 *   2. Library's own .dynsym — automatic
 *   3. Cross-library search — automatic
 */
#include "dynamic-newlib/dynamic-linker.hpp"
#include "loader/elf-image.hpp"
#include "loader/relocator.hpp"
#include "loader/symbol-resolver.hpp"
#include "scl/log.h"
#include <cstring>
#include <cstdio>
#include <new>
#include <algorithm>

namespace firmware {

dynamic_linker::dynamic_linker(const link_context& ctx) noexcept
    : ctx_(ctx) {}

/** @brief Parse, load, relocate, and construct from a byte_source. */
loader::result<library*>
dynamic_linker::load_from_source(loader::byte_source& src) noexcept {
    if (libs_.size() >= 8)
        return loader::unexpected(loader::error::OUT_OF_MEMORY);

    auto img = loader::elf_image::parse(src);
    if (!img) return loader::unexpected(img.err());

    auto lib = std::make_unique<library>();
    auto bv = img->image_base();
    if (!bv) return loader::unexpected(bv.err());
    lib->base_va = *bv;

    /* Allocate extra space at the tail of the image buffer for the
       patch_journal entries (Option 3A: co-allocation). */
    auto jcap = kJournalDefaultCapacity;
    auto jsize = jcap * sizeof(patch_journal::Entry);
    auto r = lib->image.load_segments(*img, jsize);
    if (!r) return loader::unexpected(r.err());

    /* Point the journal into the tail of the allocated buffer. */
    auto* base = lib->image.base();
    auto img_sz = lib->image.size();
    if (img_sz > jsize) {
        auto* jbuf = base + (img_sz - jsize);
        lib->journal.use_buffer(jbuf, jcap);
    }

    auto reldata = img->collect_relocations();
    if (!reldata) return loader::unexpected(reldata.err());

    auto read_section = [&](const char* name) -> loader::result<std::vector<std::byte>> {
        auto sh = img->find_section(name);
        if (!sh) return loader::unexpected(sh.err());
        std::vector<std::byte> buf(sh->sh_size);
        auto r = img->source().read_at(sh->sh_offset, std::span<std::byte>(buf.data(), buf.size()));
        if (!r) return loader::unexpected(r.err());
        return buf;
    };

    auto ds = read_section(".dynsym");
    if (!ds) return loader::unexpected(ds.err());
    auto dr = read_section(".dynstr");
    if (!dr) return loader::unexpected(dr.err());

    auto plan = loader::relocator::compute(*reldata, *ds, *dr);
    if (!plan) return loader::unexpected(plan.err());
    auto ap = apply_plan(*plan, base, lib.get());
    if (!ap) return loader::unexpected(ap.err());

    auto store_va = [&](const char* name, std::uint32_t& va, std::uint32_t& sz) {
        auto s = img->find_section(name);
        if (s.has_value()) { va = s->sh_addr; sz = s->sh_size; }
    };
    store_va(".dynsym", lib->dynsym_va, lib->dynsym_sz);
    store_va(".dynstr", lib->dynstr_va, lib->dynstr_sz);
    store_va(".hash",   lib->hash_va,   lib->hash_sz);
    /* Patch the firmware's .got.plt so direct bl calls resolve. */
    auto pmg = patch_main_got(lib.get());
    if (!pmg) return loader::unexpected(pmg.err());

    auto sh = img->find_section(".init_array");
    if (!sh.has_value())
        sh = img->find_section(".inits");
    if (sh.has_value() && sh->sh_size > 0) {  /* relative VMA 0 is valid for PIC library sections */
        lib->init_array = base + (sh->sh_addr - lib->base_va);
        lib->init_count = sh->sh_size / sizeof(void(*)());
    }

    auto ct = run_constructors(lib.get());
    if (!ct) return loader::unexpected(ct.err());

    /* Cross-resolution pass: resolve pending entries from the new library. */
    if (lib->dynsym_sz && lib->dynstr_sz && lib->hash_sz) {
        auto& nl = *lib;
        auto ds = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (nl.dynsym_va - nl.base_va)),
            nl.dynsym_sz);
        auto dr = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (nl.dynstr_va - nl.base_va)),
            nl.dynstr_sz);
        auto ha = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (nl.hash_va - nl.base_va)),
            nl.hash_sz);
        loader::symbol_resolver res(ds, dr, ha);
        for (auto& p : pending_) {
            if (*reinterpret_cast<std::uint32_t*>(p.address) != 0) continue;
            auto a = res.address_of(p.name);
            if (a.has_value() && *a != 0)
                *reinterpret_cast<std::uint32_t*>(p.address) =
                    reinterpret_cast<std::uint32_t>(base + (*a - nl.base_va));
        }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
            [](const pending_reloc& p) {
                return *reinterpret_cast<std::uint32_t*>(p.address) != 0;
            }), pending_.end());
    }

    auto* ptr = lib.get();
    libs_.push_back(std::move(lib));
    return ptr;
}

/** @brief Revert journal, prune pending relocs, free memory, remove from library list. */
void dynamic_linker::unload(library* lib) noexcept {
    if (!lib || !lib->image.is_loaded()) return;

    lib->journal.revert();
    /* Prune pending relocs pointing into this library's image (dangling). */
    auto base_addr = reinterpret_cast<std::uintptr_t>(lib->image.base());
    auto end_addr  = base_addr + lib->image.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
        [base_addr, end_addr](const pending_reloc& p) {
            auto addr = reinterpret_cast<std::uintptr_t>(p.address);
            return addr >= base_addr && addr < end_addr;
        }), pending_.end());
    /* Find and erase the library from the vector (Option 2B: heap alloc). */
    auto it = std::find_if(libs_.begin(), libs_.end(),
        [lib](const auto& lp) { return lp.get() == lib; });
    if (it != libs_.end()) libs_.erase(it);
}

/** @brief Resolve a symbol by searching main, loaded libs, cross-libs, and fallback.
 *
 *  Fully automatic — no hardcoded test data:
 *    1. Main's .dynsym (parsed from _DYNAMIC)
 *    2. Loaded library's own .dynsym
 *    3. Cross-library search (all other loaded libs)
 *    4. symtab from firmware ELF */
loader::result<void*>
dynamic_linker::lookup(library* lib, const char* name) noexcept {
    if (!lib || !lib->image.is_loaded())
        return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);

    auto* base = lib->image.base();

    /* 1. Try main's symbol table (parsed from _DYNAMIC automatically). */
    {
        auto a = ctx_.resolve(name);
        if (a != 0) return reinterpret_cast<void*>(a);
    }

    /* 2. Try the loaded library's own .dynsym (from its loaded image in RAM). */
    if (lib->dynsym_sz && lib->dynstr_sz && lib->hash_sz) {
        auto ds = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynsym_va - lib->base_va)),
            lib->dynsym_sz);
        auto dr = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynstr_va - lib->base_va)),
            lib->dynstr_sz);
        auto ha = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->hash_va - lib->base_va)),
            lib->hash_sz);
        loader::symbol_resolver res(ds, dr, ha);
        auto a = res.address_of(name);
        if (a.has_value() && *a != 0)
            return reinterpret_cast<void*>(base + (*a - lib->base_va));
    }

    /* 3. Cross-library: search all other loaded libraries' .dynsym. */
    {
        auto r = resolve_cross(name);
        if (r.has_value() && *r != 0) return reinterpret_cast<void*>(*r);
    }

    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

/** @brief Search all loaded libraries' .dynsym for a symbol by name. */
loader::result<std::uint32_t>
dynamic_linker::resolve_cross(const char* name) const noexcept {
    for (auto& lp : libs_) {
        auto* lib = lp.get();
        if (!lib || !lib->image.is_loaded()) continue;
        if (!lib->dynsym_sz || !lib->dynstr_sz || !lib->hash_sz) continue;
        auto* base = lib->image.base();

        auto ds = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynsym_va - lib->base_va)),
            lib->dynsym_sz);
        auto dr = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynstr_va - lib->base_va)),
            lib->dynstr_sz);
        auto ha = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->hash_va - lib->base_va)),
            lib->hash_sz);
        loader::symbol_resolver res(ds, dr, ha);
        auto a = res.address_of(name);
        if (a.has_value() && *a != 0)
            return reinterpret_cast<std::uint32_t>(base + (*a - lib->base_va));
    }
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

/** @brief Apply all relocations: RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32, COPY. */
loader::result<void>
dynamic_linker::apply_plan(loader::relocation_plan& plan, std::byte* base,
                          library* lib) noexcept {
    auto main_ds = ctx_.dynsym();
    auto main_dr = ctx_.dynstr();
    auto main_ha = ctx_.hash();
    loader::symbol_resolver main_res(main_ds, main_dr, main_ha);

    auto bv = lib->base_va;
    for (auto& entry : plan) {
        auto* patch_site = reinterpret_cast<std::uint32_t*>(base + (entry.offset - bv));
        std::uint32_t addend = *patch_site;

        switch (entry.type) {
        case loader::elf::rel_type::RELATIVE: {
            auto new_val = reinterpret_cast<std::uint32_t>(base) + addend - bv;
            auto r = lib->journal.record(reinterpret_cast<std::byte*>(patch_site), addend);
            if (!r) return loader::unexpected(r.err());
            *patch_site = new_val;
            break;
        }
        case loader::elf::rel_type::GLOB_DAT:
        case loader::elf::rel_type::JUMP_SLOT: {
            std::uint32_t sym_addr;
            if (entry.sym_value != 0) {
                sym_addr = reinterpret_cast<std::uint32_t>(base) + entry.sym_value - bv;
            } else {
                /* Automatic: main symtab -> cross-lib -> pending */
                auto addr = ctx_.resolve(entry.name.data());
                if (addr != 0)
                    sym_addr = addr;
                else {
                    auto* fb = reinterpret_cast<void*>(ctx_.resolve(entry.name.data()));
                    if (fb)
                        sym_addr = reinterpret_cast<std::uint32_t>(fb);
                    else {
                        auto ca = resolve_cross(entry.name.data());
                        if (ca.has_value())
                            sym_addr = *ca;
                        else
                            sym_addr = 0;
                    }
                }
            }
            /* Record as pending if still unresolved (0). */
            if (sym_addr == 0) {
                static constexpr std::size_t NAME_LEN = 32;
                auto it = pending_.emplace(pending_.end());
                it->address = reinterpret_cast<std::byte*>(patch_site);
                it->orig_val = addend;
                std::strncpy(it->name, entry.name.data(), NAME_LEN - 1);
                it->name[NAME_LEN - 1] = '\0';
            }
            auto r = lib->journal.record(reinterpret_cast<std::byte*>(patch_site), addend);
            if (!r) return loader::unexpected(r.err());
            *patch_site = sym_addr;
            break;
        }
        case loader::elf::rel_type::ABS32:
        case loader::elf::rel_type::REL32: {
            std::uint32_t sym_addr;
            if (entry.sym_value != 0) {
                sym_addr = reinterpret_cast<std::uint32_t>(base) + entry.sym_value - bv;
            } else {
                auto addr = ctx_.resolve(entry.name.data());
                if (addr != 0)
                    sym_addr = addr;
                else {
                    auto* fb = reinterpret_cast<void*>(ctx_.resolve(entry.name.data()));
                    if (fb)
                        sym_addr = reinterpret_cast<std::uint32_t>(fb);
                    else {
                        auto ca = resolve_cross(entry.name.data());
                        if (ca.has_value())
                            sym_addr = *ca;
                        else
                            sym_addr = 0;
                    }
                }
            }
            if (sym_addr == 0) {
                static constexpr std::size_t NAME_LEN = 32;
                auto it = pending_.emplace(pending_.end());
                it->address = reinterpret_cast<std::byte*>(patch_site);
                it->orig_val = addend;
                std::strncpy(it->name, entry.name.data(), NAME_LEN - 1);
                it->name[NAME_LEN - 1] = '\0';
            }
            auto r = lib->journal.record(reinterpret_cast<std::byte*>(patch_site), addend);
            if (!r) return loader::unexpected(r.err());
            *patch_site = sym_addr + addend;
            break;
        }
        case loader::elf::rel_type::COPY: {
            auto addr = ctx_.resolve(entry.name.data());
            if (addr == 0) {
                auto* fb = reinterpret_cast<void*>(ctx_.resolve(entry.name.data()));
                if (fb) addr = reinterpret_cast<std::uint32_t>(fb);
            }
            if (addr != 0) {
                auto* src = reinterpret_cast<const std::byte*>(addr);
                auto sz = entry.sym_size ? entry.sym_size : sizeof(std::uint32_t);
                auto r = lib->journal.record_bytes(reinterpret_cast<std::byte*>(patch_site), sz);
                if (!r) return loader::unexpected(r.err());
                std::memcpy(patch_site, src, sz);
            }
            break;
        }
        default:
            return loader::unexpected(loader::error::UNSUPPORTED_RELOC);
        }
    }
    return {};
}

/** @brief Patch the firmware's own GOT entries using symbols from a loaded library. */
loader::result<void>
dynamic_linker::patch_main_got(library* lib) noexcept {
    auto reldyn = ctx_.reldyn();
    if (reldyn.empty()) return {};

    auto ds = ctx_.dynsym();
    auto dr = ctx_.dynstr();
    auto ha = ctx_.hash();

    /* Build a resolver for the library's symbols. */
    std::span<const std::byte> lib_ds, lib_dr, lib_ha;
    if (lib->dynsym_sz && lib->dynstr_sz && lib->hash_sz) {
        auto* base = lib->image.base();
        lib_ds = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynsym_va - lib->base_va)),
            lib->dynsym_sz);
        lib_dr = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->dynstr_va - lib->base_va)),
            lib->dynstr_sz);
        lib_ha = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(base + (lib->hash_va - lib->base_va)),
            lib->hash_sz);
    }
    loader::symbol_resolver lib_res(lib_ds, lib_dr, lib_ha);

    const std::size_t count = reldyn.size() / sizeof(loader::elf::Rel);
    auto* base = lib->image.base();
    auto bv = lib->base_va;

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t off = i * sizeof(loader::elf::Rel);
        loader::elf::Rel rel{};
        std::memcpy(&rel, reldyn.data() + off, sizeof(rel));
        auto r_type = loader::elf::r_type(rel.r_info);
        auto r_sym  = loader::elf::r_sym(rel.r_info);

        /* Only handle JUMP_SLOT and GLOB_DAT (main's PLT/GOT imports). */
        if (r_type != 22 && r_type != 21) continue;

        /* Read symbol name from firmware's .dynstr. */
        loader::elf::Sym sym{};
        auto sym_off = r_sym * sizeof(loader::elf::Sym);
        if (sym_off + sizeof(sym) > ds.size()) continue;
        std::memcpy(&sym, ds.data() + sym_off, sizeof(sym));
        auto* name = reinterpret_cast<const char*>(dr.data()) + sym.st_name;

        /* Look up the symbol in the loaded library. */
        auto lib_addr = lib_res.address_of(name);
        if (!lib_addr.has_value() || *lib_addr == 0) continue;

        /* Symbol is in the library — compute absolute address. */
        auto abs_addr = reinterpret_cast<std::uint32_t>(base) + *lib_addr - bv;

        /* Patch the firmware's GOT entry at rel.r_offset. */
        auto* patch_site = reinterpret_cast<std::uint32_t*>(rel.r_offset);
        auto r = lib->journal.record(reinterpret_cast<std::byte*>(patch_site), *patch_site);
        if (!r) return loader::unexpected(r.err());
        *patch_site = abs_addr;
    }
    return {};
}

/** @brief Iterate .init_array and call each non-null constructor function. */
loader::result<void>
dynamic_linker::run_constructors(library* lib) noexcept {
    if (!lib->init_array || lib->init_count == 0) return {};
    using Ctor = void(*)();
    auto* ctors = static_cast<Ctor*>(lib->init_array);
    for (std::size_t i = 0; i < lib->init_count; ++i) {
        if (ctors[i])  /* skip null entries (e.g. .inits placeholder at VMA 0) */
            ctors[i]();
    }
    lib->constructed = true;
    return {};
}

} // namespace firmware

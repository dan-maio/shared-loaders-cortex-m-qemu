#include "test-helpers.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"

namespace {
void classify(const char* label, std::uint32_t a, const char* note) {
    trace_printf("      %-18s 0x%08x %s  %s\n",
                 label, a, is_flash(a) ? "(*F*)" : "(*R*)", note);
}

void dump_got_walkthrough(const char* name, const scl::Module& m) {
    const auto& sm = m.get_smodule();
    const fw::FsEntry* e = fw::fs_find(name);
    if (!e) {
        trace_printf("  failed to find %s in filesystem\n", name);
        return;
    }
    std::span<const std::byte> blob{ fw::fs_blob(*e), e->size };
    loader::memory_byte_source src(blob);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) {
        trace_printf("  failed to parse %s ELF\n", name);
        return;
    }
    auto img = *parsed;
    auto sh_got = img.find_section(".got");
    if (!sh_got || sh_got->sh_size == 0) {
        trace_printf("  %s has no .got section\n", name);
        return;
    }

    std::uintptr_t data_base = sm.data_base;
    std::uint32_t data_vaddr = 0;
    for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (img.program_header(i, ph).has_value()) {
            if (ph.p_type == loader::elf::PT_LOAD && (ph.p_flags & 0x2)) {
                data_vaddr = ph.p_vaddr;
                break;
            }
        }
    }

    const std::uint32_t got_vaddr = sh_got->sh_addr;
    const std::uint32_t got_words = sh_got->sh_size / 4;
    const std::uintptr_t ram_got_addr = data_base + (got_vaddr - data_vaddr);

    trace_printf("\nGOT/PLT DIDACTIC WALKTHROUGH (%s)\n", name);
    trace_printf("==========================================\n");
    trace_printf("  .got at VMA=0x%08x, RAM=0x%08x (*R*), size=%u words\n",
                 (unsigned)got_vaddr, (unsigned)ram_got_addr, (unsigned)got_words);

    auto sh_plt = img.find_section(".plt");
    if (sh_plt && sh_plt->sh_size > 0) {
        std::uintptr_t flash_plt_addr = sm.text_base + (sh_plt->sh_addr - sm.text_vaddr);
        trace_printf("  .plt at VMA=0x%08x, FLASH=0x%08x (*F*), size=%u bytes\n",
                     (unsigned)sh_plt->sh_addr, (unsigned)flash_plt_addr, (unsigned)sh_plt->sh_size);
    } else {
        trace_printf("  .plt (not found)\n");
    }

    auto get_reloc_info = [&](std::uint32_t slot_va, std::string_view& sym_name, std::uint32_t& rel_type, const char*& sec_name) -> bool {
        for (const char* sn : { ".rel.dyn", ".rel.plt" }) {
            auto sh = img.find_section(sn);
            if (!sh || sh->sh_size == 0) continue;
            std::span<const std::byte> rel_span{ blob.data() + sh->sh_offset, sh->sh_size };
            const std::size_t count = rel_span.size() / sizeof(loader::elf::Rel);
            for (std::size_t k = 0; k < count; ++k) {
                using loader::detail::load_word;
                const std::uint32_t r_offset = load_word(rel_span, k * sizeof(loader::elf::Rel) + 0);
                const std::uint32_t r_info   = load_word(rel_span, k * sizeof(loader::elf::Rel) + 4);
                if (r_offset == slot_va) {
                    rel_type = r_info & 0xffu;
                    sec_name = sn;
                    const std::uint32_t sym_idx = r_info >> 8;
                    if (sym_idx != 0) {
                        auto dynsym = img.find_section(".dynsym");
                        auto dynstr = img.find_section(".dynstr");
                        if (dynsym && dynstr) {
                            std::span<const std::byte> dynsym_sp{ blob.data() + dynsym->sh_offset, dynsym->sh_size };
                            std::span<const std::byte> dynstr_sp{ blob.data() + dynstr->sh_offset, dynstr->sh_size };
                            const std::uint32_t st_name = load_word(dynsym_sp, sym_idx * sizeof(loader::elf::Sym) + 0);
                            sym_name = loader::detail::name_at(dynstr_sp, st_name);
                        }
                    }
                    return true;
                }
            }
        }
        return false;
    };

    // [BEFORE] values read from the ELF file
    trace_printf("\n  Initial GOT values BEFORE relocation:\n");
    for (std::uint32_t i = 0; i < got_words; ++i) {
        using loader::detail::load_word;
        std::uint32_t val_before = load_word(blob, sh_got->sh_offset + i * 4);
        std::string_view sym_name{};
        std::uint32_t rel_type = 0;
        const char* sec_name = nullptr;
        bool has_reloc = get_reloc_info(got_vaddr + i * 4, sym_name, rel_type, sec_name);

        const char* m_before = "";
        if (val_before >= 0x08000000u) {
            m_before = is_flash(val_before) ? " (*F*)" : " (*R*)";
        }

        trace_printf("    [%2u] Slot VMA=0x%08x: 0x%08x%s", (unsigned)i, (unsigned)(got_vaddr + i * 4), (unsigned)val_before, m_before);
        if (i < 3) {
            trace_printf("  (reserved)");
        } else if (has_reloc) {
            if (rel_type == 23) {
                trace_printf("  (RELATIVE -> rebase)");
            } else {
                trace_printf("  (to be patched: %.*s)", (int)sym_name.size(), sym_name.data());
            }
        }
        trace_printf("\n");
    }

    // [AFTER] values read from RAM
    trace_printf("\n  GOT values AFTER relocation (current state in RAM):\n");
    for (std::uint32_t i = 0; i < got_words; ++i) {
        using loader::detail::load_word;
        std::uint32_t val_before = load_word(blob, sh_got->sh_offset + i * 4);
        std::uint32_t val_after = *reinterpret_cast<const volatile std::uint32_t*>(ram_got_addr + i * 4);
        std::string_view sym_name{};
        std::uint32_t rel_type = 0;
        const char* sec_name = nullptr;
        bool has_reloc = get_reloc_info(got_vaddr + i * 4, sym_name, rel_type, sec_name);

        const char* m_before = "";
        if (val_before >= 0x08000000u) {
            m_before = is_flash(val_before) ? " (*F*)" : " (*R*)";
        }
        const char* m_after = "";
        if (val_after >= 0x08000000u) {
            m_after = is_flash(val_after) ? " (*F*)" : " (*R*)";
        }

        trace_printf("    [%2u] RAM=0x%08x (*R*): 0x%08x%s -> 0x%08x%s", (unsigned)i, (unsigned)(ram_got_addr + i * 4), (unsigned)val_before, m_before, (unsigned)val_after, m_after);
        if (i < 3) {
            trace_printf("  (reserved)");
        } else if (has_reloc) {
            const char* sn = (std::strcmp(sec_name, ".rel.plt") == 0) ? "PLT" : "DATA";
            if (rel_type == 23) {
                trace_printf("  (%s RELATIVE -> rebased)", sn);
            } else {
                trace_printf("  (%s -> resolved: %.*s)", sn, (int)sym_name.size(), sym_name.data());
            }
        }
        trace_printf("\n");
    }

    // [PLT AFTER] values in PLT (if any) to check branches
    if (sh_plt && sh_plt->sh_size > 0) {
        trace_printf("\n  PLT resolution details:\n");
        std::uintptr_t flash_plt_addr = sm.text_base + (sh_plt->sh_addr - sm.text_vaddr);
        const std::size_t count = sh_plt->sh_size / 12;
        for (std::size_t i = 1; i < count; ++i) {
            std::uint32_t slot_va = sh_plt->sh_addr + i * 12;
            std::uint32_t r_offset = 0;
            std::string_view sym_name{};
            auto sh_rplt = img.find_section(".rel.plt");
            if (sh_rplt) {
                std::span<const std::byte> rel_span{ blob.data() + sh_rplt->sh_offset, sh_rplt->sh_size };
                const std::size_t rc = rel_span.size() / sizeof(loader::elf::Rel);
                if (i - 1 < rc) {
                    using loader::detail::load_word;
                    r_offset = load_word(rel_span, (i - 1) * sizeof(loader::elf::Rel) + 0);
                    std::uint32_t r_info = load_word(rel_span, (i - 1) * sizeof(loader::elf::Rel) + 4);
                    std::uint32_t sym_idx = r_info >> 8;
                    if (sym_idx != 0) {
                        auto dynsym = img.find_section(".dynsym");
                        auto dynstr = img.find_section(".dynstr");
                        if (dynsym && dynstr) {
                            std::span<const std::byte> dynsym_sp{ blob.data() + dynsym->sh_offset, dynsym->sh_size };
                            std::span<const std::byte> dynstr_sp{ blob.data() + dynstr->sh_offset, dynstr->sh_size };
                            const std::uint32_t st_name = load_word(dynsym_sp, sym_idx * sizeof(loader::elf::Sym) + 0);
                            sym_name = loader::detail::name_at(dynstr_sp, st_name);
                        }
                    }
                }
            }

            std::uint32_t val_after = 0;
            if (r_offset) {
                std::uintptr_t slot_ram = data_base + (r_offset - data_vaddr);
                val_after = *reinterpret_cast<const volatile std::uint32_t*>(slot_ram);
            }

            const char* m_after = "";
            if (val_after >= 0x08000000u) {
                m_after = is_flash(val_after) ? " (*F*)" : " (*R*)";
            }

            trace_printf("    PLT[%u] VMA=0x%08x, FLASH=0x%08x (*F*): resolves via GOT[0x%04x] -> 0x%08x%s  %.*s\n",
                         (unsigned)i, (unsigned)slot_va, (unsigned)(flash_plt_addr + i * 12),
                         (unsigned)(r_offset - got_vaddr), (unsigned)val_after, m_after,
                         (int)sym_name.size(), sym_name.data());
        }
    }

    // [IMPORT ANALYSIS] check imports of this library
    auto dynsym = img.find_section(".dynsym");
    auto dynstr = img.find_section(".dynstr");
    auto rel = img.find_section(".rel.dyn");
    if (dynsym && dynstr && rel) {
        std::span<const std::byte> dynsym_sp{ blob.data() + dynsym->sh_offset, dynsym->sh_size };
        std::span<const std::byte> dynstr_sp{ blob.data() + dynstr->sh_offset, dynstr->sh_size };
        std::span<const std::byte> rel_sp{ blob.data() + rel->sh_offset, rel->sh_size };
        const std::size_t rc = rel_sp.size() / sizeof(loader::elf::Rel);

        trace_printf("\n  Imported symbols bound in this module:\n");
        for (std::size_t k = 0; k < rc; ++k) {
            using loader::detail::load_word;
            std::uint32_t r_offset = load_word(rel_sp, k * sizeof(loader::elf::Rel) + 0);
            std::uint32_t r_info   = load_word(rel_sp, k * sizeof(loader::elf::Rel) + 4);
            std::uint32_t rel_type = r_info & 0xffu;
            std::uint32_t sym_idx  = r_info >> 8;

            std::string_view sym_name{};
            if (sym_idx != 0) {
                const std::uint32_t st_name = load_word(dynsym_sp, sym_idx * sizeof(loader::elf::Sym) + 0);
                sym_name = loader::detail::name_at(dynstr_sp, st_name);
            }

            if (rel_type == 21 || rel_type == 22) {
                std::uintptr_t slot_ram = data_base + (r_offset - data_vaddr);
                std::uint32_t val_after = *reinterpret_cast<const volatile std::uint32_t*>(slot_ram);
                const char* m_after = "";
                if (val_after >= 0x08000000u) {
                    m_after = is_flash(val_after) ? " (*F*)" : " (*R*)";
                }
                const char* sn = (std::strcmp(name, "cpp3.so") == 0) ? "cpp3.so" : "c2.so";
                trace_printf("    %s GOT[0x%04x] RAM=0x%08x (*R*): 0x%08x%s -> 0x%08x%s  %.*s\n",
                             sn, (unsigned)(r_offset - got_vaddr), (unsigned)slot_ram,
                             0, "", (unsigned)val_after, m_after,
                             (int)sym_name.size(), sym_name.data());
            }
        }
    }
    trace_printf("==========================================\n");
}
}  // namespace

void xip_walkthrough_complex() {
    trace_printf("\n======================================================================\n");
    trace_printf(" COMPLEX XIP WALKTHROUGH — c1 + c2 + cpp3 (static loader - Direct)\n");
    trace_printf("======================================================================\n");

    scl::Module m_c2, m_cpp3;
    if (!xload("c2.so", m_c2) || !xload("cpp3.so", m_cpp3)) {
        trace_printf("  failed to load c2 or cpp3\n");
        return;
    }

    const auto& sm_c2 = m_c2.get_smodule();
    const auto& sm_cpp3 = m_cpp3.get_smodule();

    // [1] Loader placement
    trace_printf("\n[1] Segment Mapping:\n");
    trace_printf("    c2.so:\n");
    trace_printf("      .text/.rodata/PLT  0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(sm_c2.text_base), is_flash(sm_c2.text_base) ? "(*F*)" : "(*R*)", sm_c2.text_size);
    trace_printf("      .got/.data/.bss    0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(sm_c2.data_base), is_flash(sm_c2.data_base) ? "(*F*)" : "(*R*)", sm_c2.data_memsz);
    trace_printf("    cpp3.so:\n");
    trace_printf("      .text/.rodata/PLT  0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(sm_cpp3.text_base), is_flash(sm_cpp3.text_base) ? "(*F*)" : "(*R*)", sm_cpp3.text_size);
    trace_printf("      .got/.data/.bss    0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(sm_cpp3.data_base), is_flash(sm_cpp3.data_base) ? "(*F*)" : "(*R*)", sm_cpp3.data_memsz);

    // GOT/PLT didactic dumps
    dump_got_walkthrough("c2.so", m_c2);
    dump_got_walkthrough("cpp3.so", m_cpp3);

    // [2] Retrieve parameters from c2 (direct call)
    trace_printf("\n[2] Retrieve parameters from c2:\n");
    std::uint32_t x = call_sym0(m_c2, "c2_param");
    std::uint32_t seed = rd(m_c2, "c2_const");
    trace_printf("      c2_param()   = %u\n", x);
    trace_printf("      c2_const     = %u\n", seed);

    // [3] Classify symbols in cpp3
    trace_printf("\n[3] Symbol placement in cpp3.so:\n");
    void* fn_method = m_cpp3.sym("cpp3_method");
    classify("cpp3_method", reinterpret_cast<std::uint32_t>(fn_method), "code: executing in flash");

    // [4] Prove execution
    trace_printf("\n[4] Execution Proof:\n");
    std::uint32_t r = call_sym2(m_cpp3, "cpp3_method", 2, 100);
    trace_printf("      cpp3_method(2,100) = %u  (entry 0x%08x is %s)\n",
                 r, reinterpret_cast<std::uint32_t>(fn_method),
                 is_flash(reinterpret_cast<std::uint32_t>(fn_method)) ? "FLASH" : "RAM");

    m_cpp3 = scl::Module{};
    m_c2 = scl::Module{};
}

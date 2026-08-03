#include "test-helpers.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"

namespace {
void classify(const char* label, std::uint32_t a, const char* note) {
    trace_printf("      %-18s 0x%08x %s  %s\n",
                 label, a, is_flash(a) ? "(*F*)" : "(*R*)", note);
}

void dump_got_walkthrough(const char* name, const xip::stat::SModule& m) {
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

    std::uintptr_t data_base = m.data_base;
    // Find data_vaddr (the virtual address of the first writable LOAD segment)
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
        std::uintptr_t flash_plt_addr = m.text_base + (sh_plt->sh_addr - m.text_vaddr);
        trace_printf("  .plt at VMA=0x%08x, FLASH=0x%08x (*F*), size=%u bytes\n",
                     (unsigned)sh_plt->sh_addr, (unsigned)flash_plt_addr, (unsigned)sh_plt->sh_size);
    } else {
        trace_printf("  .plt (not found)\n");
    }

    // Helper to find relocation for a GOT offset
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
            if (rel_type == 23) { // R_ARM_RELATIVE
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
            trace_printf("  (reserved by dynamic linker)");
        } else if (has_reloc) {
            if (rel_type == 23) { // R_ARM_RELATIVE
                trace_printf("  <- rebased");
            } else {
                trace_printf("  <- resolved (%.*s)", (int)sym_name.size(), sym_name.data());
            }
        }
        trace_printf("\n");
    }

    // [PLT] PLT-targeted GOT slots (relocations of type JUMP_SLOT)
    trace_printf("\n  [PLT] function slots reached via the PLT (GOT JUMP_SLOTs):\n");
    for (const char* sn : { ".rel.dyn", ".rel.plt" }) {
        auto sh = img.find_section(sn);
        if (!sh || sh->sh_size == 0) continue;
        std::span<const std::byte> rel_span{ blob.data() + sh->sh_offset, sh->sh_size };
        const std::size_t count = rel_span.size() / sizeof(loader::elf::Rel);
        for (std::size_t k = 0; k < count; ++k) {
            using loader::detail::load_word;
            const std::uint32_t r_offset = load_word(rel_span, k * sizeof(loader::elf::Rel) + 0);
            const std::uint32_t r_info   = load_word(rel_span, k * sizeof(loader::elf::Rel) + 4);
            const std::uint32_t rel_type = r_info & 0xffu;
            if (rel_type != 22) continue; // R_ARM_JUMP_SLOT = 22
            
            const std::uintptr_t slot_ram = ram_got_addr + (r_offset - got_vaddr);
            std::uint32_t val_before = load_word(blob, sh_got->sh_offset + (r_offset - got_vaddr));
            std::uint32_t val_after = *reinterpret_cast<const volatile std::uint32_t*>(slot_ram);

            const std::uint32_t sym_idx = r_info >> 8;
            std::string_view sym_name{};
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

            const char* m_before = "";
            if (val_before >= 0x08000000u) {
                m_before = is_flash(val_before) ? " (*F*)" : " (*R*)";
            }
            const char* m_after = "";
            if (val_after >= 0x08000000u) {
                m_after = is_flash(val_after) ? " (*F*)" : " (*R*)";
            }

            trace_printf("    %-8s GOT[0x%04x] RAM=0x%08x (*R*): 0x%08x%s -> 0x%08x%s  %.*s\n",
                         sn, (unsigned)(r_offset - got_vaddr), (unsigned)slot_ram,
                         (unsigned)val_before, m_before, (unsigned)val_after, m_after,
                         (int)sym_name.size(), sym_name.data());
        }
    }
    trace_printf("==========================================\n");
}
}  // namespace

void xip_walkthrough_complex() {
    trace_printf("\n======================================================================\n");
    trace_printf(" COMPLEX XIP WALKTHROUGH — c1 + c2 + cpp3 (static loader)\n");
    trace_printf("======================================================================\n");

    xip::stat::SModule m_c2, m_cpp3;
    if (!xload("c2.so", m_c2) || !xload("cpp3.so", m_cpp3)) {
        trace_printf("  failed to load c2 or cpp3\n");
        return;
    }

    // [1] Loader placement
    trace_printf("\n[1] Segment Mapping:\n");
    trace_printf("    c2.so:\n");
    trace_printf("      .text/.rodata/PLT  0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(m_c2.text_base), is_flash(m_c2.text_base) ? "(*F*)" : "(*R*)", m_c2.text_size);
    trace_printf("      .got/.data/.bss    0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(m_c2.data_base), is_flash(m_c2.data_base) ? "(*F*)" : "(*R*)", m_c2.data_memsz);
    trace_printf("    cpp3.so:\n");
    trace_printf("      .text/.rodata/PLT  0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(m_cpp3.text_base), is_flash(m_cpp3.text_base) ? "(*F*)" : "(*R*)", m_cpp3.text_size);
    trace_printf("      .got/.data/.bss    0x%08x %s  %u B\n",
                 static_cast<std::uint32_t>(m_cpp3.data_base), is_flash(m_cpp3.data_base) ? "(*F*)" : "(*R*)", m_cpp3.data_memsz);

    // GOT/PLT didactic dumps
    dump_got_walkthrough("c2.so", m_c2);
    dump_got_walkthrough("cpp3.so", m_cpp3);

    // [2] Retrieve parameters from c2
    trace_printf("\n[2] Retrieve parameters from c2:\n");
    std::uint32_t x = C0(m_c2, "c2_param");
    auto* cv = static_cast<volatile std::uint32_t*>(xip::stat::resolve(m_c2, "c2_const"));
    std::uint32_t seed = cv ? *cv : 0u;
    trace_printf("      c2_param()   = %u\n", x);
    trace_printf("      c2_const     = %u\n", seed);

    // [3] Classify symbols in cpp3
    trace_printf("\n[3] Symbol placement in cpp3.so:\n");
    void* fn_method = xip::stat::resolve(m_cpp3, "cpp3_method");
    void* dv_magic = xip::stat::resolve(m_cpp3, "cpp3_magic");
    classify("cpp3_method", reinterpret_cast<std::uint32_t>(fn_method), "code segment");
    classify("cpp3_magic", reinterpret_cast<std::uint32_t>(dv_magic), "data segment");

    // [4] Call cpp3_method using c2's parameters
    trace_printf("\n[4] Execution Proof — call cpp3_method(seed, x):\n");
    std::uint32_t res = C2(m_cpp3, "cpp3_method", seed, x);
    trace_printf("      cpp3_method(%u, %u) = %u\n", seed, x, res);
    trace_printf("      a correct value proves cross-lib parameter passing & execution directly from flash\n");

    xip::stat::unload(m_cpp3);
    xip::stat::unload(m_c2);
    trace_printf("======================================================================\n");
}

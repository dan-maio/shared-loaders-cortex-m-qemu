#include "test-helpers.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"

#ifndef XIP_NS_ALIAS
#error "Define XIP_NS_ALIAS before including this file"
#endif

namespace {
void classify(const char* label, std::uint32_t a, const char* note) {
    trace_printf("      %-18s 0x%08x %s  %s\n",
                 label, a, is_flash(a) ? "(*F*)" : "(*R*)", note);
}

void dump_got_walkthrough(const char* name, std::uintptr_t data_base, std::uintptr_t got_base, std::uintptr_t text_base) {
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

    std::uint32_t text_vaddr = 0;
    std::uint32_t data_vaddr = 0;
    for (loader::elf::half i = 0; i < img.header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (img.program_header(i, ph).has_value()) {
            if (ph.p_type == loader::elf::PT_LOAD) {
                if (ph.p_flags & 0x2) {
                    data_vaddr = ph.p_vaddr;
                } else {
                    text_vaddr = ph.p_vaddr;
                }
            }
        }
    }

    const std::uint32_t got_vaddr = sh_got->sh_addr;
    const std::uint32_t got_words = sh_got->sh_size / 4;
    const std::uintptr_t ram_got_addr = data_base + (got_vaddr - data_vaddr);

    trace_printf("\nGOT/PLT DIDACTIC WALKTHROUGH (%s)\n", name);
    trace_printf("==========================================\n");
    trace_printf("  .got at VMA=0x%08x, RAM=0x%08x (*R*), size=%u words\n",
                 got_vaddr, static_cast<std::uint32_t>(ram_got_addr), got_words);

    auto sh_plt = img.find_section(".plt");
    if (sh_plt && sh_plt->sh_size > 0) {
        std::uintptr_t flash_plt_addr = text_base + (sh_plt->sh_addr - text_vaddr);
        trace_printf("  .plt at VMA=0x%08x, FLASH=0x%08x (*F*), size=%u bytes\n",
                     (unsigned)sh_plt->sh_addr, (unsigned)flash_plt_addr, (unsigned)sh_plt->sh_size);
    } else {
        trace_printf("  .plt (not found)\n");
    }

    using loader::detail::load_word;
    trace_printf("\n  [RAW .got BEFORE] — %u words at RAM=0x%08x:\n",
                 got_words, static_cast<std::uint32_t>(ram_got_addr));
    for (std::uint32_t i = 0; i < got_words; ++i) {
        const std::uint32_t val = load_word(blob, got_vaddr + i * 4);
        trace_printf("    [%2u] 0x%08x\n", i, val);
    }

    trace_printf("\n  [RAM .got AFTER XIP load + relocation] — %u words:\n",
                 got_words);
    for (std::uint32_t i = 0; i < got_words; ++i) {
        const std::uint32_t val = load_word(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(ram_got_addr), got_words * 4}, i * 4);
        trace_printf("    [%2u] 0x%08x  %s\n", i, val,
                     (i < 3) ? "(reserved)" :
                     val == 0 ? "" :
                     is_flash(val) ? "(*F*)" : "(*R*)");
    }
}

}  // namespace

void xip_walkthrough_complex() {
    trace_printf("\n==========================================\n");
    trace_printf(" COMPLEX XIP WALKTHROUGH — c1 + c2 + cpp3\n");
    trace_printf("==========================================\n");

    void* h_c2 = xopen("c2.so");
    void* h_cpp3 = xopen("cpp3.so");
    if (!h_c2 || !h_cpp3) {
        trace_printf("  load c2/cpp3 FAILED\n");
        if (h_c2) XIP_NS_ALIAS::dlclose(h_c2);
        if (h_cpp3) XIP_NS_ALIAS::dlclose(h_cpp3);
        return;
    }

    xip::xip_map m_c2{}, m_cpp3{};
    XIP_NS_ALIAS::get_xip_map(h_c2, m_c2);
    XIP_NS_ALIAS::get_xip_map(h_cpp3, m_cpp3);

    trace_printf("\n  c2  : text=0x%08x data=0x%08x got=0x%08x size=%u+%u\n",
                 static_cast<std::uint32_t>(m_c2.text_base),
                 static_cast<std::uint32_t>(m_c2.data_base),
                 static_cast<std::uint32_t>(m_c2.got_base),
                 m_c2.text_size, m_c2.data_memsz);
    trace_printf("  cpp3: text=0x%08x data=0x%08x got=0x%08x size=%u+%u\n",
                 static_cast<std::uint32_t>(m_cpp3.text_base),
                 static_cast<std::uint32_t>(m_cpp3.data_base),
                 static_cast<std::uint32_t>(m_cpp3.got_base),
                 m_cpp3.text_size, m_cpp3.data_memsz);

    dump_got_walkthrough("c2.so", m_c2.data_base, m_c2.got_base, m_c2.text_base);
    dump_got_walkthrough("cpp3.so", m_cpp3.data_base, m_cpp3.got_base, m_cpp3.text_base);

    auto* cv = static_cast<volatile std::uint32_t*>(XIP_NS_ALIAS::dlsym(h_c2, "c2_const"));
    trace_printf("\n  dlsym(c2, \"c2_const\") -> 0x%08x %s\n",
                 reinterpret_cast<std::uint32_t>(cv),
                 is_flash(reinterpret_cast<std::uint32_t>(cv)) ? "(*F*)" : "(*R*)");
    if (cv) trace_printf("    *c2_const = %u\n", *cv);

    void* fn_method = XIP_NS_ALIAS::dlsym(h_cpp3, "cpp3_method");
    void* dv_magic = XIP_NS_ALIAS::dlsym(h_cpp3, "cpp3_magic");
    classify("cpp3_method", reinterpret_cast<std::uint32_t>(fn_method), "flash code");
    classify("cpp3_magic",  reinterpret_cast<std::uint32_t>(dv_magic), "RAM data");

    trace_printf("\n  c2 hit count (via got_base): %u\n",
                 C0(h_c2, m_c2.got_base, "c2_state"));

    XIP_NS_ALIAS::dlclose(h_cpp3);
    XIP_NS_ALIAS::dlclose(h_c2);
}

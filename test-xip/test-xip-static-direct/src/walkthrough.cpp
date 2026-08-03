/**
 * @file walkthrough.cpp
 * @brief Didactic XIP walkthrough (static loader): for one loaded module, show
 *        WHERE each part lives and WHERE the CPU actually executes it — flash
 *        (R-X, in place) or RAM (RW copy).
 */
#include "test-helpers.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/detail/bytes.hpp"

namespace {

// Print "<label> <addr> <FLASH|RAM> <note>" with the region decided by address.
void classify(const char* label, std::uint32_t a, const char* note) {
    trace_printf("      %-18s 0x%08x  %-5s  %s\n",
                 label, a, is_flash(a) ? "FLASH" : "RAM", note);
}

}  // namespace

void xip_walkthrough() {
    const char* MOD = "slib.so";
    trace_printf("\n======================================================================\n");
    trace_printf(" XIP WALKTHROUGH — where does it live & execute? (%s)\n", MOD);
    trace_printf("======================================================================\n");

    scl::Module m;
    if (!xload(MOD, m)) { trace_printf("  load %s FAILED (is it provisioned?)\n", MOD); return; }

    const auto& sm = m.get_smodule();

    // [1] Where the loader put each segment (static loader = always XIP split).
    trace_printf("\n[1] Loader placement — static loader is XIP-only\n");
    trace_printf("      (R-X in flash, only RW copied to RAM; text never patched)\n");
    trace_printf("      SEGMENT            ADDRESS     REGION  SIZE      disposition\n");
    trace_printf("      .text/.rodata/PLT  0x%08x  %-5s  %5u B   execute-in-place (NOT copied)\n",
                 static_cast<std::uint32_t>(sm.text_base),
                 is_flash(sm.text_base) ? "FLASH" : "RAM", sm.text_size);
    trace_printf("      .got/.data/.bss    0x%08x  %-5s  %5u B   copied to RAM + relocated\n",
                 static_cast<std::uint32_t>(sm.data_base),
                 is_flash(sm.data_base) ? "FLASH" : "RAM", sm.data_memsz);
    trace_printf("      r10 (got_base)     0x%08x  %-5s            passed to pic_call_*\n",
                 static_cast<std::uint32_t>(sm.got_base),
                 is_flash(sm.got_base) ? "FLASH" : "RAM");

    // [2] Resolve symbols and classify each runtime address.
    trace_printf("\n[2] Resolve symbols — classify each runtime address\n");
    void* fn = m.sym("s_add");
    void* dv = m.sym("s_info");
    classify("s_add", reinterpret_cast<std::uint32_t>(fn), "code: CPU fetches it here");
    classify("s_info (data)", reinterpret_cast<std::uint32_t>(dv), "RW global: in the RAM copy");

    // Retrieve and classify .got, .got.plt, and .plt section addresses
    const fw::FsEntry* e = fw::fs_find(MOD);
    if (e) {
        std::span<const std::byte> blob{ fw::fs_blob(*e), e->size };
        loader::memory_byte_source src(blob);
        auto parsed = loader::elf_image::parse(src);
        if (parsed) {
            auto img = *parsed;
            auto sh_got = img.find_section(".got");
            auto sh_plt = img.find_section(".plt");

            auto map_vaddr = [&](std::uint32_t vaddr) -> std::uintptr_t {
                if (vaddr >= sm.data_vaddr && vaddr < sm.data_vaddr + sm.data_memsz) {
                    return sm.data_base + (vaddr - sm.data_vaddr);
                } else if (vaddr >= sm.text_vaddr && vaddr < sm.text_vaddr + sm.text_size) {
                    return sm.text_base + (vaddr - sm.text_vaddr);
                }
                return 0;
            };

            trace_printf("\n    Section locations & segment bases from ELF:\n");
            if (sh_got) {
                std::uint32_t sec_addr = static_cast<std::uint32_t>(map_vaddr(sh_got->sh_addr));
                trace_printf("      %-18s 0x%08x  %-5s  (segment base: 0x%08x)\n",
                             ".got", sec_addr, is_flash(sec_addr) ? "FLASH" : "RAM",
                             static_cast<std::uint32_t>(sm.data_base));
            } else {
                trace_printf("      %-18s (not found)\n", ".got");
            }
            if (sh_plt) {
                std::uint32_t sec_addr = static_cast<std::uint32_t>(map_vaddr(sh_plt->sh_addr));
                trace_printf("      %-18s 0x%08x  %-5s  (segment base: 0x%08x)\n",
                             ".plt", sec_addr, is_flash(sec_addr) ? "FLASH" : "RAM",
                             static_cast<std::uint32_t>(sm.text_base));
            } else {
                trace_printf("      %-18s (not found)\n", ".plt");
            }
        }
    }

    // [3] Prove the execution address by actually calling the flash function.
    trace_printf("\n[3] Proof — call the flash-resident function\n");
    const std::uint32_t r = call_sym2(m, "s_add", 10, 5);
    trace_printf("      s_add(10, 5) = %u  (entry 0x%08x is %s)\n",
                 r, reinterpret_cast<std::uint32_t>(fn),
                 is_flash(reinterpret_cast<std::uint32_t>(fn)) ? "FLASH" : "RAM");
    trace_printf("      a correct return proves the core executed it straight from flash\n");

    // [4] The XIP saving this module buys.
    const std::uint32_t total = sm.text_size + sm.data_memsz;
    const std::uint32_t pct   = total ? (sm.text_size * 100u) / total : 0;
    trace_printf("\n[4] XIP saving for %s\n", MOD);
    trace_printf("      %u of %u B (%u%%) stay in flash, NEVER copied to RAM\n",
                 sm.text_size, total, pct);
    trace_printf("      only %u B (the RW segment) consumes SRAM\n", sm.data_memsz);
    trace_printf("======================================================================\n");

    m = scl::Module{};
}

#include "test-helpers.hpp"

#ifndef XIP_NS_ALIAS
#error "Define XIP_NS_ALIAS before including this file"
#endif

namespace {

void classify(const char* label, std::uint32_t a, const char* note) {
    trace_printf("      %-18s 0x%08x  %-5s  %s\n",
                 label, a, is_flash(a) ? "FLASH" : "RAM", note);
}

}  // namespace

void xip_walkthrough() {
    const char* MOD = "mylib1.so";
    trace_printf("\n======================================================================\n");
    trace_printf(" XIP WALKTHROUGH — where does it live & execute? (%s)\n", MOD);
    trace_printf("======================================================================\n");

    void* h = xopen(MOD);
    if (!h) { trace_printf("  load %s FAILED (is it provisioned?)\n", MOD); return; }

    xip::xip_map m{};
    if (!XIP_NS_ALIAS::get_xip_map(h, m)) {
        trace_printf("  no segment map for %s\n", MOD);
        XIP_NS_ALIAS::dlclose(h);
        return;
    }

    trace_printf("\n[1] Loader placement — is_xip = %s\n",
                 m.is_xip ? "YES (R-X in flash, only RW copied to RAM)"
                          : "NO (copy-whole: whole image in RAM)");
    trace_printf("      SEGMENT            ADDRESS     REGION  SIZE      disposition\n");
    trace_printf("      .text/.rodata/PLT  0x%08x  %-5s  %5u B   execute-in-place (NOT copied)\n",
                 static_cast<std::uint32_t>(m.text_base),
                 is_flash(m.text_base) ? "FLASH" : "RAM", m.text_size);
    trace_printf("      .got/.data/.bss    0x%08x  %-5s  %5u B   copied to RAM + relocated\n",
                 static_cast<std::uint32_t>(m.data_base),
                 is_flash(m.data_base) ? "FLASH" : "RAM", m.data_memsz);
    trace_printf("      r10 (got_base)     0x%08x  %-5s            passed to pic_call_*\n",
                 static_cast<std::uint32_t>(m.got_base),
                 is_flash(m.got_base) ? "FLASH" : "RAM");

    trace_printf("\n[2] Resolve symbols — classify each runtime address\n");
    void* fn = XIP_NS_ALIAS::dlsym(h, "library_function");
    void* dv = XIP_NS_ALIAS::dlsym(h, "s_lib");
    classify("library_function", reinterpret_cast<std::uint32_t>(fn), "code: CPU fetches it here");
    classify("s_lib (data)",     reinterpret_cast<std::uint32_t>(dv), "RW global: in the RAM copy");

    trace_printf("\n[3] Proof — call the flash-resident function\n");
    const std::uint32_t r = C1(h, m.got_base, "library_function", 39);
    trace_printf("      library_function(39) = %u  (entry 0x%08x is %s)\n",
                 r, reinterpret_cast<std::uint32_t>(fn),
                 is_flash(reinterpret_cast<std::uint32_t>(fn)) ? "FLASH" : "RAM");
    trace_printf("      a correct return proves the core executed it straight from flash\n");

    const std::uint32_t total = m.text_size + m.data_memsz;
    const std::uint32_t pct   = total ? (m.text_size * 100u) / total : 0;
    trace_printf("\n[4] XIP saving for %s\n", MOD);
    trace_printf("      %u of %u B (%u%%) stay in flash, NEVER copied to RAM\n",
                 m.text_size, total, pct);
    trace_printf("      only %u B (the RW segment) consumes SRAM\n", m.data_memsz);
    trace_printf("======================================================================\n");

    XIP_NS_ALIAS::dlclose(h);
}

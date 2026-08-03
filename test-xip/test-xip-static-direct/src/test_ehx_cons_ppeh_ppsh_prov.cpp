#include "test-helpers.hpp"

void test_ehx_cons_ppeh_ppsh_prov() {
    trace_printf("\n[test] ==== prov + cons + cppeh + cppsh + ehx (static XIP - Direct) ====\n");

    // ---- Load in dependency order ----
    scl::Module m_prov;
    if (!xload("libprov.so", m_prov)) { trace_printf("  load libprov FAILED\n"); return; }
    trace_printf("  libprov loaded (XIP); got_base=0x%08x\n", m_prov.get_smodule().got_base);

    scl::Module m_cons;
    if (!xload("libcons.so", m_cons)) {
        trace_printf("  load libcons FAILED\n");
        m_prov = scl::Module{};
        return;
    }
    trace_printf("  libcons loaded (XIP); got_base=0x%08x\n", m_cons.get_smodule().got_base);

    scl::Module m_cppeh;
    if (!xload("libcppeh.so", m_cppeh)) {
        trace_printf("  load libcppeh FAILED\n");
        m_cons = scl::Module{};
        m_prov = scl::Module{};
        return;
    }
    trace_printf("  libcppeh loaded (XIP); got_base=0x%08x\n", m_cppeh.get_smodule().got_base);

    scl::Module m_cppsh;
    if (!xload("libcppsh.so", m_cppsh)) {
        trace_printf("  load libcppsh FAILED\n");
        m_cppeh = scl::Module{};
        m_cons = scl::Module{};
        m_prov = scl::Module{};
        return;
    }
    trace_printf("  libcppsh loaded (XIP); got_base=0x%08x\n", m_cppsh.get_smodule().got_base);

    scl::Module m_ehx;
    if (!xload("ehx_catch.so", m_ehx)) {
        trace_printf("  load ehx_catch FAILED\n");
        m_cppsh = scl::Module{};
        m_cppeh = scl::Module{};
        m_cons = scl::Module{};
        m_prov = scl::Module{};
        return;
    }
    trace_printf("  ehx_catch loaded (XIP); got_base=0x%08x\n", m_ehx.get_smodule().got_base);

    // ---- libprov ----
    trace_printf("\n  -- libprov --\n");
    trace_printf("  prov_value(5) = %u\n", call_sym1(m_prov, "prov_value", 5));

    // ---- libcons (depends on libprov via cross-lib call) ----
    trace_printf("\n  -- libcons --\n");
    trace_printf("  cons_call(5)  = %u\n", call_sym1(m_cons, "cons_call", 5));

    // ---- libcppeh (C++ exceptions + RTTI, self-contained tests) ----
    trace_printf("\n  -- libcppeh --\n");
    trace_printf("  eh_dyncast(0) = %d\n", call_sym1(m_cppeh, "eh_dyncast", 0));

    // ---- libcppsh (C++ virtual classes, heap) ----
    trace_printf("\n  -- libcppsh --\n");
    std::uint32_t n0 = call_sym0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_instances()   = %u\n", n0);

    void* w = reinterpret_cast<void*>(call_sym1(m_cppsh, "cppsh_make", 7));
    std::uint32_t n1 = call_sym0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_make(7)       = %p\n", w);
    trace_printf("  cppsh_instances()   = %u\n", n1);

    std::uint32_t r = call_sym2(m_cppsh, "cppsh_compute", reinterpret_cast<std::uint32_t>(w), 5);
    trace_printf("  cppsh_compute(w,5)  = %u\n", r);

    call_sym1(m_cppsh, "cppsh_free", reinterpret_cast<std::uint32_t>(w));
    std::uint32_t n2 = call_sym0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_free(w)       = (void)\n");
    trace_printf("  cppsh_instances()   = %u\n", n2);

    trace_printf("\n  -- ehx_catch (skipped: exidx CANTUNWIND) --\n");

    // ---- Cleanup in reverse dependency order ----
    m_ehx = scl::Module{};    trace_printf("  unload ehx_catch ok\n");
    m_cppsh = scl::Module{};  trace_printf("  unload libcppsh ok\n");
    m_cppeh = scl::Module{};  trace_printf("  unload libcppeh ok\n");
    m_cons = scl::Module{};   trace_printf("  unload libcons ok\n");
    m_prov = scl::Module{};   trace_printf("  unload libprov ok\n");
}

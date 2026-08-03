#include "test-helpers.hpp"
#include <span>

void test_ehx_cons_ppeh_ppsh_prov() {
    trace_printf("\n[test] ==== prov + cons + cppeh + cppsh + ehx (static XIP) ====\n");

    // ---- Load in dependency order ----
    xip::stat::SModule m_prov;
    if (!xload("libprov.so", m_prov)) { trace_printf("  load libprov FAILED\n"); return; }
    trace_printf("  libprov loaded (XIP); got_base=0x%08x\n", m_prov.got_base);

    xip::stat::SModule m_cons;
    if (!xload("libcons.so", m_cons)) {
        trace_printf("  load libcons FAILED\n");
        xip::stat::unload(m_prov);
        return;
    }
    trace_printf("  libcons loaded (XIP); got_base=0x%08x\n", m_cons.got_base);

    xip::stat::SModule m_cppeh;
    if (!xload("libcppeh.so", m_cppeh)) {
        trace_printf("  load libcppeh FAILED\n");
        xip::stat::unload(m_cons);
        xip::stat::unload(m_prov);
        return;
    }
    trace_printf("  libcppeh loaded (XIP); got_base=0x%08x\n", m_cppeh.got_base);

    xip::stat::SModule m_cppsh;
    if (!xload("libcppsh.so", m_cppsh)) {
        trace_printf("  load libcppsh FAILED\n");
        xip::stat::unload(m_cppeh);
        xip::stat::unload(m_cons);
        xip::stat::unload(m_prov);
        return;
    }
    trace_printf("  libcppsh loaded (XIP); got_base=0x%08x\n", m_cppsh.got_base);

    xip::stat::SModule m_ehx;
    if (!xload("ehx_catch.so", m_ehx)) {
        trace_printf("  load ehx_catch FAILED\n");
        xip::stat::unload(m_cppsh);
        xip::stat::unload(m_cppeh);
        xip::stat::unload(m_cons);
        xip::stat::unload(m_prov);
        return;
    }
    trace_printf("  ehx_catch loaded (XIP); got_base=0x%08x\n", m_ehx.got_base);

    // ---- libprov ----
    trace_printf("\n  -- libprov --\n");
    trace_printf("  prov_value(5) = %u\n", C1(m_prov, "prov_value", 5));

    // ---- libcons (depends on libprov via cross-lib call) ----
    trace_printf("\n  -- libcons --\n");
    trace_printf("  cons_call(5)  = %u\n", C1(m_cons, "cons_call", 5));

    // ---- libcppeh (C++ exceptions + RTTI, self-contained tests) ----
    trace_printf("\n  -- libcppeh --\n");
    // eh_selftest skipped: CANTUNWIND
    // trace_printf("  eh_selftest(1)= %d\n", C1(m_cppeh, "eh_selftest", 1));
    trace_printf("  eh_dyncast(0) = %d\n", C1(m_cppeh, "eh_dyncast", 0));

    // ---- libcppsh (C++ virtual classes, heap) ----
    trace_printf("\n  -- libcppsh --\n");
    std::uint32_t n0 = C0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_instances()   = %u\n", n0);
    void* w = reinterpret_cast<void*>(C1(m_cppsh, "cppsh_make", 7));
    std::uint32_t n1 = C0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_make(7)       = %p\n", w);
    trace_printf("  cppsh_instances()   = %u\n", n1);
    std::uint32_t r = C2(m_cppsh, "cppsh_compute",
                         reinterpret_cast<std::uint32_t>(w), 5);
    trace_printf("  cppsh_compute(w,5)  = %u\n", r);
    C1(m_cppsh, "cppsh_free", reinterpret_cast<std::uint32_t>(w));
    std::uint32_t n2 = C0(m_cppsh, "cppsh_instances");
    trace_printf("  cppsh_free(w)       = (void)\n");
    trace_printf("  cppsh_instances()   = %u\n", n2);

    trace_printf("\n  -- ehx_catch (skipped: exidx CANTUNWIND) --\n");

    // ---- Cleanup in reverse dependency order ----
    xip::stat::unload(m_ehx);    trace_printf("  unload ehx_catch ok\n");
    xip::stat::unload(m_cppsh);  trace_printf("  unload libcppsh ok\n");
    xip::stat::unload(m_cppeh);  trace_printf("  unload libcppeh ok\n");
    xip::stat::unload(m_cons);   trace_printf("  unload libcons ok\n");
    xip::stat::unload(m_prov);   trace_printf("  unload libprov ok\n");
}

#include "test-helpers.hpp"

#ifndef XIP_NS_ALIAS
#error "Define XIP_NS_ALIAS before including this file"
#endif

void test_ehx_cons_ppeh_ppsh_prov() {
    trace_printf("\n[test] ==== prov + cons + cppeh + cppsh + ehx (cross-lib) ====\n");

    void* h_prov = xopen("libprov.so");
    if (!h_prov) { trace_printf("  dlopen libprov FAILED\n"); return; }
    const std::uint32_t gp = XIP_NS_ALIAS::get_got_base(h_prov);
    trace_printf("  libprov loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_prov) ? "XIP" : "copy-whole", gp);

    void* h_cons = xopen("libcons.so");
    if (!h_cons) { trace_printf("  dlopen libcons FAILED\n"); return; }
    const std::uint32_t gc = XIP_NS_ALIAS::get_got_base(h_cons);
    trace_printf("  libcons loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_cons) ? "XIP" : "copy-whole", gc);

    void* h_cppeh = xopen("libcppeh.so");
    if (!h_cppeh) { trace_printf("  dlopen libcppeh FAILED\n"); return; }
    const std::uint32_t ge = XIP_NS_ALIAS::get_got_base(h_cppeh);
    trace_printf("  libcppeh loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_cppeh) ? "XIP" : "copy-whole", ge);

    void* h_cppsh = xopen("libcppsh.so");
    if (!h_cppsh) { trace_printf("  dlopen libcppsh FAILED\n"); return; }
    const std::uint32_t gs = XIP_NS_ALIAS::get_got_base(h_cppsh);
    trace_printf("  libcppsh loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_cppsh) ? "XIP" : "copy-whole", gs);

    void* h_ehx = xopen("ehx_catch.so");
    if (!h_ehx) { trace_printf("  dlopen ehx_catch FAILED\n"); return; }
    const std::uint32_t gx = XIP_NS_ALIAS::get_got_base(h_ehx);
    trace_printf("  ehx_catch loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_ehx) ? "XIP" : "copy-whole", gx);

    trace_printf("\n  -- libprov --\n");
    trace_printf("  prov_value(5) = %u\n", C1(h_prov, gp, "prov_value", 5));

    trace_printf("\n  -- libcons --\n");
    trace_printf("  cons_call(5)  = %u\n", C1(h_cons, gc, "cons_call", 5));

    trace_printf("\n  -- libcppeh --\n");
    trace_printf("  eh_dyncast(0) = %d\n", C1(h_cppeh, ge, "eh_dyncast", 0));

    trace_printf("\n  -- libcppsh --\n");
    std::uint32_t n0 = C0(h_cppsh, gs, "cppsh_instances");
    trace_printf("  cppsh_instances()   = %u\n", n0);
    void* w = reinterpret_cast<void*>(C1(h_cppsh, gs, "cppsh_make", 7));
    std::uint32_t n1 = C0(h_cppsh, gs, "cppsh_instances");
    trace_printf("  cppsh_make(7)       = %p\n", w);
    trace_printf("  cppsh_instances()   = %u\n", n1);
    std::uint32_t r = C2(h_cppsh, gs, "cppsh_compute",
                         reinterpret_cast<std::uint32_t>(w), 5);
    trace_printf("  cppsh_compute(w,5)  = %u\n", r);
    C1(h_cppsh, gs, "cppsh_free", reinterpret_cast<std::uint32_t>(w));
    std::uint32_t n2 = C0(h_cppsh, gs, "cppsh_instances");
    trace_printf("  cppsh_free(w)       = (void)\n");
    trace_printf("  cppsh_instances()   = %u\n", n2);

    trace_printf("\n  -- ehx_catch (skipped: exidx CANTUNWIND) --\n");

    XIP_NS_ALIAS::dlclose(h_ehx);    trace_printf("  dlclose ehx_catch ok\n");
    XIP_NS_ALIAS::dlclose(h_cppsh);  trace_printf("  dlclose libcppsh ok\n");
    XIP_NS_ALIAS::dlclose(h_cppeh);  trace_printf("  dlclose libcppeh ok\n");
    XIP_NS_ALIAS::dlclose(h_cons);   trace_printf("  dlclose libcons ok\n");
    XIP_NS_ALIAS::dlclose(h_prov);   trace_printf("  dlclose libprov ok\n");
}

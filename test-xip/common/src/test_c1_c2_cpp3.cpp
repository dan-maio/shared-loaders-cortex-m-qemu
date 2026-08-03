#include "test-helpers.hpp"

#ifndef XIP_NS_ALIAS
#error "Define XIP_NS_ALIAS to the namespace alias (e.g. 'xip_ns') before including this file"
#endif

extern "C" {
extern std::uint32_t fm_var;
extern struct FmInfo { std::uint32_t id; std::uint32_t flags; } fm_info;
}

void test_c1_c2_cpp3() {
    trace_printf("\n[test] ==== c1 + c2 + cpp3 (XIP, cross-lib) ====\n");

    void* h_c2 = xopen("c2.so");
    if (!h_c2) { trace_printf("  dlopen c2 FAILED\n"); return; }
    const std::uint32_t g2 = XIP_NS_ALIAS::get_got_base(h_c2);
    trace_printf("  c2 loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_c2) ? "XIP" : "copy-whole", g2);
    void* lf2 = XIP_NS_ALIAS::dlsym(h_c2, "c2_add");
    trace_printf("  c2_add @ 0x%08x (%s)\n",
                 reinterpret_cast<std::uint32_t>(lf2),
                 is_flash(reinterpret_cast<std::uint32_t>(lf2)) ? "FLASH" : "RAM");

    void* h_c1 = xopen("c1.so");
    if (!h_c1) { trace_printf("  dlopen c1 FAILED\n"); return; }
    const std::uint32_t g1 = XIP_NS_ALIAS::get_got_base(h_c1);
    trace_printf("  c1 loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_c1) ? "XIP" : "copy-whole", g1);

    void* h_cpp3 = xopen("cpp3.so");
    if (!h_cpp3) { trace_printf("  dlopen cpp3 FAILED\n"); return; }
    const std::uint32_t g3 = XIP_NS_ALIAS::get_got_base(h_cpp3);
    trace_printf("  cpp3 loaded (%s); got_base=0x%08x\n",
                 XIP_NS_ALIAS::is_xip(h_cpp3) ? "XIP" : "copy-whole", g3);

    trace_printf("\n  -- c2 --\n");
    trace_printf("  c2_data            = %u (0x%04x)\n", RD(h_c2, "c2_data"), RD(h_c2, "c2_data"));
    trace_printf("  c2_const           = %u (0x%04x)\n", RD(h_c2, "c2_const"), RD(h_c2, "c2_const"));
    trace_printf("  c2_tag             = \"%s\"\n", RD_STR(h_c2, "c2_tag"));
    trace_printf("  c2_state()         = %u\n", C0(h_c2, g2, "c2_state"));
    trace_printf("  c2_param()         = %u\n", C0(h_c2, g2, "c2_param"));
    trace_printf("  c2_add(10, 20)     = %u\n", C2(h_c2, g2, "c2_add", 10, 20));
    trace_printf("  c2_state()         = %u\n", C0(h_c2, g2, "c2_state"));

    trace_printf("\n  -- c1 --\n");
    trace_printf("  c1_data            = %u (0x%04x)\n", RD(h_c1, "c1_data"), RD(h_c1, "c1_data"));
    trace_printf("  c1_tag             = \"%s\"\n", RD_STR(h_c1, "c1_tag"));
    trace_printf("  c1_state()         = %u\n", C0(h_c1, g1, "c1_state"));
    trace_printf("  c1_call_c2(5)      = %u\n", C1(h_c1, g1, "c1_call_c2", 5));
    trace_printf("  c1_state()         = %u\n", C0(h_c1, g1, "c1_state"));
    trace_printf("  c1_use_fm()        = %u\n", C0(h_c1, g1, "c1_use_fm"));

    trace_printf("\n  -- cpp3 --\n");
    trace_printf("  cpp3_magic         = %u (0x%04x)\n", RD(h_cpp3, "cpp3_magic"), RD(h_cpp3, "cpp3_magic"));
    trace_printf("  cpp3_var           = %u (0x%04x)\n", RD(h_cpp3, "cpp3_var"), RD(h_cpp3, "cpp3_var"));
    trace_printf("  cpp3_calls()       = %u\n", C0(h_cpp3, g3, "cpp3_calls"));
    trace_printf("  cpp3_made()        = %u\n", C0(h_cpp3, g3, "cpp3_made"));
    trace_printf("  cpp3_method(2,100) = %u\n", C2(h_cpp3, g3, "cpp3_method", 2, 100));
    trace_printf("  cpp3_calls()       = %u\n", C0(h_cpp3, g3, "cpp3_calls"));
    trace_printf("  cpp3_heap(8)       = %u\n", C1(h_cpp3, g3, "cpp3_heap", 8));
    trace_printf("  cpp3_orchestrate(5)= %u\n", C1(h_cpp3, g3, "cpp3_orchestrate", 5));
    trace_printf("  cpp3_recurse(5)    = %u\n", C1(h_cpp3, g3, "cpp3_recurse", 5));
    trace_printf("  cpp3_float_cross(10)= %u\n", C1(h_cpp3, g3, "cpp3_float_cross", 10));

    XIP_NS_ALIAS::dlclose(h_cpp3);  trace_printf("  dlclose cpp3 ok\n");
    XIP_NS_ALIAS::dlclose(h_c1);    trace_printf("  dlclose c1 ok\n");
    XIP_NS_ALIAS::dlclose(h_c2);    trace_printf("  dlclose c2 ok\n");
}

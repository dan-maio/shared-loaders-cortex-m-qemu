#include "test-helpers.hpp"
#include <span>

void test_c1_c2_cpp3() {
    trace_printf("\n[test] ==== c1 + c2 + cpp3 (static XIP) ====\n");

    xip::stat::SModule m_c2;
    if (!xload("c2.so", m_c2)) { trace_printf("  load c2 FAILED\n"); return; }
    trace_printf("  c2 loaded (XIP); got_base=0x%08x\n", m_c2.got_base);
    void* lf2 = xip::stat::resolve(m_c2, "c2_add");
    trace_printf("  c2_add @ 0x%08x (%s)\n",
                 reinterpret_cast<std::uint32_t>(lf2),
                 is_flash(reinterpret_cast<std::uint32_t>(lf2)) ? "FLASH" : "RAM");

    xip::stat::SModule m_c1;
    if (!xload("c1.so", m_c1)) { trace_printf("  load c1 FAILED\n"); xip::stat::unload(m_c2); return; }
    trace_printf("  c1 loaded (XIP); got_base=0x%08x\n", m_c1.got_base);

    xip::stat::SModule m_cpp3;
    if (!xload("cpp3.so", m_cpp3)) {
        trace_printf("  load cpp3 FAILED\n");
        xip::stat::unload(m_c1);
        xip::stat::unload(m_c2);
        return;
    }
    trace_printf("  cpp3 loaded (XIP); got_base=0x%08x\n", m_cpp3.got_base);

    // ---- c2 surface ----
    trace_printf("\n  -- c2 --\n");
    trace_printf("  c2_data            = %u (0x%04x)\n", RD(m_c2, "c2_data"), RD(m_c2, "c2_data"));
    trace_printf("  c2_const           = %u (0x%04x)\n", RD(m_c2, "c2_const"), RD(m_c2, "c2_const"));
    trace_printf("  c2_tag             = \"%s\"\n", RD_STR(m_c2, "c2_tag"));
    trace_printf("  c2_state()         = %u\n", C0(m_c2, "c2_state"));
    trace_printf("  c2_param()         = %u\n", C0(m_c2, "c2_param"));
    trace_printf("  c2_add(10, 20)     = %u\n", C2(m_c2, "c2_add", 10, 20));
    trace_printf("  c2_state()         = %u\n", C0(m_c2, "c2_state"));

    // ---- c1 surface ----
    trace_printf("\n  -- c1 --\n");
    trace_printf("  c1_data            = %u (0x%04x)\n", RD(m_c1, "c1_data"), RD(m_c1, "c1_data"));
    trace_printf("  c1_tag             = \"%s\"\n", RD_STR(m_c1, "c1_tag"));
    trace_printf("  c1_state()         = %u\n", C0(m_c1, "c1_state"));
    trace_printf("  c1_call_c2(5)      = %u\n", C1(m_c1, "c1_call_c2", 5));
    trace_printf("  c1_use_fm()        = %u\n", C0(m_c1, "c1_use_fm"));

    // ---- cpp3 surface ----
    trace_printf("\n  -- cpp3 --\n");
    trace_printf("  cpp3_magic         = %u (0x%04x)\n", RD(m_cpp3, "cpp3_magic"), RD(m_cpp3, "cpp3_magic"));
    trace_printf("  cpp3_var           = %u (0x%04x)\n", RD(m_cpp3, "cpp3_var"), RD(m_cpp3, "cpp3_var"));
    trace_printf("  cpp3_calls()       = %u\n", C0(m_cpp3, "cpp3_calls"));
    trace_printf("  cpp3_made()        = %u\n", C0(m_cpp3, "cpp3_made"));
    trace_printf("  cpp3_method(2,100) = %u\n", C2(m_cpp3, "cpp3_method", 2, 100));
    trace_printf("  cpp3_calls()       = %u\n", C0(m_cpp3, "cpp3_calls"));
    trace_printf("  cpp3_heap(8)       = %u\n", C1(m_cpp3, "cpp3_heap", 8));
    trace_printf("  cpp3_orchestrate(5)= %u\n", C1(m_cpp3, "cpp3_orchestrate", 5));
    trace_printf("  cpp3_recurse(5)    = %u\n", C1(m_cpp3, "cpp3_recurse", 5));
    trace_printf("  cpp3_float_cross(10)= %u\n", C1(m_cpp3, "cpp3_float_cross", 10));

    // Cleanup.
    xip::stat::unload(m_cpp3);  trace_printf("  unload cpp3 ok\n");
    xip::stat::unload(m_c1);    trace_printf("  unload c1 ok\n");
    xip::stat::unload(m_c2);    trace_printf("  unload c2 ok\n");
}

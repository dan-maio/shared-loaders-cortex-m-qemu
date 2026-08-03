#include "test-helpers.hpp"

void test_c1_c2_cpp3() {
    trace_printf("\n[test] ==== c1 + c2 + cpp3 (static XIP - Direct) ====\n");

    scl::Module m_c2;
    if (!xload("c2.so", m_c2)) { trace_printf("  load c2 FAILED\n"); return; }
    trace_printf("  c2 loaded (XIP); got_base=0x%08x\n", m_c2.get_smodule().got_base);

    scl::Module m_c1;
    if (!xload("c1.so", m_c1)) { trace_printf("  load c1 FAILED\n"); m_c2 = scl::Module{}; return; }
    trace_printf("  c1 loaded (XIP); got_base=0x%08x\n", m_c1.get_smodule().got_base);

    scl::Module m_cpp3;
    if (!xload("cpp3.so", m_cpp3)) {
        trace_printf("  load cpp3 FAILED\n");
        m_c1 = scl::Module{};
        m_c2 = scl::Module{};
        return;
    }
    trace_printf("  cpp3 loaded (XIP); got_base=0x%08x\n", m_cpp3.get_smodule().got_base);

    // ---- c2 surface ----
    trace_printf("\n  -- c2 --\n");
    trace_printf("  c2_data            = %u (0x%04x)\n", rd(m_c2, "c2_data"), rd(m_c2, "c2_data"));
    trace_printf("  c2_const           = %u (0x%04x)\n", rd(m_c2, "c2_const"), rd(m_c2, "c2_const"));
    trace_printf("  c2_tag             = \"%s\"\n", rd_str(m_c2, "c2_tag"));
    trace_printf("  c2_state()         = %u\n", call_sym0(m_c2, "c2_state"));
    trace_printf("  c2_param()         = %u\n", call_sym0(m_c2, "c2_param"));
    trace_printf("  c2_add(10, 20)     = %u\n", call_sym2(m_c2, "c2_add", 10, 20));
    trace_printf("  c2_state()         = %u\n", call_sym0(m_c2, "c2_state"));

    // ---- c1 surface ----
    trace_printf("\n  -- c1 --\n");
    trace_printf("  c1_data            = %u (0x%04x)\n", rd(m_c1, "c1_data"), rd(m_c1, "c1_data"));
    trace_printf("  c1_tag             = \"%s\"\n", rd_str(m_c1, "c1_tag"));
    trace_printf("  c1_state()         = %u\n", call_sym0(m_c1, "c1_state"));
    trace_printf("  c1_call_c2(5)      = %u\n", call_sym1(m_c1, "c1_call_c2", 5));
    trace_printf("  c1_use_fm()        = %u\n", call_sym0(m_c1, "c1_use_fm"));

    // ---- cpp3 surface ----
    trace_printf("\n  -- cpp3 --\n");
    trace_printf("  cpp3_magic         = %u (0x%04x)\n", rd(m_cpp3, "cpp3_magic"), rd(m_cpp3, "cpp3_magic"));
    trace_printf("  cpp3_var           = %u (0x%04x)\n", rd(m_cpp3, "cpp3_var"), rd(m_cpp3, "cpp3_var"));
    trace_printf("  cpp3_calls()       = %u\n", call_sym0(m_cpp3, "cpp3_calls"));
    trace_printf("  cpp3_made()        = %u\n", call_sym0(m_cpp3, "cpp3_made"));
    trace_printf("  cpp3_method(2,100) = %u\n", call_sym2(m_cpp3, "cpp3_method", 2, 100));
    trace_printf("  cpp3_calls()       = %u\n", call_sym0(m_cpp3, "cpp3_calls"));
    trace_printf("  cpp3_heap(8)       = %u\n", call_sym1(m_cpp3, "cpp3_heap", 8));
    trace_printf("  cpp3_orchestrate(5)= %u\n", call_sym1(m_cpp3, "cpp3_orchestrate", 5));
    trace_printf("  cpp3_recurse(5)    = %u\n", call_sym1(m_cpp3, "cpp3_recurse", 5));
    trace_printf("  cpp3_float_cross(10)= %u\n", call_sym1(m_cpp3, "cpp3_float_cross", 10));

    // Cleanup
    m_cpp3 = scl::Module{};  trace_printf("  unload cpp3 ok\n");
    m_c1 = scl::Module{};    trace_printf("  unload c1 ok\n");
    m_c2 = scl::Module{};    trace_printf("  unload c2 ok\n");
}

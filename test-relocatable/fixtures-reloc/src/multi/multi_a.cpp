/**
 * @file multi_a.cpp
 * @brief Front file: the exported entry points + a constructor. Demonstrates a
 *        call chain a -> b -> c across three files (all resolved internally by
 *        ld -r), a shared global reused across files, and a .init_array entry —
 *        the whole thing collapses into ONE multi.rel.o the loader sees.
 */
extern "C" { extern int multi_shared; }        // declared here, defined in multi_c
int b_double(int);                             // from multi_b (UND until -r)

extern "C" void trace_u32(const char*, unsigned);   // host import

// Constructor -> .init_array. Runs once when the loader loads the bundle.
__attribute__((constructor)) static void multi_ctor() {
    multi_shared += 5;                         // 100 -> 105
    trace_u32("  [multi] ctor multi_shared=", (unsigned)multi_shared);
}

// Exported: a -> b_double -> c_add (+ shared global). Deterministic.
extern "C" unsigned multi_entry(unsigned x) {
    trace_u32("  [multi] entry x=", x);
    return (unsigned)(b_double((int)x) + multi_shared);
}

// Exported: read the shared global (proves the cross-file data merge).
extern "C" unsigned multi_get_shared() { return (unsigned)multi_shared; }

// Per-instance mutable state in .bss (zeroed at load). Used by the
// two-instances and reload tests: each loaded copy has its own multi_hits.
static unsigned multi_hits = 0;
extern "C" unsigned multi_bump()     { return ++multi_hits; }
extern "C" unsigned multi_hits_get() { return multi_hits; }

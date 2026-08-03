/**
 * @file tests-dyn.cpp
 * @brief Dynamic variant test functions — included from main.cpp via #include
 * All functions use dlopen/dlsym to load and test libraries at runtime.
 */
/* ================================================================== */
/** @brief Load mylib1.so and test all exported functions. */
/** @brief Load mylib1.so, test 15+ functions, all 5 ARM reloc types. */
static void test_mylib1(bool& ok) {
    trace_puts("--- mylib1.so ---");
    auto h = dl::dlopen("/tmp/mylib1.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    test_u1(h, ok, "library_function",  42);
    test_u1(h, ok, "library_function2", 42);
    test_u1(h, ok, "library_function3", 3);
    test_u1(h, ok, "g",  5);
    test_u1(h, ok, "fff", 7);
    test_u0(h, ok, "boo"); test_u0(h, ok, "boo2"); test_u0(h, ok, "foo");
    test_u0(h, ok, "lkj"); test_u1(h, ok, "callback_f", 7);
    test_s0(h, ok, "retstr"); test_s0(h, ok, "retstr_"); test_v0(h, ok, "print_lib");
    auto g_fn  = (unsigned(*)(unsigned))dl::dlsym(h, "g");
    auto p_fn  = (unsigned(*)(unsigned(*)(unsigned),unsigned))dl::dlsym(h, "ptrfunc");
    auto gg_fn = (unsigned(*)(unsigned))dl::dlsym(h, "gg");
    if (g_fn && p_fn) trace_printf("  ptrfunc(g,5) = %u\n", p_fn(g_fn, 5));
    if (gg_fn)        trace_printf("  gg(5) = %u\n", gg_fn(5));
    auto rsl = (SL(*)())dl::dlsym(h, "ret_struct_lib");
    auto rs  = (SL(*)(SL))dl::dlsym(h, "ret_struct");
    if (rsl && rs) { SL s = rsl(); s.b += 5; SL s2 = rs(s); trace_printf("  ret_struct(s).b = %u\n", s2.b); }
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Load mylib2.so, test 5 functions, cross-lib calls. */
static void test_mylib2(bool& ok) {
    trace_puts("--- mylib2.so ---");
    auto h = dl::dlopen("/tmp/mylib2.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    test_u1(h, ok, "lf", 42); test_u1(h, ok, "lf2", 42); test_u1(h, ok, "lf3", 3);
    test_u0(h, ok, "mm"); test_u0(h, ok, "xxx");
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Load libcppsh.so, test C++ vtable/ctor/heap. */
static void test_libcppsh(bool& ok) {
    trace_puts("--- libcppsh.so ---");
    auto h = dl::dlopen("/tmp/libcppsh.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    auto make  = (void*(*)(unsigned))          dl::dlsym(h, "cppsh_make");
    auto comp  = (unsigned(*)(void*,unsigned)) dl::dlsym(h, "cppsh_compute");
    auto inst  = (unsigned(*)())               dl::dlsym(h, "cppsh_instances");
    auto cfree = (void(*)(void*))              dl::dlsym(h, "cppsh_free");
    if (!make || !comp || !inst || !cfree) { trace_puts("  ? cppsh symbols"); ok = false; }
    else { auto w = make(100); if (w) { trace_printf("  compute(100,3)=%u\n", comp(w,3)); cfree(w); } }
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Circular cross-call mylib1↔mylib2. */
static void test_cross_call(bool& ok) {
    trace_puts("--- cross-library call chain ---");
    auto h1 = dl::dlopen("/tmp/mylib1.so");
    if (!h1) { trace_puts("  dlopen mylib1 failed"); ok = false; return; }
    auto h2 = dl::dlopen("/tmp/mylib2.so");
    if (!h2) { trace_puts("  dlopen mylib2 failed"); ok = false; return; }
    auto fn3 = (unsigned(*)(unsigned))dl::dlsym(h1, "library_function3");
    if (fn3) trace_printf("  library_function3(3)=%u\n", fn3(3));
    auto g_cross = (unsigned(*)(unsigned))dl::dlsym(h1, "g_cross");
    auto lf_cross = (unsigned(*)(unsigned))dl::dlsym(h2, "lf_cross");
    if (g_cross && lf_cross) trace_printf("  g_cross(7)=%u lf_cross(7)=%u\n", g_cross(7), lf_cross(7));
    dl::dlclose(h2); dl::dlclose(h1); trace_puts("  both unloaded");
}

/** @brief Load c_test1.so: globals, strings, weak, float. */
static void test_c_test1(bool& ok) {
    trace_puts("--- c_test1.so ---");
    auto h = dl::dlopen("/tmp/c_test1.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    using ifn = int(*)(int);
    test_u1(h, ok, "get_int", 0);
    { auto fn = (int(*)(int,int))dl::dlsym(h, "add_ints"); if(fn) trace_printf("  add_ints(7,3)=%d\n",fn(7,3)); }
    test_s0(h, ok, "get_str");
    test_u1(h, ok, "double_int", 21);
    test_u1(h, ok, "call_weak", 0);
    { auto pi = (float(*)())dl::dlsym(h, "get_pi"); if(pi) trace_printf("  get_pi()=%d\n",(int)(pi()*1000000)); }
    test_u0(h, ok, "inc_counter"); test_u0(h, ok, "inc_counter"); test_u0(h, ok, "get_counter");
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Load c_test2.so: cross target, weak override. */
static void test_c_test2(bool& ok) {
    trace_puts("--- c_test2.so ---");
    auto h = dl::dlopen("/tmp/c_test2.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    test_u1(h, ok, "c_test2_func", 7);
    test_u1(h, ok, "c_test2_own_func", 10);
    test_v0(h, ok, "print_s1"); test_u0(h, ok, "get_ct2_val"); test_u0(h, ok, "get_ct2_calls");
    { auto w = (int(*)())dl::dlsym(h, "c_test1_weak_func"); if(w) trace_printf("  weak override=%d\n",w()); }
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Cross-call c_test1↔c_test2. */
static void test_cross_c1c2(bool& ok) {
    trace_puts("--- c_test1 + c_test2 cross-call ---");
    auto h1 = dl::dlopen("/tmp/c_test1.so"); if(!h1){trace_puts("  dlopen c_test1 failed");ok=false;return;}
    auto h2 = dl::dlopen("/tmp/c_test2.so"); if(!h2){trace_puts("  dlopen c_test2 failed");ok=false;return;}
    { auto cr = (int(*)(int))dl::dlsym(h1,"cross_call"); if(cr) trace_printf("  cross_call(10)=%d\n",cr(10)); }
    { auto w = (int(*)())dl::dlsym(h2,"c_test1_weak_func"); if(w) trace_printf("  weak override=%d\n",w()); }
    dl::dlclose(h2); dl::dlclose(h1); trace_puts("  both unloaded");
}

/** @brief Cross-C++ cpp_test1↔cpp_test2 inheritance. */
static void test_cross_cpp(bool& ok) {
    trace_puts("--- cpp_test1 + cpp_test2 cross-C++ ---");
    auto h1 = dl::dlopen("/tmp/cpp_test1.so"); if(!h1){trace_puts("  dlopen cpp_test1 failed");ok=false;return;}
    auto h2 = dl::dlopen("/tmp/cpp_test2.so"); if(!h2){trace_puts("  dlopen cpp_test2 failed");ok=false;return;}
    trace_puts("  both loaded");
    auto make_adv = (void*(*)(int,int))dl::dlsym(h2,"cpp2_make_advanced");
    auto comp_adv = (int(*)(void*,int))dl::dlsym(h2,"cpp2_compute");
    auto free_adv = (void(*)(void*))dl::dlsym(h2,"cpp2_free");
    auto max_fn   = (int(*)(int,int))dl::dlsym(h1,"cpp1_max_int");
    if(make_adv&&comp_adv&&free_adv&&max_fn) {
        trace_printf("  max_of(88,44)=%d\n",max_fn(88,44));
        auto w=make_adv(10,50); if(w){trace_printf("  compute(3)=%d\n",comp_adv(w,3));free_adv(w);}
    } else { trace_puts("  ? cpp cross symbols"); ok=false; }
    dl::dlclose(h2); dl::dlclose(h1); trace_puts("  both unloaded");
}

/** @brief C++ standalone: vtable, template, heap. */
static void test_cpp_test1(bool& ok) {
    trace_puts("--- cpp_test1.so ---");
    auto h = dl::dlopen("/tmp/cpp_test1.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    auto make=(void*(*)(int))dl::dlsym(h,"cpp1_make");
    auto comp=(int(*)(void*,int))dl::dlsym(h,"cpp1_compute");
    auto gid=(int(*)(void*))dl::dlsym(h,"cpp1_get_id");
    auto inst=(int(*)())dl::dlsym(h,"cpp1_instance_count");
    auto cfree=(void(*)(void*))dl::dlsym(h,"cpp1_free");
    auto max=(int(*)(int,int))dl::dlsym(h,"cpp1_max_int");
    if(!make||!comp||!inst||!cfree||!max){trace_puts("  ? cpp1 symbols");ok=false;}
    else{trace_printf("  max_of(99,77)=%d\n",max(99,77));auto w=make(42);if(w){trace_printf("  compute(10)=%d\n",comp(w,10));cfree(w);}}
    dl::dlclose(h); trace_puts("  unloaded");
}

/** @brief Hard-float ABI test (HF=1 only). */
static void test_hardfp(bool& ok) {
    trace_puts("--- hf_test1 + hf_test2 (hard-float) ---");
    auto h1 = dl::dlopen("/tmp/hf_test1.so"); if(!h1){trace_printf("dlopen hf_test1: %s\n",dl::dlerror());ok=false;return;}
    auto h2 = dl::dlopen("/tmp/hf_test2.so"); if(!h2){trace_printf("dlopen hf_test2: %s\n",dl::dlerror());ok=false;return;}
    trace_puts("  both loaded");
    {auto add=(float(*)(float,float))dl::dlsym(h1,"hf_add");auto mul=(float(*)(float,float))dl::dlsym(h1,"hf_mul");if(add&&mul){trace_printf("  hf_add(2.5,3.7)=%d\n",(int)(add(2.5f,3.7f)*10));trace_printf("  hf_mul(4.0,2.0)=%d\n",(int)(mul(4.0f,2.0f)));}}
    {auto cross=(float(*)(float))dl::dlsym(h1,"hf_cross_call");if(cross)trace_printf("  hf_cross_call(3.0)=%d\n",(int)(cross(3.0f)*10));}
    {auto sc=(float(*)(float))dl::dlsym(h1,"hf_sincos");if(sc)trace_printf("  hf_sincos(0.0)=%d\n",(int)(sc(0.0f)*100));}
    {typedef struct{float s,p,m;int c;}FR;auto comp=(FR(*)(float*,int))dl::dlsym(h1,"hf_compute");if(comp){float a[]={5,10,3};FR r=comp(a,3);trace_printf("  hf_compute: sum=%d prod=%d max=%d\n",(int)(r.s*10),(int)(r.p),(int)(r.m*10));}}
    {auto f2=(float(*)(float))dl::dlsym(h2,"hf_test2_func");if(f2)trace_printf("  hf_test2_func(1.5)=%d\n",(int)(f2(1.5f)*10));}
    dl::dlclose(h2); dl::dlclose(h1); trace_puts("  both unloaded");
}

/** @brief GOT/PLT didactic walkthrough with before/after dump. */
static void test_gotplt(bool& ok) {
    trace_puts("\n================== GOT/PLT DIDACTIC WALKTHROUGH ==================");

    /* --- STEP 1: Open .so file to read initial GOT values --- */
    trace_puts("\n[STEP 1] Reading .so file initial GOT values (BEFORE patching)...");
#ifndef HARD_FLOAT
    trace_printf("** semihost_file (semihost-file.cpp) **\n");
#endif
    firmware::semihost_file sfile("/tmp/gotplt_test.so");
    if (!sfile.is_open()) { trace_puts("  FAIL: cannot open .so"); ok = false; return; }

    /* Find .got, .got.plt, .plt sections by reading ELF headers */
    const unsigned ehdr_size = 52, shdr_size = 40;
    std::byte ehdr_buf[52] = {};
    sfile.read_at(0, std::span<std::byte>(ehdr_buf, 52));
    auto* eh = reinterpret_cast<loader::elf::Ehdr*>(ehdr_buf);
    unsigned shoff = eh->e_shoff, shnum = eh->e_shnum, shstrndx = eh->e_shstrndx;

    /* Read section name string table */
    loader::elf::Shdr strtab_sh;
    sfile.read_at(shoff + shstrndx * shdr_size, std::span<std::byte>(reinterpret_cast<std::byte*>(&strtab_sh), shdr_size));

    auto read_shdr = [&](unsigned i) {
        loader::elf::Shdr sh{};
        sfile.read_at(shoff + i * shdr_size, std::span<std::byte>(reinterpret_cast<std::byte*>(&sh), shdr_size));
        return sh;
    };
    auto sname_eq = [&](loader::elf::Shdr& sh, const char* target) -> bool {
        char buf[32] = {};
        sfile.read_at(strtab_sh.sh_offset + sh.sh_name, std::span<std::byte>(reinterpret_cast<std::byte*>(buf), 31));
        const char* a = buf; const char* b = target;
        while (*a && *b && *a == *b) { ++a; ++b; }
        return *a == *b;
    };

    unsigned got_idx = 0, gotplt_idx = 0, plt_idx = 0;
    for (unsigned i = 0; i < shnum; ++i) {
        auto sh = read_shdr(i);
        if (sname_eq(sh, ".got")) got_idx = i;
        if (sname_eq(sh, ".got.plt")) gotplt_idx = i;
        if (sname_eq(sh, ".plt")) plt_idx = i;
    }
    if (!got_idx) { trace_puts("  FAIL: .got not found"); ok = false; sfile.close(); return; }

    /* Read .got BEFORE */
    loader::elf::Shdr got_sh = read_shdr(got_idx);
    static constexpr unsigned MAX_GOT = 64;
    std::uint32_t got_before[MAX_GOT] = {};
    unsigned got_count = got_sh.sh_size / 4;
    if (got_count > MAX_GOT) got_count = MAX_GOT;
    sfile.read_at(got_sh.sh_offset, std::span<std::byte>(reinterpret_cast<std::byte*>(got_before), got_count * 4));
    trace_printf("  .got at VA=0x%x: %u entries BEFORE loading:\n", got_sh.sh_addr, got_count);
    for (unsigned i = 0; i < got_count; ++i)
        trace_printf("    [%2u] 0x%08x%s\n", i, got_before[i], i < 3 ? " (reserved)" : " (to be patched)");

    /* Read .plt stubs (unchanged, shown for reference) */
    if (plt_idx) {
        loader::elf::Shdr psh = read_shdr(plt_idx);
        trace_printf("\n  .plt at VA=0x%x size=%u (%u stubs):\n", psh.sh_addr, psh.sh_size, psh.sh_size / 12);
        std::byte pdata[36] = {};
        unsigned psz = psh.sh_size > 36 ? 36 : psh.sh_size;
        sfile.read_at(psh.sh_offset, std::span<std::byte>(pdata, psz));
        auto* pw = reinterpret_cast<std::uint32_t*>(pdata);
        unsigned stubs = psz / 12;
        for (unsigned i = 0; i < stubs && i < 3; ++i)
            trace_printf("    stub[%u]: %08x %08x %08x  (jumps via GOT+0x%x)\n", i, pw[i*3], pw[i*3+1], pw[i*3+2], 0x130 + 4*(i+3));
        if (psh.sh_size / 12 > 3)
            trace_printf("    ... (%u more stubs)\n", psh.sh_size / 12 - 3);
    }
    sfile.close();

    /* --- STEP 2: dlopen (GOT gets patched) --- */
    trace_puts("\n[STEP 2] dlopen() - loading library...");
#ifndef HARD_FLOAT
    trace_printf("** dlopen (dl-impl.cpp) **\n");
#endif
    auto lib_h = dl::dlopen("/tmp/gotplt_test.so");
    if (!lib_h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    auto* lib = reinterpret_cast<firmware::library*>(lib_h);
    auto* ram = lib->image.base();
    trace_printf("  library RAM base = 0x%p (base_va=0x%x)\n", (void*)ram, lib->base_va);

    /* --- STEP 3: Read GOT from RAM AFTER patching --- */
    auto* got_after = reinterpret_cast<std::uint32_t*>(ram + (got_sh.sh_addr - lib->base_va));
    trace_printf("\n[STEP 3] .got AFTER patching (%u entries):\n", got_count);
    trace_printf("  BEFORE -> AFTER  (what changed)\n");
    trace_printf("  ----------------------------------------\n");
    for (unsigned i = 0; i < got_count; ++i) {
        const char* annot;
        if (i < 3) {
            annot = "  (reserved by dynamic linker)";
        } else if (got_before[i] == 0 && got_after[i] != 0) {
            annot = "  <- PATCHED (external symbol resolved)";
        } else if (got_after[i] == got_before[i]) {
            annot = "  (self-relative, unchanged)";
        } else {
            annot = "";
        }
        trace_printf("  [%2u] 0x%08x -> 0x%08x%s\n", i, got_before[i], got_after[i], annot);
    }

    /* --- STEP 4: dlsym resolution --- */
    trace_puts("\n[STEP 4] dlsym() - symbol resolution through patched GOT:");
    const char* syms[] = {"got_aa","got_bb","got_s1","got_add","got_mul",
                          "got_compute","got_float_chain","got_call_chain",
                          "got_get_block","got_print","got_show_block"};
    for (auto n : syms) {
        void* a = dl::dlsym(lib_h, n);
        trace_printf("    %-20s -> 0x%p %s\n", n, a, a ? "" : "NOT FOUND");
    }

    /* --- STEP 5: Verify relocation types work --- */
    trace_puts("\n[STEP 5] Verifying each relocation type:");
    {
        auto* pa = (unsigned int*)dl::dlsym(lib_h, "got_aa");
        trace_printf("    GLOB_DAT: got_aa=%u (AA=%u -> AA+100=%u)\n", pa ? *pa : 0, 10u, pa ? *pa : 0);
    }
    {
        auto fn = (int(*)(int,int))dl::dlsym(lib_h, "got_add");
        if (fn) trace_printf("    JUMP_SLOT: got_add(5,3)=%d (5+3+AA)\n", fn(5,3));
    }
    {
        auto fn = (int(*)(int))dl::dlsym(lib_h, "got_compute");
        if (fn) trace_printf("    INT-PLT: got_compute(7)=%d (add+mul chain)\n", fn(7));
    }
    {
        auto fn = (float(*)(float))dl::dlsym(lib_h, "got_float_chain");
        if (fn) trace_printf("    MATH-PLT: chain(0.5)=%d (sinf+cosf+sqrtf)\n", (int)(fn(0.5f)*10000));
    }
    {
        typedef struct { int id; float v; const char* l; } DB;
        auto fn = (DB(*)(int))dl::dlsym(lib_h, "got_get_block");
        if (fn) { DB b = fn(7); trace_printf("    RELATIVE: block id=%d val=%.1f label=%s\n", b.id, (double)b.v, b.l ? b.l : "?"); }
    }
    dl::dlclose(lib_h);
    trace_puts("\n================== GOT/PLT WALKTHROUGH COMPLETE ==================\n");
}

/**
 * @brief Probe libraries until a target function is found (dynamic discovery).
 *
 * 1. Try dlsym on a non-loaded library → expect nullptr.
 * 2. Load libraries one by one, after each load try to resolve a symbol.
 *    Stop when it resolves to a non-null address.
 * 3. Call the found function.
 */
static void test_probe(bool& ok) {
    trace_puts("--- probe discovery ---");

    /* Step 1: try dlsym before any load — should fail. */
    {
        auto fn = (unsigned(*)(unsigned))dl::dlsym(nullptr, "library_function");
        if (fn) { trace_puts("  FAIL: dlsym(nullptr) returned non-null"); ok = false; }
        else    { trace_puts("  dlsym(nullptr) = nullptr (expected)"); }
    }

    /* Step 2: probe libraries until we find "library_function". */
    const char* candidates[] = {"/tmp/mylib2.so", "/tmp/libcppsh.so", "/tmp/mylib1.so"};
    dl::handle found = nullptr;
    unsigned probe_ix = 0;

    for (auto path : candidates) {
        trace_printf("  probe[%u]: %s\n", probe_ix++, path);
        auto h = dl::dlopen(path);
        if (!h) { trace_printf("    dlopen failed, try next\n"); continue; }
        trace_puts("    loaded");

        auto fn = (unsigned(*)(unsigned))dl::dlsym(h, "library_function");
        if (fn) {
            trace_printf("    FOUND library_function in %s\n", path);
            auto r = fn(42);
            trace_printf("    fn(42) = %u\n", r);
            found = h;
            break;
        }
        dl::dlclose(h);
        trace_puts("    not found, unloaded");
    }

    if (!found) { trace_puts("  FAIL: never found"); ok = false; return; }
    dl::dlclose(found);
    trace_puts("  probe — found, closed (sequential test will reload)");
}

/**
 * @brief Load mylib1 + mylib2 simultaneously, test interleaved calls.
 */
static void test_concurrent(bool& ok) {
    trace_puts("--- concurrent mylib1 + mylib2 ---");
    auto h1 = dl::dlopen("/tmp/mylib1.so");
    auto h2 = dl::dlopen("/tmp/mylib2.so");
    if (!h1 || !h2) { trace_puts("  dlopen failed"); ok = false; return; }
    trace_puts("  both loaded");

    test_u1(h1, ok, "library_function",  42);
    test_u1(h1, ok, "library_function2", 42);
    test_u1(h1, ok, "library_function3", 3);
    test_u1(h1, ok, "g",  5);
    test_u0(h1, ok, "boo");
    test_u0(h1, ok, "foo");
    test_s0(h1, ok, "retstr");

    test_u1(h2, ok, "lf",  42);
    test_u1(h2, ok, "lf2", 42);
    test_u1(h2, ok, "lf3", 3);
    test_u0(h2, ok, "mm");
    test_u0(h2, ok, "xxx");

    auto fn_lib1 = (unsigned(*)(unsigned))dl::dlsym(h1, "library_function");
    auto fn_l2   = (unsigned(*)(unsigned))dl::dlsym(h2, "lf2");
    if (fn_lib1 && fn_l2) {
        auto r1 = fn_lib1(7);
        auto r2 = fn_l2(7);
        trace_printf("  cross: lib1(7)=%u lib2(7)=%u (distinct GOTs)\n", r1, r2);
    } else {
        trace_puts("  cross dlsym failed"); ok = false;
    }

    dl::dlclose(h1);
    dl::dlclose(h2);
    trace_puts("  both unloaded");
}

/** @brief Two-lib coverage: cov1+cov2, all reloc types. */
static void test_cov(bool& ok) {
    trace_puts("\n============= TEST2: COVERAGE cov1+cov2 =============");
    trace_puts("  PHASE 1: Load cov1.so");
    trace_puts("    R_ARM_ABS32 → constructor, R_ARM_RELATIVE → data ptr,");
    trace_puts("    R_ARM_GLOB_DAT → AA/BB/s1 from firmware");
    auto h1 = dl::dlopen("/tmp/cov1.so");
    if (!h1) { trace_printf("dlopen cov1: %s\n", dl::dlerror()); ok = false; return; }
    {
        auto fn = (int(*)())dl::dlsym(h1, "cov1_init_val");
        if (fn) trace_printf("  GLOB_DAT: init_val=%d (AA+50)\n", fn());
    }
    {
        typedef struct { int s; float v; const char* l; int cv; } CR;
        auto fn = (CR(*)(int))dl::dlsym(h1, "cov1_get_result");
        if (fn) { CR r = fn(5); trace_printf("  RELATIVE: sum=%d val=%d label=%s\n", r.s, (int)(r.v), r.l); }
    }
    {
        auto* pg = (int*)dl::dlsym(h1, "cov1_global");
        trace_printf("  ABS32: global=%d (AA*2)\n", pg ? *pg : 0);
    }

    trace_puts("  PHASE 2: Load cov2.so — JUMP_SLOT (printf, sinf, cosf, sqrtf)");
    auto h2 = dl::dlopen("/tmp/cov2.so");
    if (!h2) { trace_printf("dlopen cov2: %s\n", dl::dlerror()); ok = false; return; }
    {
        auto fn = (float(*)(float))dl::dlsym(h2, "cov2_math_chain");
        if (fn) trace_printf("  MATH-PLT: chain(0)=%d\n", (int)(fn(0.0f)*10000));
    }

    trace_puts("  PHASE 3: Cross-call cov1→cov2 (JUMP_SLOT across libs)");
    {
        auto fn = (int(*)(int))dl::dlsym(h1, "cov1_cross_call");
        if (fn) trace_printf("  CROSS cov1→cov2: cov1_cross_call(5)=%d (expected 130)\n", fn(5));
    }

    trace_puts("  PHASE 4: Cross-call cov2→cov1 (reverse PLT direction)");
    {
        auto fn = (int(*)(int))dl::dlsym(h2, "cov2_call_back");
        if (fn) trace_printf("  CROSS cov2→cov1: cov2_call_back(7)=%d (expected 24)\n", fn(7));
    }

    trace_puts("  PHASE 5: Array + math + struct return");
    {
        typedef struct { float mv; int cnt; const char* tag; } C2R;
        auto fn = (C2R(*)(int*,int))dl::dlsym(h2, "cov2_process");
        if (fn) { int a[]={1,2,3}; C2R r = fn(a,3); trace_printf("  ARRAY: math=%d cnt=%d tag=%s\n", (int)(r.mv*100), r.cnt, r.tag); }
    }

    dl::dlclose(h2); dl::dlclose(h1);
    trace_puts("  TEST2: ALL RELOCATION TYPES VERIFIED ✓");
    trace_puts("    (ABS32, RELATIVE, GLOB_DAT, JUMP_SLOT, CROSS-LIB)\n");
}

/** @brief Recursion, arrays, math lib, struct return. */
static void test_complex(bool& ok) {
    trace_puts("--- complex_test.so ---");
    auto h = dl::dlopen("/tmp/complex_test.so");
    if (!h) { trace_printf("dlopen: %s\n", dl::dlerror()); ok = false; return; }
    trace_puts("  loaded");
    {
        auto rc = (int(*)(int,int))dl::dlsym(h, "recursive_collatz");
        if (rc) trace_printf("  recursive_collatz(7,5)=%d\n", rc(7, 5));
        else { trace_puts("  ? recursive_collatz"); ok = false; }
    }
    {
        auto pa = (float(*)(float*,int,int))dl::dlsym(h, "process_array");
        if (pa) { float arr[] = {1.5f,2.0f,3.0f,4.5f,6.0f}; float s = pa(arr,5,3); trace_printf("  process_array sum=%d\n",(int)(s*10)); }
        else { trace_puts("  ? process_array"); ok = false; }
    }
    {
        typedef struct { int rv; float s,p; int it; float mv; } CR;
        auto ct = (CR(*)(int,float*,int,int))dl::dlsym(h,"run_complex_test");
        if (ct) { float buf[]={2.0f,4.0f,1.5f,3.0f,5.0f}; CR r=ct(10,buf,5,2); trace_printf("  run: rec_val=%d sum=%d prod=%d it=%d math=%d\n", r.rv,(int)(r.s*10),(int)r.p,r.it,(int)(r.mv*10)); }
        else { trace_puts("  ? run_complex_test"); ok = false; }
    }
    dl::dlclose(h); trace_puts("  unloaded");
}

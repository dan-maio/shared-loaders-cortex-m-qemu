# Test Libraries — Relocation Coverage

## C Libraries

### c_test1.so (12 KB)
Full coverage of C-level relocations:

| Reloc | Count | Source |
|-------|-------|--------|
| R_ARM_RELATIVE | many | string pointers, struct members, self-ref ptr |
| R_ARM_GLOB_DAT | several | g_int, g_int_arr, g_str, g_test_struct |
| R_ARM_JUMP_SLOT | several | printf, c_test2_func, f(), w() |
| R_ARM_ABS32 | 1 | constructor c_test1_init in .init_array |

Exports: `get_int`, `add_ints`, `set_int`, `get_str`, `cross_call`,
`call_f_from_firmware`, `call_w_with_BB`, `print_stats`, `inc_counter`, etc.

### c_test2.so (8.5 KB)
Cross-call target + weak override:

| Reloc | Count | Source |
|-------|-------|--------|
| R_ARM_GLOB_DAT | several | AA, BB, s1 from firmware |
| R_ARM_JUMP_SLOT | several | printf |
| R_ARM_ABS32 | 1 | constructor |
| R_ARM_COPY | possible | char* s1 from main |

Exports: `c_test2_func`, `c_test2_own_func`, `print_s1`, `c_test1_weak_func`
(overrides c_test1's weak)

## C++ Libraries

### cpp_test1.so (95 KB)
C++23 features: vtable, static local, template, global ctor, heap:

| Reloc | Source |
|-------|--------|
| R_ARM_RELATIVE | vtable pointers in .data.rel.ro |
| R_ARM_GLOB_DAT | extern AA, operator new, nothrow |
| R_ARM_JUMP_SLOT | printf, operator new/delete |
| R_ARM_ABS32 | .init_array for global ctor |

Exports: `cpp1_make`, `cpp1_compute`, `cpp1_get_id`,
`cpp1_instance_count`, `cpp1_free`, `cpp1_max_int`

### cpp_test2.so (94 KB)
Cross-library C++ inheritance:

| Reloc | Source |
|-------|--------|
| R_ARM_RELATIVE | vtable for AdvancedWidget |
| R_ARM_GLOB_DAT | extern BB, cpp1::Widget vtable ref |
| R_ARM_JUMP_SLOT | cpp1::Widget constructor, virtual dispatch |

Exports: `cpp2_make_advanced`, `cpp2_compute`, `cpp2_get_id`, `cpp2_free`
Note: `cpp2_cross_call` is defined inside `namespace cpp2` (not `extern "C"`),
so it's exported with C++ name mangling as `_ZN4cpp215cpp2_cross_callEi`.

/**
 * @file cpp-test1.cpp
 * @brief C++23 test lib 1 — vtable, static local, template, global ctor, heap.
 *
 * Relocation types:
 *   R_ARM_RELATIVE  — vtable pointers in .data.rel.ro, string pointers
 *   R_ARM_GLOB_DAT  — extern globals (AA, BB), static locals guard
 *   R_ARM_JUMP_SLOT — calls to printf, operator new/delete
 *   R_ARM_ABS32     — .init_array entries for global ctors
 *   R_ARM_COPY      — guard variables for static locals
 */
#include "cpp-test1.h"
#include <cstdio>
#include <new>

namespace cpp1 {

/* ================================================================
 * Extern globals from firmware
 * ================================================================ */
extern int AA;
extern int BB;

/* ================================================================
 * Global object with constructor — R_ARM_ABS32 in .init_array
 * ================================================================ */
static int g_start_count = 0;
int start_counter() noexcept { return ++g_start_count; }

// This global runs its constructor at load time (via .init_array)
namespace {
struct GlobalInit {
    GlobalInit() noexcept {
        g_start_count = 100;
        std::printf("cpp_test1: GlobalInit ctor, g_start_count=%d\n", g_start_count);
    }
} g_init;
}

/* ================================================================
 * Widget implementation
 * ================================================================ */

Widget::Widget(int seed) noexcept
    : seed_(seed)
    , id_(++g_start_count)
{
    std::printf("cpp_test1: Widget ctor id=%d seed=%d\n", id_, seed);
}

int Widget::compute(int x) const {
    // Static local — requires guard variable (R_ARM_COPY / TLS)
    static const int MAGIC = 42;
    // Virtual call through vtable
    return x * seed_ + MAGIC + AA + get_id();
}

int Widget::get_id() const {
    return id_;
}

int Widget::instance_count() noexcept {
    // Static local counter
    static int count = 0;
    return ++count;
}

/* ================================================================
 * Template instantiation (no relocation — inlined at compile time)
 * ================================================================ */
template int max_of<int>(int, int);          // explicit instantiation
template float max_of<float>(float, float);

} // namespace cpp1

/* ================================================================
 * C linkage exports
 * ================================================================ */

extern "C" void* cpp1_make(int seed) {
    auto* w = new (std::nothrow) cpp1::Widget(seed);  // operator new (JUMP_SLOT)
    return w;
}

extern "C" int cpp1_compute(void* w, int x) {
    return static_cast<cpp1::Widget*>(w)->compute(x);  // virtual dispatch
}

extern "C" int cpp1_get_id(void* w) {
    return static_cast<cpp1::Widget*>(w)->get_id();
}

extern "C" int cpp1_instance_count() {
    return cpp1::Widget::instance_count();  // static local
}

extern "C" void cpp1_free(void* w) {
    delete static_cast<cpp1::Widget*>(w);  // virtual destructor via vtable
}

extern "C" int cpp1_max_int(int a, int b) {
    return cpp1::max_of(a, b);  // template
}

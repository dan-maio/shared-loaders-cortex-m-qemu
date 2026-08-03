/**
 * @file cpp-test2.cpp
 * @brief C++23 test lib 2 — cross-library inheritance, virtual dispatch.
 *
 * Relocation types:
 *   R_ARM_RELATIVE  — vtable in .data.rel.ro (cross-library!)
 *   R_ARM_GLOB_DAT  — extern AA, BB
 *   R_ARM_JUMP_SLOT — operator new, printf, cpp1::max_of template call
 *   R_ARM_ABS32     — global constructor
 */
#include "cpp-test2.h"
#include "cpp-test1.h"
#include <cstdio>
#include <new>

namespace cpp2 {

/* ================================================================
 * Cross-library externs from cpp_test1
 * ================================================================ */
extern int AA;
extern int BB;

/* ================================================================
 * AdvancedWidget — inherits from cpp1::Widget
 * vtable is in THIS library, base vtable is in cpp_test1
 * ================================================================ */

AdvancedWidget::AdvancedWidget(int seed, int extra) noexcept
    : cpp1::Widget(seed)   // base constructor via PLT → cpp_test1
    , extra_(extra)
{
    std::printf("cpp_test2: AdvancedWidget ctor extra=%d\n", extra);
}

int AdvancedWidget::compute(int x) const {
    // Call base compute through cross-library vtable dispatch
    int base = cpp1::Widget::compute(x);
    return base + extra_ + BB;
}

int AdvancedWidget::get_extra() const noexcept {
    return extra_;
}

/* ================================================================
 * Cross-library call: calls cpp1::max_of template
 * ================================================================ */
int cpp2_cross_call(int x) {
    return cpp1::max_of(x, 50);  // template instantiated in cpp_test1
}

} // namespace cpp2

/* ================================================================
 * C linkage exports
 * ================================================================ */

extern "C" void* cpp2_make_advanced(int seed, int extra) {
    auto* w = new (std::nothrow) cpp2::AdvancedWidget(seed, extra);
    return w;
}

extern "C" int cpp2_compute(void* w, int x) {
    return static_cast<cpp2::AdvancedWidget*>(w)->compute(x);
}

extern "C" int cpp2_get_id(void* w) {
    return static_cast<cpp2::AdvancedWidget*>(w)->get_id();
}

extern "C" void cpp2_free(void* w) {
    delete static_cast<cpp2::AdvancedWidget*>(w);  // virtual destructor
}

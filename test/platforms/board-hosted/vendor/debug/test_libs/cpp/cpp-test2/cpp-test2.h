/**
 * @file cpp-test2.h
 * @brief Public API for cpp_test2 — cross-library C++ inheritance.
 *
 * Exercises:
 * - R_ARM_RELATIVE: vtable for AdvancedWidget (cross-library!)
 * - R_ARM_GLOB_DAT: extern BB, cpp1::Widget vtable reference
 * - R_ARM_JUMP_SLOT: cpp1::Widget constructor, virtual dispatch, operator new
 */
#ifndef CPP_TEST2_H
#define CPP_TEST2_H

#include "cpp-test1.h"

namespace cpp2 {

/**
 * @class AdvancedWidget
 * @brief Extends cpp1::Widget — tests cross-library vtable + inheritance.
 *
 * Virtual method compute() calls base class compute() through cross-library
 * vtable dispatch, then adds extra_ + BB.
 */
class AdvancedWidget final : public cpp1::Widget {
public:
    /**
     * @brief Constructor — calls base Widget(seed) across library boundary.
     * @param seed Seed value for base Widget.
     * @param extra Extra value added to compute result.
     */
    explicit AdvancedWidget(int seed, int extra) noexcept;

    /**
     * @brief Override compute — tests cross-library virtual dispatch.
     * @param x Input value.
     * @return Widget::compute(x) + extra_ + BB.
     */
    int compute(int x) const override;

    /**
     * @brief Get the extra value.
     * @return extra_.
     */
    int get_extra() const noexcept;
private:
    int extra_; /**< @brief Extra value added to compute result. */
};

} // namespace cpp2

extern "C" {
    /**
     * @brief Factory — creates AdvancedWidget via operator new.
     * @param seed Seed value for base Widget.
     * @param extra Extra value for AdvancedWidget.
     * @return Pointer to new AdvancedWidget (cast to void*).
     */
    void* cpp2_make_advanced(int seed, int extra);

    /**
     * @brief Compute via virtual dispatch.
     * @param w Pointer to AdvancedWidget.
     * @param x Input value.
     * @return widget->compute(x).
     */
    int   cpp2_compute(void* w, int x);

    /**
     * @brief Get widget ID via virtual dispatch.
     * @param w Pointer to AdvancedWidget.
     * @return widget->get_id().
     */
    int   cpp2_get_id(void* w);

    /**
     * @brief Free an AdvancedWidget — tests virtual destructor across libs.
     * @param w Pointer to AdvancedWidget.
     */
    void  cpp2_free(void* w);

    /**
     * @brief Cross-call to cpp1::max_of — tests template across library boundary.
     * @param x Input value.
     * @return max_of(x, 50).
     */
    int   cpp2_cross_call(int x);
}

#endif

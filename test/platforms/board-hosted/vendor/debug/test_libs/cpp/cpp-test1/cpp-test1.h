/**
 * @file cpp-test1.h
 * @brief Public API for cpp_test1 — C++23 vtable, template, heap, static local.
 *
 * Exercises:
 * - R_ARM_RELATIVE: vtable pointers in .data.rel.ro
 * - R_ARM_GLOB_DAT: extern AA, operator new, nothrow
 * - R_ARM_JUMP_SLOT: printf, operator new/delete
 * - R_ARM_ABS32: .init_array for global ctor
 */
#ifndef CPP_TEST1_H
#define CPP_TEST1_H

namespace cpp1 {

/**
 * @struct ICompute
 * @brief Virtual base class — exercises vtable (R_ARM_RELATIVE in .data.rel.ro).
 */
struct ICompute {
    virtual ~ICompute() = default;      /**< @brief Virtual destructor. */
    virtual int compute(int x) const = 0; /**< @brief Pure virtual compute. */
    virtual int get_id() const = 0;     /**< @brief Pure virtual get_id. */
};

/**
 * @class Widget
 * @brief Concrete class with virtual methods + static local + template.
 *
 * Tests vtable relocation, global constructor, static local guard variable,
 * and operator new from the heap pool.
 */
class Widget : public ICompute {
public:
    /**
     * @brief Constructor — exercises R_ARM_ABS32 for vtable init.
     * @param seed Seed value stored in the widget.
     */
    explicit Widget(int seed) noexcept;

    /** @brief Destructor — virtual, tests vtable dispatch. */
    ~Widget() override = default;

    /**
     * @brief Compute result — virtual dispatch through relocated vtable.
     * @param x Input value.
     * @return seed_ * x + id_.
     */
    int compute(int x) const override;

    /**
     * @brief Get widget ID.
     * @return The widget's id_.
     */
    int get_id() const override;

    /**
     * @brief Get instance count — tests static local with guard variable.
     * @return Number of Widget instances created.
     */
    static int instance_count() noexcept;

private:
    int seed_; /**< @brief Seed value. */
    int id_;   /**< @brief Widget identifier. */
};

/**
 * @brief Template function — instantiated at compile time.
 * @tparam T Numeric type.
 * @param a First value.
 * @param b Second value.
 * @return The greater of a and b.
 */
template <class T>
constexpr T max_of(T a, T b) noexcept {
    return a > b ? a : b;
}

/**
 * @brief Global counter — constructed before main.
 * @return Initial counter value.
 */
int start_counter() noexcept;

} // namespace cpp1

extern "C" {
    /**
     * @brief Factory function — creates a Widget via operator new.
     * @param seed Seed value for the widget.
     * @return Pointer to new Widget (cast to void*).
     */
    void*   cpp1_make(int seed);

    /**
     * @brief Compute via virtual dispatch.
     * @param w Pointer to Widget (from cpp1_make).
     * @param x Input value.
     * @return widget->compute(x).
     */
    int     cpp1_compute(void* w, int x);

    /**
     * @brief Get widget ID via virtual dispatch.
     * @param w Pointer to Widget.
     * @return widget->get_id().
     */
    int     cpp1_get_id(void* w);

    /**
     * @brief Get instance count — tests static local across load/unload.
     * @return Number of Widget instances.
     */
    int     cpp1_instance_count();

    /**
     * @brief Free a widget — tests operator delete.
     * @param w Pointer to Widget.
     */
    void    cpp1_free(void* w);

    /**
     * @brief Call max_of<int> — tests template instantiation.
     * @param a First value.
     * @param b Second value.
     * @return max_of(a, b).
     */
    int     cpp1_max_int(int a, int b);
}

#endif

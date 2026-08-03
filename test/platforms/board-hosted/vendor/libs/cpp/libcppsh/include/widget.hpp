/**
 * @file widget.hpp
 * @brief Internal C++23 polymorphic type — no STL headers.
 */
#pragma once
#include <cstdint>

namespace cppsh {

/**
 * @class ICompute
 * @brief Interface for generic calculation operations.
 */
class ICompute {
public:
    virtual ~ICompute() = default;
    /**
     * @brief Performs computation on an input value.
     * @param x Input argument.
     * @return Computed 32-bit unsigned integer.
     */
    [[nodiscard]] virtual std::uint32_t compute(std::uint32_t x) const noexcept = 0;
};

/**
 * @class Widget
 * @brief Concrete implementation of ICompute using a seed value.
 */
class Widget final : public ICompute {
public:
    /**
     * @brief Construct a Widget with a seed.
     * @param seed Calculation seed value.
     */
    explicit Widget(std::uint32_t seed) noexcept;
    /**
     * @brief Computes x + seed.
     * @param x Input value.
     * @return Sum of x and seed_.
     */
    [[nodiscard]] std::uint32_t compute(std::uint32_t x) const noexcept override;
private:
    std::uint32_t seed_; ///< seed value.
};

/**
 * @brief Computes the sum of elements in an array.
 * @tparam T Type of the array elements.
 * @param xs Pointer to the array elements.
 * @param n Number of elements in the array.
 * @return Sum of the array elements.
 */
template <class T>
[[nodiscard]] constexpr T sum(const T* xs, std::size_t n) noexcept {
    T acc{};
    for (std::size_t i = 0; i < n; ++i) acc += xs[i];
    return acc;
}

/**
 * @brief Retrieves the active instance count of Widgets.
 * @return Count of active Widget objects.
 */
[[nodiscard]] std::uint32_t instance_count() noexcept;

} // namespace cppsh


/**
 * @file widget.hpp
 * @brief Internal C++23 polymorphic type used by libcppsh.
 */
#pragma once
#include <cstdint>
#include <span>

namespace cppsh {

/// @brief Abstract compute interface (forces a vtable → C++ relocations).
class ICompute {
public:
    virtual ~ICompute() = default;
    /// @brief Compute a result from @p x.
    [[nodiscard]] virtual std::uint32_t compute(std::uint32_t x) const noexcept = 0;
};

/// @brief Concrete Widget: holds a seed and folds it with a static table.
class Widget final : public ICompute {
public:
    /// @brief Construct with a seed; bumps a global instance counter.
    explicit Widget(std::uint32_t seed) noexcept;
    [[nodiscard]] std::uint32_t compute(std::uint32_t x) const noexcept override;

private:
    std::uint32_t seed_;
};

/// @brief Sum a span using a C++ template (exercises constexpr/templated codegen).
template <class T>
[[nodiscard]] constexpr T sum(std::span<const T> xs) noexcept {
    T acc{};
    for (T v : xs) acc += v;
    return acc;
}

/// @brief Process-wide construction counter (defined in widget.cpp).
[[nodiscard]] std::uint32_t instance_count() noexcept;

} // namespace cppsh

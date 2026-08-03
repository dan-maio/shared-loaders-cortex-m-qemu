/**
 * @file widget.cpp
 * @brief Widget implementation, a global object, and a static-local lookup table.
 */
#include "widget.hpp"
#include <array>

namespace cppsh {
namespace {

/// @brief Global instance counter (lives in .bss; mutated by constructors).
std::uint32_t g_instances = 0;

/// @brief A global object with a non-trivial constructor → emits an .init_array entry.
struct Banner {
    Banner() noexcept { g_instances += 1000; } ///< Marks "library constructed".
};
Banner g_banner; ///< Forces the loader to run a C++ constructor at load time.

} // namespace

Widget::Widget(std::uint32_t seed) noexcept : seed_(seed) { ++g_instances; }

std::uint32_t Widget::compute(std::uint32_t x) const noexcept {
    // Static local with first-use init (no guard: -fno-threadsafe-statics).
    static const std::array<std::uint32_t, 4> kTable{2u, 3u, 5u, 7u};
    const std::span<const std::uint32_t> view{kTable};
    return seed_ + x * sum(view); // sum == 17
}

std::uint32_t instance_count() noexcept { return g_instances; }

} // namespace cppsh

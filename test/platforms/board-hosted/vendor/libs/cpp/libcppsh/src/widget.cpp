/**
 * @file widget.cpp
 * @brief Widget implementation — no STL headers, self-contained.
 */
#include "widget.hpp"

namespace cppsh {
namespace {

std::uint32_t g_instances = 0;

struct Banner {
    Banner() noexcept { g_instances += 1000; }
};
Banner g_banner;

} // namespace

Widget::Widget(std::uint32_t seed) noexcept : seed_(seed) { ++g_instances; }

std::uint32_t Widget::compute(std::uint32_t x) const noexcept {
    static const std::uint32_t kTable[4] = {2, 3, 5, 7};
    std::uint32_t acc = 0;
    for (auto v : kTable) acc += v;
    return seed_ + x * acc;
}

std::uint32_t instance_count() noexcept { return g_instances; }

} // namespace cppsh

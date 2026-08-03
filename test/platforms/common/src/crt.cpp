/**
 * @file crt.cpp
 * @brief Minimal C++ runtime for the firmware: operator new/delete backed by a
 *        static heap, plus std::nothrow. The nothrow new, sized delete, and
 *        std::nothrow are exported via .dynsym (-Wl,-E) so loaded libraries
 *        (libcppsh) resolve them. Throwing new/delete are also provided so any
 *        incidental host-side allocation (e.g. std::function) has a backing heap.
 */
#include "loader/region-allocator.hpp"
#include <cstddef>
#include <new>

namespace {
alignas(8) unsigned char g_heap[32 * 1024];        ///< C++ heap for loaded libs.
/** @brief Lazily-constructed singleton allocator over @ref g_heap. */
loader::region_allocator& heap() noexcept {
    static loader::region_allocator a(g_heap, sizeof(g_heap));
    return a;
}
} // namespace

/// @brief The std::nothrow tag object (libcppsh references _ZSt7nothrow).
namespace std { const nothrow_t nothrow{}; }

/** @brief Throwing/nothrow operator new/delete, all backed by @ref heap(). */
void* operator new(std::size_t n) {
    auto p = heap().alloc(n);
    return p.has_value() ? *p : nullptr;   // -fno-exceptions: cannot throw bad_alloc
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    auto p = heap().alloc(n);
    return p.has_value() ? *p : nullptr;
}
void  operator delete(void* p) noexcept { heap().free(p); }
void  operator delete[](void* p) noexcept { heap().free(p); }
void  operator delete(void* p, std::size_t) noexcept { heap().free(p); }
void  operator delete[](void* p, std::size_t) noexcept { heap().free(p); }

/**
 * @brief malloc/free/calloc/realloc backed by @ref heap().
 *
 * Plan 5d: the C++ EH runtime (libsupc++, now linked into main) allocates each
 * thrown exception object via malloc/free (__cxa_allocate_exception/__cxa_free_exception).
 * Back them by the same heap as operator new so the throw path has memory. Bound to
 * main's own libsupc++ refs at link time (-Wl,-Bsymbolic).
 */
extern "C" void* malloc(std::size_t n) noexcept {
    auto p = heap().alloc(n);
    return p.has_value() ? *p : nullptr;
}
extern "C" void free(void* p) noexcept { heap().free(p); }
extern "C" void* calloc(std::size_t a, std::size_t b) noexcept {
    const std::size_t n = a * b;                       // sizes here are tiny; no overflow guard
    void* p = malloc(n);
    if (p) for (std::size_t i = 0; i < n; ++i) static_cast<unsigned char*>(p)[i] = 0;
    return p;
}
// Best-effort: the allocator exposes no block size, so grow-realloc copies the new
// size (over-read stays within the static heap). Only the demangler/diagnostic paths
// hit realloc, never the happy catch path.
extern "C" void* realloc(void* p, std::size_t n) noexcept {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return nullptr; }
    void* q = malloc(n);
    if (q) for (std::size_t i = 0; i < n; ++i)
        static_cast<unsigned char*>(q)[i] = static_cast<unsigned char*>(p)[i];
    free(p);
    return q;
}

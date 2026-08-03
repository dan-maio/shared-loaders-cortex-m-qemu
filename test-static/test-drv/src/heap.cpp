/* Minimal operator new/delete backed by newlib's malloc/free (via sbrk). */
#include <cstdlib>
#include <new>

void* operator new(std::size_t s) { return std::malloc(s); }
void* operator new[](std::size_t s) { return std::malloc(s); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

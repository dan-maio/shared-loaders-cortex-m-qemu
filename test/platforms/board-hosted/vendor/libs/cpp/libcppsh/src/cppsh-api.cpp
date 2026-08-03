/**
 * @file cppsh-api.cpp
 * @brief extern "C" surface — self-contained, no libstdc++ dependency.
 */
#include "cppsh.h"
#include "widget.hpp"

/* Static pool for operator new (no libstdc++, no malloc). */
static char heap[256];
static unsigned heap_used = 0;

void* operator new(unsigned sz) noexcept {
    sz = (sz + 3) & ~3u;
    if (heap_used + sz > sizeof(heap)) return nullptr;
    void* p = heap + heap_used;
    heap_used += sz;
    return p;
}
void operator delete(void*) noexcept {}
void operator delete(void*, unsigned) noexcept {}

extern "C" cppsh_widget cppsh_make(uint32_t seed) {
    auto* w = new cppsh::Widget(seed);
    return w;
}

extern "C" uint32_t cppsh_compute(cppsh_widget w, uint32_t x) {
    return static_cast<cppsh::ICompute*>(w)->compute(x);
}

extern "C" uint32_t cppsh_instances(void) {
    return cppsh::instance_count();
}

extern "C" void cppsh_free(cppsh_widget w) {
    delete static_cast<cppsh::ICompute*>(w);
}

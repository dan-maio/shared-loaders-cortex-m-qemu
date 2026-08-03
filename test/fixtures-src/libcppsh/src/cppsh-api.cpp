/**
 * @file cppsh-api.cpp
 * @brief extern "C" surface bridging the C ABI to the C++ Widget (heap via main).
 */
#include "cppsh.h"
#include "widget.hpp"
#include <new>

extern "C" cppsh_widget cppsh_make(uint32_t seed) {
    return new (std::nothrow) cppsh::Widget(seed); // operator new resolved from main
}

extern "C" uint32_t cppsh_compute(cppsh_widget w, uint32_t x) {
    return static_cast<cppsh::ICompute*>(w)->compute(x); // virtual call via vtable
}

extern "C" uint32_t cppsh_instances(void) {
    return cppsh::instance_count();
}

extern "C" void cppsh_free(cppsh_widget w) {
    delete static_cast<cppsh::ICompute*>(w); // operator delete resolved from main
}

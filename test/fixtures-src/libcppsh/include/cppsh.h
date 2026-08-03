/**
 * @file cppsh.h
 * @brief Mangling-free public API of libcppsh, for resolution via dlsym.
 *
 * The library is implemented in C++23 but exports plain C names so the loader
 * can look them up without demangling.
 */
#ifndef CPPSH_H_
#define CPPSH_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/// @brief Opaque handle to a heap-allocated Widget instance.
typedef void* cppsh_widget;

/// @brief Construct a Widget seeded with @p seed (uses operator new from main).
/// @return Opaque handle, or NULL on allocation failure.
cppsh_widget cppsh_make(uint32_t seed);

/// @brief Invoke the Widget's virtual compute() with argument @p x.
uint32_t     cppsh_compute(cppsh_widget w, uint32_t x);

/// @brief Return a process-wide counter incremented by each constructor.
uint32_t     cppsh_instances(void);

/// @brief Destroy a Widget (uses operator delete from main).
void         cppsh_free(cppsh_widget w);

#if defined(__cplusplus)
}
#endif

#endif /* CPPSH_H_ */

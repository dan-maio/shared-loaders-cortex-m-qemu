/**
 * @file cppsh.h
 * @brief Mangling-free public API of libcppsh, for resolution via dlsym.
 */
#ifndef CPPSH_H_
#define CPPSH_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef void* cppsh_widget;

cppsh_widget cppsh_make(uint32_t seed);

uint32_t     cppsh_compute(cppsh_widget w, uint32_t x);

uint32_t     cppsh_instances(void);

void         cppsh_free(cppsh_widget w);

#if defined(__cplusplus)
}
#endif

#endif /* CPPSH_H_ */

#ifndef CPP_FEATURES_H_
#define CPP_FEATURES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t cpp_features_run(uint32_t mode, uint32_t x);
uint32_t cpp_features_summary(void);
void     cpp_features_print(void);

#ifdef __cplusplus
}
#endif

#endif

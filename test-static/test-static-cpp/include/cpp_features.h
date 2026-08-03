#ifndef CPP_FEATURES_H_
#define CPP_FEATURES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum CppFeature {
    FEATURE_CLASS = 0,
    FEATURE_CTOR_DTOR,
    FEATURE_METHOD,
    FEATURE_INHERITANCE,
    FEATURE_VIRTUAL,
    FEATURE_POLYMORPHISM,
    FEATURE_ABSTRACT,
    FEATURE_STATIC,
    FEATURE_NEW_DELETE,
    FEATURE_TEMPLATE,
    FEATURE_COUNT
};

uint32_t cpp_features_run(uint32_t mode, uint32_t x);
uint32_t cpp_features_summary(void);

#ifdef __cplusplus
}
#endif

#endif

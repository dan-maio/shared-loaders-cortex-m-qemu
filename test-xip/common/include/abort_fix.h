#pragma once
#ifdef __cplusplus
#include <cstdlib>
namespace std {
  extern "C" void abort(void) noexcept __attribute__((noreturn));
}
using std::abort;
#endif

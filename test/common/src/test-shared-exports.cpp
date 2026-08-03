#include "test-shared-exports.hpp"
#include "diag/trace.h"

unsigned int AA = 42;
unsigned int BB = 84;
char*        s1  = const_cast<char*>("main-s1");

unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }

std::uint32_t fm_var = 123;
struct FmInfo fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }

void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg);
}

void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v);
}

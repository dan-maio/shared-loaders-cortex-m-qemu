#include "test-shared-helpers.hpp"
#include "diag/trace.h"
#include "scl/log.h"
#include <cstring>

void put_hex(const char* label, std::uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9];
    for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0';
    trace_printf("%s%s", label, out);
}

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    while (*name && i + 1 < cap) out[i++] = *name++;
    out[i] = '\0';
}

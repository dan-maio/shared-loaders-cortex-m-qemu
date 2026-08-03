#include "scl/log.h"

void scl_log_put_hex(const char* label, uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9];
    for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0';
    scl_log_puts(label);
    scl_log_puts(out);
}

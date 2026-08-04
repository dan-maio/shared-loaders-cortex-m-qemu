/**
 * @file printf-shim.c
 * @brief Minimal printf/puts over semihosting, EXPORTED to loaded libraries.
 *
 * The fixture libraries are built -nostdlib and leave printf/puts undefined,
 * expecting main to provide them (mylib1's constructors call printf). This
 * bare-metal firmware has no libc, so this shim implements the tiny subset the
 * fixtures use: %s %c %u %d %x %p %% (field widths/precision are ignored).
 * Kept alive in .dynsym via -Wl,-u,printf -Wl,-u,puts anchors + -Wl,-E.
 */
#include "semihost.h"
#include <stdarg.h>
#include <stdint.h>

static void emit_uint(unsigned v, unsigned base, int is_signed) {
    char b[12]; int i = 0;
    unsigned x = v;
    if (is_signed && (int)v < 0) { trace_putc('-'); x = (unsigned)(-(int)v); }
    if (x == 0) { trace_putc('0'); return; }
    static const char hx[] = "0123456789abcdef";
    while (x) { b[i++] = hx[x % base]; x /= base; }
    while (i) trace_putc(b[--i]);
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { trace_putc(*p); continue; }
        ++p;
        while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == 'l')
            ++p;                                   /* ignore width/precision/length */
        switch (*p) {
        case 's': { const char* s = va_arg(ap, const char*); trace_puts(s ? s : "(null)"); break; }
        case 'c': trace_putc((char)va_arg(ap, int)); break;
        case 'u': emit_uint(va_arg(ap, unsigned), 10, 0); break;
        case 'd': case 'i': emit_uint(va_arg(ap, unsigned), 10, 1); break;
        case 'x': case 'X': emit_uint(va_arg(ap, unsigned), 16, 0); break;
        case 'p': trace_puts("0x"); emit_uint((unsigned)(uintptr_t)va_arg(ap, void*), 16, 0); break;
        case '%': trace_putc('%'); break;
        case '\0': va_end(ap); return 0;
        default:  trace_putc('%'); trace_putc(*p); break;   /* unsupported: echo */
        }
    }
    va_end(ap);
    return 0;
}

int puts(const char* s) {
    trace_puts(s ? s : "(null)");
    trace_putc('\n');
    return 0;
}

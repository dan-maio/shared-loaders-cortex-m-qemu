/**
 * @file semihost.c
 * @brief ARM semihosting host calls via the AngelSWI BKPT 0xAB convention.
 */
#include "semihost.h"
#include <stdint.h>

/** @brief ARM semihosting operation numbers (the r0 value passed to BKPT 0xAB). */
enum { SYS_OPEN = 0x01, SYS_CLOSE = 0x02, SYS_WRITEC = 0x03, SYS_WRITE0 = 0x04,
       SYS_READ = 0x06, SYS_SEEK = 0x0A, SYS_FLEN = 0x0C, SYS_GET_CMDLINE = 0x15,
       SYS_EXIT = 0x18 };

/* SYS_EXIT reason code: normal application exit (vs a runtime error). */
enum { ADP_STOPPED_APPLICATION_EXIT = 0x20026 };

/** @brief Issue one semihosting call: r0=op, r1=arg, returns r0. */
static inline uintptr_t semihost_call(uintptr_t op, uintptr_t arg)
{
    register uintptr_t r0 __asm__("r0") = op;
    register uintptr_t r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void trace_puts(const char* s)
{
    semihost_call(SYS_WRITE0, (uintptr_t) s);
}

void trace_putc(char c)
{
    semihost_call(SYS_WRITEC, (uintptr_t) &c);
}

int semihost_open(const char* path, int mode)
{
    uint32_t args[3] = { (uint32_t) path, (uint32_t) mode,
                         (uint32_t) __builtin_strlen(path) };
    return (int) semihost_call(SYS_OPEN, (uintptr_t) args);
}

long semihost_flen(int handle)
{
    uint32_t a = (uint32_t) handle;
    return (long) semihost_call(SYS_FLEN, (uintptr_t) &a);
}

int semihost_seek(int handle, long pos)
{
    uint32_t args[2] = { (uint32_t) handle, (uint32_t) pos };
    return (int) semihost_call(SYS_SEEK, (uintptr_t) args);
}

int semihost_read(int handle, void* buf, int len)
{
    uint32_t args[3] = { (uint32_t) handle, (uint32_t) buf, (uint32_t) len };
    return (int) semihost_call(SYS_READ, (uintptr_t) args);
}

int semihost_close(int handle)
{
    uint32_t a = (uint32_t) handle;
    return (int) semihost_call(SYS_CLOSE, (uintptr_t) &a);
}

int semihost_get_cmdline(char* buf, int len)
{
    /* Block: [0]=buffer ptr, [1]=buffer length. On return [1]=string length. */
    uint32_t args[2] = { (uint32_t) (uintptr_t) buf, (uint32_t) len };
    if (len <= 0) return -1;
    if (semihost_call(SYS_GET_CMDLINE, (uintptr_t) args) != 0) return -1;
    int n = (int) args[1];
    if (n < 0 || n >= len) n = len - 1;   /* clamp to buffer */
    buf[n] = '\0';                         /* ensure NUL-terminated */
    return n;
}

void semihost_exit(int code)
{
    uint32_t args[2] = { ADP_STOPPED_APPLICATION_EXIT, (uint32_t) code };
    semihost_call(SYS_EXIT, (uintptr_t) args);
    for (;;) { }   /* host ignored SYS_EXIT; never return into a dead frame */
}

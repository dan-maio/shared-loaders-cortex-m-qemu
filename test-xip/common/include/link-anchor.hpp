/**
 * @file link-anchor.hpp
 * @brief Forces the libc/libsupc++/libm symbols the fixtures borrow into main's link.
 *
 * The loaded libraries import symbols main never calls itself — operator new,
 * printf, the C++ EH runtime, libm. Those live in libc.a / libsupc++.a and are
 * pulled from an archive only when referenced, so main must reference them or
 * they are never linked in. Then `-Wl,-E` has nothing to export, they are absent
 * from main's `.dynsym`, and the loader cannot resolve them — which surfaces as a
 * HardFault inside the module rather than as a link error.
 *
 * This anchor forces exactly that set into the link. It replaces the old pile of
 * `-u` flags, and it is NOT a resolver: the loader still resolves via `.dynsym`.
 *
 * Include from exactly one translation unit (main). A test needing extra symbols
 * declares its own additional array alongside — see test-xip-interrupt/test1,
 * which anchors `fm_toggle_led` as well.
 *
 * Deliberately does not include <cstdio> / <cmath>: these declarations would
 * conflict with the library ones (`size_t` vs `unsigned int` on snprintf, for
 * instance). A main that already includes those headers should reference
 * `&::printf` directly instead of using this file — see
 * test-xip-static/spi-flash, which also anchors a different set because its
 * fixtures are built `-nostdlib` and borrow malloc/free/snprintf rather than the
 * C++ runtime.
 */
#pragma once

extern "C" {
int   printf(const char*, ...);
int   puts(const char*);
void* _Znwj(unsigned int);                                   // operator new
void* _ZnwjRKSt9nothrow_t(unsigned int, const void*);        // operator new(nothrow)
void* __cxa_allocate_exception(unsigned int);
void  __aeabi_unwind_cpp_pr0(void);
void* __dynamic_cast(const void*, const void*, const void*, long);
float sinf(float);  float cosf(float);  float logf(float);  float sqrtf(float);
}

__attribute__((used, retain)) static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(&printf),
    reinterpret_cast<const void*>(&puts),
    reinterpret_cast<const void*>(&_Znwj),
    reinterpret_cast<const void*>(&_ZnwjRKSt9nothrow_t),
    reinterpret_cast<const void*>(&__cxa_allocate_exception),
    reinterpret_cast<const void*>(&__aeabi_unwind_cpp_pr0),
    reinterpret_cast<const void*>(&__dynamic_cast),
    reinterpret_cast<const void*>(&sinf), reinterpret_cast<const void*>(&cosf),
    reinterpret_cast<const void*>(&logf), reinterpret_cast<const void*>(&sqrtf),
};

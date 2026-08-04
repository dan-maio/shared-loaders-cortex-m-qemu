/**
 * @file libc-shim.c
 * @brief Freestanding implementations of the few libc functions the compiler
 *        emits calls to (we build -nostdlib, so nothing provides them otherwise).
 *
 * gcc lowers struct copies / std::memcpy / std::memset into calls to these, and
 * with -nostdlib they would be undefined PLT slots that fault when first called.
 * Built with -fno-tree-loop-distribute-patterns so these loops are not themselves
 * turned back into memcpy/memset calls. -Wl,-Bsymbolic binds main's references to
 * these definitions directly (relocations become R_ARM_RELATIVE, not JUMP_SLOT).
 */
#include <stddef.h>

/** @brief Freestanding memcpy(): copy @p n bytes @p src -> @p dst (no overlap). */
__attribute__((used)) void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    const unsigned char* s = (const unsigned char*) src;
    while (n--) *d++ = *s++;
    return dst;
}

/** @brief Freestanding memset(): fill @p n bytes of @p dst with byte @p c. */
__attribute__((used)) void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    while (n--) *d++ = (unsigned char) c;
    return dst;
}

/** @brief Freestanding memmove(): copy @p n bytes, handling overlapping ranges. */
__attribute__((used)) void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    const unsigned char* s = (const unsigned char*) src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

/** @brief Freestanding memcmp(): compare @p n bytes; <0/0/>0 like the C standard. */
__attribute__((used)) int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*) a;
    const unsigned char* y = (const unsigned char*) b;
    while (n--) { if (*x != *y) return (int) *x - (int) *y; ++x; ++y; }
    return 0;
}

/** @brief Freestanding strlen(): length of NUL-terminated @p s. */
__attribute__((used)) size_t strlen(const char* s) {
    const char* p = s;
    while (*p) ++p;
    return (size_t) (p - s);
}

/**
 * @brief Freestanding strcmp(): lexicographic compare of @p a and @p b.
 *
 * Plan 5d: the C++ EH runtime (libsupc++, now linked into main) pulls in these.
 * strcmp is on the happy catch path: __gxx_personality_v0's __do_catch compares
 * typeinfo NAME strings across modules (the cross-module RTTI match).
 */
__attribute__((used)) int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

/** @brief Freestanding strncmp(): compare at most @p n chars of @p a and @p b. */
__attribute__((used)) int strncmp(const char* a, const char* b, size_t n) {
    while (n-- ) {
        unsigned char ca = (unsigned char) *a++, cb = (unsigned char) *b++;
        if (ca != cb) return (int) ca - (int) cb;
        if (ca == 0) break;
    }
    return 0;
}

/** @brief Freestanding abort(): spin forever. Reached only on an unhandled
 *         exception / failed assert (not the demo path). */
__attribute__((used, noreturn)) void abort(void) { for (;;) { } }

#pragma once
#include <cstdint>
#include <cstdio>
#include "diag/trace.h"
#include "fs-archive.hpp"
#include "xip/dynamic-newlib/adapter.hpp"
#include "scl/pic-trampoline.h"

inline bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

inline void* xopen(const char* name) {
    const fw::FsEntry* e = fw::fs_find(name);
    if (!e) return nullptr;
    return xip::newlib::dlopen(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, name);
}

inline std::uint32_t C0(void* h, std::uint32_t g, const char* s) {
    void* fn = xip::newlib::dlsym(h, s); return fn ? pic_call_u32(fn, (void*)g, 0) : 0; }
inline std::uint32_t C1(void* h, std::uint32_t g, const char* s, std::uint32_t a) {
    void* fn = xip::newlib::dlsym(h, s); return fn ? pic_call_u32(fn, (void*)g, a) : 0; }
inline std::uint32_t C2(void* h, std::uint32_t g, const char* s, std::uint32_t a, std::uint32_t b) {
    void* fn = xip::newlib::dlsym(h, s); return fn ? pic_call_u32_2(fn, (void*)g, a, b) : 0; }
inline std::uint32_t RD(void* h, const char* s) {
    void* p = xip::newlib::dlsym(h, s); return p ? *static_cast<volatile std::uint32_t*>(p) : 0; }
inline const char* CSTR(void* h, std::uint32_t g, const char* s) {
    void* fn = xip::newlib::dlsym(h, s); return fn ? reinterpret_cast<const char*>(pic_call_u32(fn, (void*)g, 0)) : "(null)"; }
inline const char* RD_STR(void* h, const char* s) {
    void* p = xip::newlib::dlsym(h, s); return p ? *static_cast<const char**>(p) : "(null)"; }

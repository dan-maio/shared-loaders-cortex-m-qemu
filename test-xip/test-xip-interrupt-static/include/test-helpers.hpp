#pragma once
#include <cstdint>
#include <cstdio>
#include "diag/trace.h"
#include "fs-archive.hpp"
#include "xip/static/adapter.hpp"
#include "scl/pic-trampoline.h"

inline bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

inline bool xload(const char* name, xip::stat::SModule& out) {
    const fw::FsEntry* e = fw::fs_find(name);
    if (!e) return false;
    return xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, out);
}

inline std::uint32_t C0(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0) : 0; }

inline std::uint32_t C1(xip::stat::SModule& m, const char* s, std::uint32_t a) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), a) : 0; }

inline std::uint32_t C2(xip::stat::SModule& m, const char* s, std::uint32_t a, std::uint32_t b) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32_2(fn, reinterpret_cast<void*>(m.got_base), a, b) : 0; }

inline std::uint32_t RD(xip::stat::SModule& m, const char* s) {
    void* p = xip::stat::resolve(m, s);
    return p ? *reinterpret_cast<volatile std::uint32_t*>(p) : 0; }

inline const char* CSTR(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? reinterpret_cast<const char*>(pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0)) : "(null)"; }

inline const char* RD_STR(xip::stat::SModule& m, const char* s) {
    void* p = xip::stat::resolve(m, s);
    return p ? *static_cast<const char**>(p) : "(null)"; }

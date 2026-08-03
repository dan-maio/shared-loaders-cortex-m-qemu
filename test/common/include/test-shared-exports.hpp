#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---- FM exports (AA, BB, s1, f, w, fm_var, fm_info, fm_callback) ----
extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;
unsigned int f(unsigned int x);
unsigned int w(unsigned int x);
extern std::uint32_t fm_var;
struct FmInfo { std::uint32_t id; std::uint32_t flags; };
extern struct FmInfo fm_info;
std::uint32_t fm_callback(std::uint32_t x);

// ---- Tracing helpers ----
void trace_event(const char* origin, const char* fn, std::uint32_t arg);
void trace_u32(const char* label, std::uint32_t v);

#ifdef __cplusplus
}
#endif

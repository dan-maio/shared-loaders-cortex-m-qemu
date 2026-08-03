#ifndef ELF_LOADER_LOGGER_H_
#define ELF_LOADER_LOGGER_H_

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string_view>

/// @file logger.h
/// Centralised logging for the ELF loader.
///
/// All diagnostic output is routed through elf_loader::log so that format,
/// level filtering and source-location tags are controlled from one place.
/// On Cortex-M the overhead is minimal — just a printf prefix.

namespace elf_loader {
namespace log {

/// Log severity levels.
/// NOTE: values are lowercase-prefixed to avoid clashes with common
/// preprocessor macros like DEBUG, INFO, ERROR.
enum level : uint8_t {
    LVL_DEBUG = 0,   ///< Verbose debug (disabled unless set_verbose(true))
    LVL_INFO  = 1,   ///< Normal operational messages
    LVL_WARN  = 2,   ///< Recoverable issues
    LVL_ERROR = 3    ///< Non-recoverable failures
};

/// Global minimum level; messages below this are suppressed.
extern level g_level;

/// If true, DEBUG-level messages are printed.
extern bool g_verbose;

/// Set the minimum log level.
inline void set_level(level l) { g_level = l; }

/// Enable/disable verbose (DEBUG) output.
inline void set_verbose(bool v) { g_verbose = v; }

/// @name Low-level emit
///@{

/// Print a log header with level tag and source location.
void header(level l, const char* func, const char* file, unsigned line);

/// Core printf-style logger (internal).
void vprint(level l, const char* func, const char* file, unsigned line,
            const char* fmt, va_list args);

///@}

/// @name Formatted helpers
///@{
//
// Macros are used here (not templates + source_location) because
// __builtin_FILE/LINE/FUNCTION evaluate at the definition point inside
// a template, not at the call site.  Macros correctly capture the caller.
// C++20's std::source_location would be ideal but its <source_location>
// header is not universally available on bare-metal targets (newlib-nano).

/// Core print template — all log_* macros forward here.
template<typename... Args>
inline void print(level l, const char* func, const char* file, unsigned line,
                  const char* fmt, Args&&... args) {
    if (l < g_level) return;
    if (l == LVL_DEBUG && !g_verbose) return;
    header(l, func, file, line);
    printf(fmt, args...);
    printf("\n");
}

///@{  log_info, log_warn, log_error, log_debug — capture caller location.
#define log_info(fmt, ...)    elf_loader::log::print(elf_loader::log::LVL_INFO,  __func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)    elf_loader::log::print(elf_loader::log::LVL_WARN,  __func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...)   elf_loader::log::print(elf_loader::log::LVL_ERROR, __func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...)   elf_loader::log::print(elf_loader::log::LVL_DEBUG, __func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
///@}

///@}

/// @name Hex formatting helpers
///@{

/// Number of rotating buffers, so several hex()/hex16() results can coexist
/// as arguments to a single printf/log call without clobbering each other.
inline constexpr uint32_t HEX_RING = 8;

/// Format a 32-bit value as 0xXXXXXXXX.
///
/// Returns a pointer into a small ring of static buffers so that multiple
/// calls in the same expression (e.g. `log_info("%s %s", hex(a), hex(b))`)
/// each get a distinct result.  Up to HEX_RING live results are supported.
inline const char* hex(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    static char ring[HEX_RING][11];
    static uint32_t slot = 0;
    char* buf = ring[slot];
    slot = (slot + 1) % HEX_RING;
    buf[0] = '0'; buf[1] = 'x';
    for (uint32_t i = 0; i < 8; ++i)
        buf[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    return buf;
}

/// Format a 16-bit value as 0xXXXX (also uses a rotating buffer ring).
inline const char* hex16(uint16_t v) {
    static const char h[] = "0123456789ABCDEF";
    static char ring[HEX_RING][7];
    static uint32_t slot = 0;
    char* buf = ring[slot];
    slot = (slot + 1) % HEX_RING;
    buf[0] = '0'; buf[1] = 'x';
    for (uint32_t i = 0; i < 4; ++i)
        buf[2 + i] = h[(v >> ((3 - i) * 4)) & 0xF];
    buf[6] = '\0';
    return buf;
}

/// Format a binding byte as human-readable string.
inline const char* binding_str(uint8_t b) {
    switch (b) {
        case 0: return "LOCAL";
        case 1: return "GLOBAL";
        case 2: return "WEAK";
        default: return "?";
    }
}

///@}

} // namespace log
} // namespace elf_loader

#endif // ELF_LOADER_LOGGER_H_

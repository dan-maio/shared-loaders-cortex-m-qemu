#include "static/logger.h"

namespace elf_loader {
namespace log {

/// Default: show LVL_INFO and above.
level g_level = LVL_INFO;

/// Debug output off by default.
bool g_verbose = false;

/// level tag strings.
static const char* level_tag(level l) {
    switch (l) {
        case LVL_DEBUG: return "DEBUG";
        case LVL_INFO:  return "INFO";
        case LVL_WARN:  return "WARN";
        case LVL_ERROR: return "ERROR";
    }
    return "?";
}

/// Strip the directory part so only the file name is shown.
static const char* basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

void header(level l, const char* func, const char* file, unsigned line) {
    // Routine INFO output shows just the file name (no line/func clutter);
    // DEBUG/WARN/ERROR add line and function to help locate the call site.
    if (l == LVL_INFO)
        printf("[%s] %s: ", level_tag(l), basename(file));
    else
        printf("[%s] %s:%u (%s): ", level_tag(l), basename(file), line, func);
}

void vprint(level l, const char* func, const char* file, unsigned line,
            const char* fmt, va_list args) {
    if (l < g_level) return;
    if (l == LVL_DEBUG && !g_verbose) return;
    header(l, func, file, line);
    vprintf(fmt, args);
    printf("\n");
}

} // namespace log
} // namespace elf_loader

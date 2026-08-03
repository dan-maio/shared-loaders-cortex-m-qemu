/**
 * @file main-flatfs.cpp
 * @brief flat_fs firmware entry: a command-line-driven "load-module" provisions
 *        the on-chip flat filesystem from semihosting, then the 15-test battery
 *        runs loading every library FROM FLASH.
 *
 * The host command line is delivered over semihosting (SYS_GET_CMDLINE), set on
 * the OpenOCD side with `arm semihosting_cmdline <args...>`. Supported forms
 * (tokens separated by spaces, commas, or braces -- "{a,b}" == "a b"):
 *
 *   -c <lib...>   CREATE:     (re)format the flat_fs, then copy in each listed
 *                             library that is present on the host.
 *   <lib...>      APPEND:     append each listed library to the existing flat_fs
 *                             (auto-creates the archive first if none is valid).
 *   -d <lib...>   DELETE-ALL: erase every file in the flat_fs (reformat), then
 *                             copy in the listed libraries. (The append-log
 *                             archive has no per-file delete, so "delete all"
 *                             is a reformat -- see docs/FLAT_FS_DESIGN.md.)
 *   -l            LIST:       print the flat_fs table of contents over semihosting,
 *                             leave the fs untouched (provision the standard set
 *                             only if it is empty), then -- like no args -- run
 *                             the full battery.
 *   (no args)     DEFAULT:    if the flat_fs is empty/invalid, provision the
 *                             standard battery set; otherwise leave it as-is.
 *
 * Each <lib> is a basename (e.g. "c1.so"); the host file read is DL_LIB_DIR/<lib>,
 * and the archive stores it under <lib> (what dlopen_fs looks up). "present" means
 * the host file opens over semihosting; missing ones are skipped with a note.
 *
 * Ordering note (critical): provisioning erases/programs flash, during which an
 * instruction fetch from flash stalls. The load-module therefore runs BEFORE
 * board_init() (which enables SysTick) so no interrupt handler is fetched from
 * flash mid-erase. battery_run() calls board_init() itself, afterwards.
 */
#include "semihost.h"
#include "semihost-file.hpp"
#include "image-memory.hpp"
#include "fs-archive.hpp"
#include "dl.hpp"
#include "dl-semihost.hpp"
#include "dl-fs.hpp"
#include "battery.hpp"
#include "fm-iface.h"     // trace_u32 (FM-exported; defined in main-tests.cpp)
#include <cstdint>
#include <span>
#include <utility>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

namespace {

/** @brief load-module operating mode, decoded from the semihosting command line. */
enum class Mode { Default, Create, Append, DeleteAll, List };

/** @brief Print @p v as decimal (no newline, no libc) for the -l listing. */
void put_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) trace_putc(b[--i]);
}
/** @brief Print @p v as "0x" + 8 hex digits (no newline) for the -l listing. */
void put_hex(std::uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    trace_puts("0x");
    for (int i = 28; i >= 0; i -= 4) trace_putc(hx[(v >> i) & 0xfu]);
}

/** @brief Print the flat_fs table of contents over semihosting (the -l listing). */
void list_files() {
    if (!fw::fs_valid()) { trace_puts("  (flat_fs invalid / empty)\n"); return; }
    const std::uint32_t n = fw::fs_count();
    for (std::uint32_t i = 0; i < n; ++i) {
        const fw::FsEntry* e = fw::fs_entry(i);
        if (!e) break;
        trace_puts("  ["); put_dec(i); trace_puts("] ");
        trace_puts(e->name);
        trace_puts("  bytes="); put_dec(e->size);
        trace_puts("  off=");   put_hex(e->off);
        trace_puts("  crc=");   put_hex(e->crc32);
        trace_putc('\n');
    }
    trace_puts("  ("); put_dec(n); trace_puts(" file(s), ");
    put_dec(fw::fs_used()); trace_puts(" bytes used)\n");
}

/** @brief Default battery set provisioned when no command line is supplied. */
const char* const kDefault[] = {
    "c1.so", "c2.so", "cpp3.so", "libcppeh.so", "ehx_catch.so", "notelf.bin"
};

/** @brief True if @p c is a token delimiter (space, tab, comma, or a brace). */
bool is_delim(char c) { return c == ' ' || c == '\t' || c == ',' || c == '{' || c == '}'; }

/** @brief Equality of two NUL-terminated strings. */
bool streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}

/**
 * @brief In-place tokenizer: split @p s on delimiters, NUL-terminating each token and
 *        filling @p argv (up to @p maxv) with pointers into @p s. Returns the count.
 */
int tokenize(char* s, const char** argv, int maxv) {
    int n = 0;
    char* p = s;
    while (*p && n < maxv) {
        while (*p && is_delim(*p)) ++p;          // skip leading delimiters
        if (!*p) break;
        argv[n++] = p;                           // token start
        while (*p && !is_delim(*p)) ++p;         // to end of token
        if (*p) *p++ = '\0';                     // terminate + step past
    }
    return n;
}

/** @brief Build "DL_LIB_DIR/name" into @p out (bounded by @p cap). */
void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}

/** @brief Read one host file @p name over semihosting (into the arena) and append it to the FS. */
bool provision_name(const char* name) {
    char path[160];
    make_path(path, sizeof(path), name);

    auto srcr = fw::semihost_file::open(path);
    if (!srcr) { trace_puts("  [load-module] absent, skipped: "); trace_puts(name); trace_putc('\n'); return false; }
    fw::semihost_file src = std::move(*srcr);
    const std::size_t sz = src.size();

    auto memp = fw::image_arena().alloc(sz);
    if (!memp) { trace_puts("  [load-module] arena OOM: "); trace_puts(name); trace_putc('\n'); return false; }
    auto* buf = static_cast<std::byte*>(*memp);

    bool ok = false;
    if (src.read_at(0, std::span<std::byte>{buf, sz}) &&
        fw::fs_add(name, buf, static_cast<std::uint32_t>(sz))) {
        trace_puts("  [load-module] saved "); trace_puts(name);
        trace_u32(" bytes=", static_cast<std::uint32_t>(sz));
        ok = true;
    } else {
        trace_puts("  [load-module] FAILED: "); trace_puts(name); trace_putc('\n');
    }
    fw::image_arena().free(buf);
    return ok;
}

/** @brief The command-line-driven load-module: parse the cmdline and create / append /
 *         delete-all / list / default-provision the flat_fs accordingly. */
void load_module() {
    static char cmd[256];
    if (semihost_get_cmdline(cmd, static_cast<int>(sizeof(cmd))) < 0) cmd[0] = '\0';

    trace_puts("\n[load-module] cmdline: \"");
    trace_puts(cmd[0] ? cmd : "(none)");
    trace_puts("\"\n");

    const char* argv[24];
    int argc = tokenize(cmd, argv, 24);

    Mode mode = Mode::Default;
    int first = 0;
    if (argc > 0 && streq(argv[0], "-c"))      { mode = Mode::Create;    first = 1; }
    else if (argc > 0 && streq(argv[0], "-d")) { mode = Mode::DeleteAll; first = 1; }
    else if (argc > 0 && streq(argv[0], "-l")) { mode = Mode::List;      first = 1; }
    else if (argc > 0)                         { mode = Mode::Append;    first = 0; }

    switch (mode) {
    case Mode::Create:
        trace_puts("[load-module] -c CREATE: formatting flat_fs\n");
        if (!fw::fs_format()) { trace_puts("[load-module] fs_format FAILED\n"); return; }
        break;
    case Mode::DeleteAll:
        trace_puts("[load-module] -d DELETE-ALL: erasing flat_fs (reformat)\n");
        if (!fw::fs_format()) { trace_puts("[load-module] fs_format FAILED\n"); return; }
        break;
    case Mode::Append:
        trace_puts("[load-module] APPEND to flat_fs\n");
        if (!fw::fs_valid()) {
            trace_puts("[load-module] (no valid flat_fs -> creating it first)\n");
            if (!fw::fs_format()) { trace_puts("[load-module] fs_format FAILED\n"); return; }
        }
        break;
    case Mode::List:
        trace_puts("[load-module] -l LIST flat_fs files:\n");
        list_files();
        // Then behave like no cmdline: leave a valid fs untouched, or provision the
        // standard set if empty/invalid, so the battery that follows has its libs.
        if (!fw::fs_valid()) {
            trace_puts("[load-module] (empty) -> default provisioning so tests can run\n");
            if (!fw::fs_format()) { trace_puts("[load-module] fs_format FAILED\n"); return; }
            for (const char* nm : kDefault) provision_name(nm);
        }
        trace_puts("[load-module] done\n");
        return;

    case Mode::Default:
        trace_puts("[load-module] no args -> default provisioning\n");
        if (fw::fs_valid()) { trace_puts("[load-module] flat_fs already present -> nothing to do\n"); return; }
        if (!fw::fs_format()) { trace_puts("[load-module] fs_format FAILED\n"); return; }
        for (const char* nm : kDefault) provision_name(nm);
        trace_puts("[load-module] done\n");
        return;
    }

    // Create / DeleteAll / Append: copy in each listed library that is present.
    for (int i = first; i < argc; ++i) provision_name(argv[i]);
    trace_puts("[load-module] done\n");
}

} // namespace

/** @brief flat_fs entry: run the cmdline-driven load-module, then the battery FROM FLASH. */
int main() {
    // NOTE: no board_init()/interrupts before provisioning (see file header).
    trace_puts("\nf411re-dynloader flat_fs: on-chip filesystem build\n");

    load_module();   // command-line-driven: create / append / delete-all / default

    trace_u32("[boot] flat_fs files   = ", fw::fs_count());
    trace_u32("[boot] flat_fs used (B)= ", fw::fs_used());

    g_use_flatfs = true;     // route the battery's dlopen through the flash archive
    return battery_run();    // calls board_init(), then runs the 15 tests from flash
}

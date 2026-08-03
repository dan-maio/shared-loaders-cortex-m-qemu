#include "semihost.h"
#include "semihost-file.hpp"
#include "image-memory.hpp"
#include "fs-archive-spi.hpp"
#include "spi-flash.hpp"
#include "battery.hpp"
#include "fm-iface.h"
#include <cstdint>
#include <span>
#include <utility>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

static void put_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) trace_putc(b[--i]);
}

static bool provision_name(const char* name) {
    char path[160]; std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < 160; ++d) path[i++] = *d;
    path[i++] = '/';
    for (const char* n = name; *n && i + 1 < 160; ++n) path[i++] = *n;
    path[i] = '\0';

    auto srcr = fw::semihost_file::open(path);
    if (!srcr) { trace_puts("  open FAILED: "); trace_puts(path); trace_puts("\n"); return false; }
    fw::semihost_file src = std::move(*srcr);
    const std::size_t sz = src.size();

    auto memp = fw::image_arena().alloc(sz);
    if (!memp) { trace_puts("  arena OOM: "); trace_puts(name); trace_puts("\n"); return false; }
    auto* buf = static_cast<std::byte*>(*memp);

    bool ok = false;
    auto rr = src.read_at(0, std::span<std::byte>{buf, sz});
    if (!rr) { trace_puts("  read FAILED: "); trace_puts(path); trace_puts("\n"); }
    if (rr && fw::fs_add(name, buf, static_cast<std::uint32_t>(sz))) {
        trace_puts("  saved "); trace_puts(name);
        trace_puts(" bytes="); put_dec(static_cast<std::uint32_t>(sz));
        trace_puts("\n");
        ok = true;
    } else {
        trace_puts("  FAILED: "); trace_puts(name); trace_puts("\n");
    }
    fw::image_arena().free(buf);
    return ok;
}

int main() {
    trace_puts("\n=== flatfs-bigbang-spi: provision + battery ===\n");

    spi_flash_init();
    trace_puts("SPI flash OK\n");

    trace_puts("Formatting SPI flatfs...\n");
    if (!fw::fs_format()) { trace_puts("fs_format FAILED\n"); for (;;); }
    trace_puts("Format OK\n");

    const char* libs[] = { "c1.so", "c2.so", "cpp3.so", "libcppeh.so",
                           "ehx_catch.so", "libprov.so", "libcons.so",
                           "libcppsh.so","notelf.bin" };
    for (auto lib : libs) provision_name(lib);

    trace_puts("\nFiles: "); put_dec(fw::fs_count()); trace_puts("\n");

    g_use_flatfs = true;
    trace_puts("battery_run...\n");
    return battery_run();
}

#include "board.h"
#include "semihost.h"
#include "semihost-file.hpp"
#include "image-memory.hpp"
#include "fs-archive-spi.hpp"
#include "spi-flash.h"
#include "battery.hpp"
#include "fm-iface.h"
#include <cstdint>
#include <span>
#include <utility>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

static void put_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) trace_putc(b[--i]);
}

// SPI-flash flatfs backend: bit-banged GPIO SPI driver (w25q64.cpp), the same
// approach the XIP static/spi-flash test's libspi.so uses and which is proven
// reliable on this board at any CPU clock. The spi_flash_* API comes from the
// driver; no HAL SPI peripheral, no PLL-dependent re-init needed.

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

/* Read a provisioned blob back from the W25Q64 and check its CRC32 against the
 * archive TOC entry. Runs at the current clock (provisioning clock here). */
static bool verify_blob(const char* name) {
    const fw::FsEntry* e = fw::fs_find(name);
    if (!e) return false;
    auto memp = fw::image_arena().alloc(e->size);
    if (!memp) return false;
    auto* buf = static_cast<std::byte*>(*memp);
    spi_flash_reinit();
    spi_flash_read(e->off, reinterpret_cast<std::uint8_t*>(buf),
                   static_cast<std::uint16_t>(e->size));
    const bool ok = (fw::crc32_calc(buf, e->size) == e->crc32);
    fw::image_arena().free(buf);
    return ok;
}

int main() {
    trace_puts("\n=== flatfs-spi (HAL-based SPI driver) ===\n");

    /* SPI init at HSI 16MHz (before battery's PLL config) */

    spi_flash_init();
    trace_puts("SPI flash init OK\n");

    uint8_t mid, tid;
    if (spi_flash_read_id(&mid, &tid) == 0) {
        trace_puts("W25Q64 detected\n");
    } else {
        trace_puts("W25Q64 NOT found\n");
        for (;;);
    }

    trace_puts("Formatting SPI flatfs...\n");
    if (!fw::fs_format()) { trace_puts("fs_format FAILED\n"); for (;;); }
    trace_puts("Format OK\n");

    const char* libs[] = { "c1.so", "c2.so", "cpp3.so", "libcppeh.so",
                           "ehx_catch.so", "libprov.so", "libcons.so",
                           "libcppsh.so","notelf.bin" };
    for (auto lib : libs) provision_name(lib);

    /* Bit-banged GPIO SPI is clock-independent, so reads work at any CPU
     * clock. Switch to PLL 100 MHz for the battery (board_init is idempotent
     * via the SWS guard) and verify the read-back there too. */
    board_init();
    spi_flash_reinit();   /* bit-bang no-op */

    trace_puts("\nVerify read-back (CRC32, PLL clock, bit-bang SPI)...\n");
    unsigned vbad = 0;
    for (auto lib : libs) {
        if (verify_blob(lib)) { trace_puts("  verify OK: "); trace_puts(lib); trace_puts("\n"); }
        else { trace_puts("  verify FAIL: "); trace_puts(lib); trace_puts("\n"); ++vbad; }
    }
    if (vbad) { trace_puts("SPI read-back CORRUPT\n"); }
    else      { trace_puts("SPI read-back all OK\n"); }

    trace_puts("\nFiles: "); put_dec(fw::fs_count()); trace_puts("\n");

    g_use_flatfs = true;
    trace_puts("battery_run...\n");
    return battery_run();
}

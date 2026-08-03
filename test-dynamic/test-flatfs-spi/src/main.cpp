#include "board.h"
#include "semihost.h"
#include "semihost-file.hpp"
#include "image-memory.hpp"
#include "fs-archive-spi.hpp"
#include "w25q64.h"
#include "battery.hpp"
#include "fm-iface.h"
#include <cstdint>
#include <span>
#include <utility>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

extern "C" void HAL_Init(void);
extern "C" void HAL_IncTick(void);

static void put_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) trace_putc(b[--i]);
}

static w25q64 flash;
static bool flash_init_ok = false;

// SPI-flash flatfs backend using the HAL-based w25q64 driver
extern "C" {
void spi_flash_init(void) {
    HAL_Init();
    flash.power_up();
    flash_init_ok = true;
}
int  spi_flash_read_id(uint8_t* mid, uint8_t* tid) {
    return flash.read_id(mid, tid) ? 0 : -1;
}
void spi_flash_sector_erase(uint32_t addr) {
    flash.sector_erase(addr);
}
int  spi_flash_page_program(uint32_t addr, const uint8_t* data, uint16_t len) {
    return flash.page_program(addr, data, len) ? 0 : -1;
}
void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len) {
    flash.read(addr, data, len);
}
int  spi_flash_write(uint32_t addr, const uint8_t* data, uint32_t len) {
    return flash.write(addr, data, len) ? 0 : -1;
}
void spi_flash_get_sr(uint32_t* sr, uint32_t* cr1) { (void)sr; (void)cr1; }
void spi_flash_reinit(void) { flash.power_up(); }
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
    trace_puts("\n=== flatfs-spi (HAL-based SPI driver) ===\n");

    /* SPI init at HSI 16MHz (before battery's PLL config) */

    spi_flash_init();
    trace_puts("SPI flash init OK\n");

    uint8_t mid, tid;
    if (flash.read_id(&mid, &tid)) {
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

    trace_puts("\nFiles: "); put_dec(fw::fs_count()); trace_puts("\n");

    g_use_flatfs = true;
    trace_puts("battery_run...\n");
    return battery_run();
}

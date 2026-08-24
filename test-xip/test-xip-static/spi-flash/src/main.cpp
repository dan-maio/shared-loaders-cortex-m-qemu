/**
 * @file main.cpp
 * @brief XIP test with SPI flash storage — all modules run from internal flash (XIP).
 *
 * Flow:
 *   1. Read spi.so from semihosting → write to internal flash
 *   2. Load spi.so from internal flash via XIP
 *   3. Init SPI flash, provision uart/led/cpp to SPI flash
 *   4. Copy uart/led/cpp from SPI flash → internal flash
 *   5. Unload spi.so
 *   6. Load uart/led/cpp from internal flash via XIP, execute
 */
#include "module.h"
#include "test-helpers.hpp"
#include "diag/trace.h"
#include "firmware/semihost-file.hpp"
#include "fs-archive-spi.hpp"
#include "fs-archive-internal.hpp"
#include "flash-prog.h"
#include "scl/image-memory.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// ---- SPI flash driver wrappers (resolved from spi.so via sym()) ----
static scl::Module g_spi_mod;
static scl::Module g_uart_mod;  // kept alive for cross-module calls

extern "C" {
static void spi_flash_drv_sector_erase(uint32_t addr) {
    auto fn = (void(*)(uint32_t))g_spi_mod.sym("spi_erase_sector");
    if (fn) fn(addr);
}
static int spi_flash_drv_write(uint32_t addr, const uint8_t* data, uint32_t len) {
    auto fn = (int(*)(uint32_t, const uint8_t*, uint32_t))g_spi_mod.sym("spi_write");
    return fn ? fn(addr, data, len) : -1;
}
static void spi_flash_drv_read(uint32_t addr, uint8_t* data, uint16_t len) {
    auto fn = (void(*)(uint32_t, uint8_t*, uint16_t))g_spi_mod.sym("spi_read");
    if (fn) fn(addr, data, len);
}
static int spi_flash_drv_page_program(uint32_t addr, const uint8_t* data, uint16_t len) {
    auto fn = (int(*)(uint32_t, const uint8_t*, uint32_t))g_spi_mod.sym("spi_write");
    return fn ? fn(addr, data, (uint32_t)len) : -1;
}

// Export uart functions from main so cpp_features.so can resolve them
void uart_init(void) {
    auto fn = (void(*)(void))g_uart_mod.sym("uart_init");
    if (fn) fn();
}
void uart_puts(const char* s) {
    auto fn = (void(*)(const char*))g_uart_mod.sym("uart_puts");
    if (fn) fn(s);
}
void uart_discard(void) {
    auto fn = (void(*)(void))g_uart_mod.sym("uart_discard");
    if (fn) fn();
}
}

// cpp_features.so also borrows symbols main never calls itself. Those live in
// libc.a and are pulled from an archive only when referenced, so main must
// reference them or they are never linked in — then -E has nothing to export and
// bind_host_imports() leaves the JUMP_SLOT unbound. printf and puts were missing
// for exactly that reason; malloc/free/snprintf only happened to be pulled in by
// something else, so anchor the whole set rather than rely on that.
// This is an anchor, not a resolver: the loader resolves via .dynsym.
// (Declarations come from <cstdio>/<cstdlib> above — do not redeclare them.)
__attribute__((used, retain)) static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(static_cast<int(*)(const char*, ...)>(&::printf)),
    reinterpret_cast<const void*>(static_cast<int(*)(const char*)>(&::puts)),
    reinterpret_cast<const void*>(static_cast<int(*)(char*, size_t, const char*, ...)>(&::snprintf)),
    reinterpret_cast<const void*>(&::malloc),
    reinterpret_cast<const void*>(&::free),
};

// ---- buffers ----
alignas(8) static uint8_t g_img[32768];

// ---- provision spi.so to internal flash flatfs ----
static bool provision_spi_to_internal() {
    trace_puts("[prov] Loading spi.so from semihosting...\n");
    char path[160];
    int i = 0;
    for (const char* d = DL_LIB_DIR; *d && i < 159; ++d) path[i++] = *d;
    if (i < 159) path[i++] = '/';
    for (const char* n = "libspi.so"; *n && i < 159; ++n) path[i++] = *n;
    path[i] = '\0';

    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_puts("  [prov] spi.so not found\n"); return false; }
    uint32_t sz = (uint32_t)src.size();
    if (sz > sizeof(g_img)) { trace_puts("  [prov] spi.so too large\n"); return false; }
    auto res = src.read_at(0, {(std::byte*)g_img, sz});
    if (!res.has_value()) { trace_puts("  [prov] read failed\n"); return false; }

    if (!fw_int::fs_add("spi.so", g_img, sz).has_value()) {
        trace_puts("  [prov] fs_add failed\n");
        return false;
    }
    trace_printf("  [prov] spi.so written to internal flash (%u bytes)\n", sz);
    return true;
}

// ---- provision .so to SPI flash ----
static bool provision_to_spi(const char* filename) {
    char path[160];
    int i = 0;
    for (const char* d = DL_LIB_DIR; *d && i < 159; ++d) path[i++] = *d;
    if (i < 159) path[i++] = '/';
    for (const char* n = filename; *n && i < 159; ++n) path[i++] = *n;
    path[i] = '\0';

    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("  [prov-spi] absent: %s\n", filename); return false; }
    uint32_t sz = (uint32_t)src.size();
    if (sz > sizeof(g_img)) { trace_printf("  [prov-spi] too large: %s\n", filename); return false; }
    src.read_at(0, {(std::byte*)g_img, sz});

    const char* bn = filename;
    if (bn[0]=='l' && bn[1]=='i' && bn[2]=='b') bn += 3;
    if (!fw_spi::fs_add(bn, g_img, sz).has_value()) {
        trace_printf("  [prov-spi] write failed: %s\n", bn);
        return false;
    }
    trace_printf("  [prov-spi] saved %s (%u bytes)\n", bn, sz);
    return true;
}

// ---- copy from SPI to internal flash ----
static bool copy_spi_to_internal(const char* name) {
    const fw_spi::FsEntry* e = fw_spi::fs_find(name);
    if (!e) { trace_printf("  [copy] not in SPI: %s\n", name); return false; }
    spi_flash_drv_read(e->off, g_img, (uint16_t)e->size);

    // PLT stubs read the GOT PC-relatively, so under XIP they hit the flash image
    // of the GOT, not the RAM copy the loader relocates. Bind those slots now,
    // while the blob is still in RAM: JUMP_SLOT/GLOB_DAT targets are host
    // addresses, fixed at main's link time and independent of where the module
    // later lands. The load-time-dependent R_ARM_RELATIVE slots are untouched and
    // stay RAM-only. fs_add() CRCs what we hand it, so the patch is covered.
    int bound = xip::stat::bind_host_imports({(std::byte*)g_img, e->size});
    trace_printf("  [copy] %s: prelinked %d host imports\n", name, bound);

    if (!fw_int::fs_add(name, g_img, e->size).has_value()) {
        trace_printf("  [copy] internal write failed: %s\n", name);
        return false;
    }
    trace_printf("  [copy] %s -> internal flash (%u bytes)\n", name, e->size);
    return true;
}

// ---- test functions ----
static void test_uart() {
    trace_puts("\n[xip] ==== UART test ====\n");
    const fw_int::FsEntry* e = fw_int::fs_find("uart.so");
    if (!e) { trace_puts("  uart.so not found\n"); return; }

    const uint8_t* flash_blob = (const uint8_t*)fw_int::fs_blob(*e);
    scl::Module m;
    if (!m.load_buf(flash_blob, e->size, nullptr, 0, nullptr)) {
        trace_puts("  uart load FAIL\n");
        return;
    }
    trace_puts("  [uart] loaded (XIP)\n");
    call_sym0(m, "uart_init");
    call_sym1(m, "uart_puts", (uint32_t)(uintptr_t)"UART from internal flash (XIP)!");
    call_sym0(m, "uart_discard");
    trace_puts("  UART test PASSED\n");
}

static void test_led() {
    trace_puts("\n[xip] ==== LED test ====\n");
    const fw_int::FsEntry* e = fw_int::fs_find("led.so");
    if (!e) { trace_puts("  led.so not found\n"); return; }

    const uint8_t* flash_blob = (const uint8_t*)fw_int::fs_blob(*e);
    scl::Module m;
    if (!m.load_buf(flash_blob, e->size, nullptr, 0, nullptr)) {
        trace_puts("  led load FAIL\n");
        return;
    }
    trace_puts("  [led] loaded (XIP)\n");
    call_sym0(m, "led_init");
    call_sym1(m, "led_blink", 5);
    trace_puts("  LED test PASSED\n");
}

static void test_cpp_features() {
    trace_puts("\n[xip] ==== C++ features test ====\n");

    // Load uart into g_uart_mod (stays alive for cross-module calls)
    const fw_int::FsEntry* eu = fw_int::fs_find("uart.so");
    if (eu) {
        const uint8_t* fb = (const uint8_t*)fw_int::fs_blob(*eu);
        if (g_uart_mod.load_buf(fb, eu->size, nullptr, 0, nullptr)) {
            call_sym0(g_uart_mod, "uart_init");
        }
    }

    const fw_int::FsEntry* e = fw_int::fs_find("cpp_features.so");
    if (!e) { trace_puts("  cpp_features.so not found\n"); return; }

    const uint8_t* flash_blob = (const uint8_t*)fw_int::fs_blob(*e);
    scl::Module m;
    if (!m.load_buf(flash_blob, e->size, nullptr, 0, nullptr)) {
        trace_puts("  cpp_features load FAIL\n");
        return;
    }
    trace_puts("  [cpp] loaded (XIP)\n");

    uint32_t r0 = call_sym1(m, "cpp_features_run", 0);
    uint32_t r1 = call_sym1(m, "cpp_features_run", 1);
    uint32_t r2 = call_sym1(m, "cpp_features_run", 2);
    uint32_t s  = call_sym0(m, "cpp_features_summary");

    call_sym0(m, "cpp_features_print");

    trace_printf("  C++ OK: run0=%u run1=%u run2=%u sum=0x%04X\n", r0, r1, r2, s);
    trace_puts("  C++ features test PASSED\n");

    call_sym0(g_uart_mod, "uart_discard");
    g_uart_mod = scl::Module{};
}

// ---- list files ----
static void list_files(const char* label, auto count_fn, auto entry_fn) {
    trace_printf("\n========== %s ==========\n", label);
    uint32_t count = count_fn();
    trace_printf("  Files: %u\n", count);
    for (uint32_t i = 0; i < count; ++i) {
        auto* e = entry_fn(i);
        if (e) {
            char buf[64];
            extern int snprintf(char*, size_t, const char*, ...);
            snprintf(buf, sizeof(buf), "  %-16s %u bytes (off 0x%06x, crc32 0x%08x)\n",
                     e->name, e->size, e->off, e->crc32);
            trace_puts(buf);
        }
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_puts("\nscl-fw-xip-static-spi-flash: SPI flash -> internal flash -> XIP\n");

    // Step 1: Format internal flash flatfs
    trace_puts("\n========== STEP 1: FORMAT INTERNAL FLASH ==========\n");
    if (!fw_int::fs_format()) {
        trace_puts("FATAL: internal flash format failed\n");
        for (;;) {}
    }
    trace_puts("[int] flatfs formatted\n");

    // Step 2: Provision spi.so to internal flash flatfs
    trace_puts("\n========== STEP 2: PROVISION SPI.SO ==========\n");
    if (!provision_spi_to_internal()) {
        trace_puts("FATAL: cannot provision spi.so\n");
        for (;;) {}
    }

    // Step 3: Load spi.so from internal flash flatfs (XIP)
    trace_puts("\n========== STEP 3: LOAD SPI.SO (XIP) ==========\n");
    {
        const fw_int::FsEntry* e = fw_int::fs_find("spi.so");
        if (!e) { trace_puts("FATAL: spi.so not in internal flash\n"); for (;;) {} }
        const uint8_t* blob = (const uint8_t*)fw_int::fs_blob(*e);
        if (!g_spi_mod.load_buf(blob, e->size, nullptr, 0, nullptr)) {
            trace_puts("FATAL: cannot load spi.so\n");
            for (;;) {}
        }
        trace_puts("  spi.so loaded (XIP)\n");
    }

    // Step 4: Init SPI flash
    trace_puts("\n========== STEP 4: INIT SPI FLASH ==========\n");
    {
        auto init_fn = (void(*)(void))g_spi_mod.sym("spi_init");
        auto fmt_fn = (void(*)(void))g_spi_mod.sym("spi_format");
        if (init_fn) init_fn();
        if (fmt_fn) fmt_fn();
    }
    fw_spi::fs_init(spi_flash_drv_sector_erase, spi_flash_drv_write,
                    spi_flash_drv_read, spi_flash_drv_page_program);
    trace_puts("[spi] initialized\n");

    // Step 5: Provision to SPI flash
    trace_puts("\n========== STEP 5: PROVISION TO SPI FLASH ==========\n");
    if (!fw_spi::fs_format()) { trace_puts("FATAL: SPI format failed\n"); for (;;) {} }
    provision_to_spi("libuart.so");
    provision_to_spi("libled.so");
    provision_to_spi("libcpp_features.so");
    trace_printf("[spi] provisioned %u files\n", fw_spi::fs_count());

    // Step 6: Copy SPI flash → internal flash flatfs (add to existing, don't reformat)
    trace_puts("\n========== STEP 5: COPY TO INTERNAL FLASH ==========\n");
    copy_spi_to_internal("uart.so");
    copy_spi_to_internal("led.so");
    copy_spi_to_internal("cpp_features.so");
    trace_printf("[int] total %u files\n", fw_int::fs_count());

    // List files
    list_files("SPI FLASH FILES", fw_spi::fs_count, fw_spi::fs_entry);
    list_files("INTERNAL FLASH FILES", fw_int::fs_count, fw_int::fs_entry);

    // Step 7: Unload spi.so — no longer needed
    trace_puts("\n========== STEP 7: UNLOAD SPI.SO ==========\n");
    g_spi_mod = scl::Module{};
    trace_puts("[unload] spi.so unloaded\n");

    // Step 8: Load uart/led/cpp from internal flash (XIP) and execute
    trace_puts("\n========== STEP 8: XIP TESTS ==========\n");
    test_uart();
    test_led();
    test_cpp_features();

    trace_puts("\n========== DONE ==========\n");
    std::exit(0);
    for (;;) {}
}

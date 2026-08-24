#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "elf-reader.h"
#include "module.h"
#include "spi.h"
#include "uart.h"
#include "led.h"
#include "cpp_features.h"

extern "C" void trace_puts(const char*);

// Local new/delete so the linker does NOT resolve _Znaj/_ZdlPv from
// libcpp_features.so's PLT — those operators must be callable before the .so
// is loaded (e.g. init_syms → symbol_table → new[]).
void* operator new(size_t size)                  { return malloc(size); }
void* operator new[](size_t size)                { return malloc(size); }
void  operator delete(void* p) noexcept           { if (p) free(p); }
void  operator delete[](void* p) noexcept         { if (p) free(p); }
void  operator delete(void* p, size_t) noexcept   { if (p) free(p); }
void  operator delete[](void* p, size_t) noexcept { if (p) free(p); }

static elf_loader::symbol_table g_syms;

static void init_syms() {
    g_syms.add_global("trace_puts", (uint32_t)(uintptr_t)(void*)trace_puts);
    g_syms.add_global("printf",     (uint32_t)(uintptr_t)(void*)(int(*)(const char*,...))printf);
    g_syms.add_global("puts",       (uint32_t)(uintptr_t)(void*)(int(*)(const char*))puts);
    g_syms.add_global("putchar",    (uint32_t)(uintptr_t)(void*)(int(*)(int))putchar);
    g_syms.add_global("memcpy",     (uint32_t)(uintptr_t)(void*)memcpy);
    g_syms.add_global("memset",     (uint32_t)(uintptr_t)(void*)memset);
    g_syms.add_global("malloc",     (uint32_t)(uintptr_t)(void*)malloc);
    g_syms.add_global("free",       (uint32_t)(uintptr_t)(void*)free);
}

alignas(8) static uint8_t g_spi_img[32768];
alignas(8) static uint8_t g_img[32768];
alignas(8) static uint8_t g_exec[32768];

#define FS_MAGIC   0x46414C31u
#define FS_VERSION 1u
#define FS_TOC_CAP 64u
#define FS_TOC_OFF 32u
#define FS_BLOB_OFF ((32u + FS_TOC_CAP * 32u + 3u) & ~3u)

struct FsEntry { char name[16]; uint32_t off, size, crc32, state; };
static const uint32_t FS_STATE_FREE = 0xFFFFFFFFu;
static const uint32_t FS_STATE_LIVE = 0x4C495645u;
static FsEntry g_toc[FS_TOC_CAP];

static void flatfs_format() {
    trace_puts("  format...\n");
    for (uint32_t a = 0; a < (1024u*1024u); a += 4096) spi_erase_sector(a);
    struct { uint32_t magic, ver, to, tc, bo, rsv[3]; } sb = {
        FS_MAGIC, FS_VERSION, FS_TOC_OFF, FS_TOC_CAP, FS_BLOB_OFF, {0,0,0} };
    spi_write(0, (const uint8_t*)&sb, 32);
    memset(g_toc, 0xFF, sizeof(g_toc));
    spi_write(FS_TOC_OFF, (const uint8_t*)g_toc, sizeof(g_toc));
}

static int flatfs_add(const char* name, const uint8_t* data, uint32_t len) {
    int slot = -1;
    uint32_t frontier = FS_BLOB_OFF;
    for (int i = 0; i < FS_TOC_CAP; i++) {
        if (g_toc[i].state == FS_STATE_FREE && slot < 0) slot = i;
        if (g_toc[i].state == FS_STATE_LIVE) {
            uint32_t end = (g_toc[i].off + g_toc[i].size + 3) & ~3u;
            if (end > frontier) frontier = end;
        }
    }
    if (slot < 0) return -1;
    if (spi_write(frontier, data, len)) return -1;
    g_toc[slot] = FsEntry{};
    int i = 0; while (name[i] && i < 15) { g_toc[slot].name[i] = name[i]; i++; }
    g_toc[slot].off  = frontier;
    g_toc[slot].size = len;
    g_toc[slot].state = FS_STATE_LIVE;
    uint32_t toc_addr = FS_TOC_OFF + (uint32_t)slot * 32;
    if (spi_write(toc_addr, (const uint8_t*)&g_toc[slot], 32)) return -1;
    return 0;
}

static const FsEntry* flatfs_find(const char* name) {
    for (int i = 0; i < FS_TOC_CAP; i++) {
        if (g_toc[i].state != FS_STATE_LIVE) continue;
        if (strncmp(g_toc[i].name, name, 16) == 0) return &g_toc[i];
    }
    return 0;
}

int main() {
    trace_puts("test-drv start\n");
    init_syms();

    trace_puts("-- libspi.so: load + init + format --\n");
    scl::Module mod;
    if (!mod.load_file("fixtures/libspi.so", g_spi_img, sizeof(g_spi_img), &g_syms))
        { trace_puts("spi load FAIL\n"); for (;;); }

    spi_init();
    spi_format();
    flatfs_format();
    trace_puts("SPI flash formatted\n");

    trace_puts("-- provision libuart.so + libled.so to SPI flash --\n");
    {
        extern int open(const char*, int, ...);
        extern long lseek(int, long, int);
        extern int read(int, void*, unsigned);
        extern int close(int);
        const char* files[] = {"fixtures/libuart.so", "fixtures/libled.so", "fixtures/libcpp_features.so"};
        for (int f = 0; f < 3; f++) {
            int fd = open(files[f], 0, 0); if (fd < 0) continue;
            long sz = lseek(fd, 0, 2); lseek(fd, 0, 0);
            if (sz <= 0 || (size_t)sz > sizeof(g_img)) { close(fd); continue; }
            read(fd, g_img, (unsigned)sz); close(fd);
            const char* bn = files[f] + 9;
            if (*bn == 'l') bn += 3; // skip "lib"
            if (flatfs_add(bn, g_img, (uint32_t)sz) == 0)
                { trace_puts("  saved "); trace_puts(bn); trace_puts("\n"); }
        }
    }

    trace_puts("-- load uart.so from flatfs --\n");
    {
        const FsEntry* e = flatfs_find("uart.so");
        if (e) {
            spi_read(e->off, g_img, (uint16_t)e->size);
            scl::Module m;
            if (!m.load_buf(g_img, e->size, g_exec, sizeof(g_exec), &g_syms))
                { trace_puts("  uart load FAIL\n"); }
            else { uart_init(); uart_puts("UART from flatfs"); uart_discard(); }
            trace_puts("UART discarded\n");
        } else trace_puts("  uart.so not found\n");
    }

    trace_puts("-- load led.so from flatfs --\n");
    {
        const FsEntry* e = flatfs_find("led.so");
        if (e) {
            spi_read(e->off, g_img, (uint16_t)e->size);
            scl::Module m;
            if (!m.load_buf(g_img, e->size, g_exec, sizeof(g_exec), &g_syms))
                { trace_puts("  led load FAIL\n"); }
            else { led_init(); led_blink(10); led_discard(); }
            trace_puts("LED discarded\n");
        } else trace_puts("  led.so not found\n");
    }

    /* 5. Load uart.so + cpp_features.so from flatfs → C++ tests → print via uart */
    trace_puts("-- load cpp_features.so + uart.so from flatfs --\n");
    {
        const FsEntry* eu = flatfs_find("uart.so");
        const FsEntry* ec = flatfs_find("cpp_features.so");
        if (eu && ec) {
            spi_read(eu->off, g_img, (uint16_t)eu->size);
            scl::Module mu;
            if (!mu.load_buf(g_img, eu->size, g_exec, sizeof(g_exec), &g_syms))
                { trace_puts("  uart load FAIL\n"); }
            else {
                uart_init();
                spi_read(ec->off, g_img, (uint16_t)ec->size);
                /* Load cpp_features at offset 16384 (after uart's code) */
                scl::Module mc;
                if (!mc.load_buf(g_img, ec->size, g_exec + 16384,
                                 sizeof(g_exec) - 16384, &g_syms))
                    { trace_puts("  cpp load FAIL\n"); }
                else {
                    /* Direct calls — GOT patched by patch_main() */
                    uint32_t r0 = cpp_features_run(0, 10);
                    uint32_t r1 = cpp_features_run(1, 10);
                    uint32_t r2 = cpp_features_run(2, 10);
                    uint32_t s  = cpp_features_summary();
                    cpp_features_print();

                    char buf[128];
                    extern int snprintf(char*, size_t, const char*, ...);
                    snprintf(buf, sizeof(buf),
                        "C++ OK: run0=%lu run1=%lu run2=%lu sum=0x%04X\n",
                        (unsigned long)r0, (unsigned long)r1,
                        (unsigned long)r2, (unsigned long)s);
                    uart_puts(buf);
                }
            }
            uart_discard();
            trace_puts("UART+cpp discarded\n");
        } else trace_puts("  uart.so or cpp_features.so not found\n");
    }

    trace_puts("test-drv done\n");

    trace_puts("-- flatfs files --\n");
    {
        FsEntry toc[FS_TOC_CAP];
        spi_read(FS_TOC_OFF, (uint8_t*)toc, sizeof(toc));
        for (uint32_t i = 0; i < FS_TOC_CAP; i++) {
            if (toc[i].state == FS_STATE_LIVE) {
                trace_puts("  ");
                trace_puts(toc[i].name);
                char buf[48];
                extern int snprintf(char*, size_t, const char*, ...);
                snprintf(buf, sizeof(buf), "  %lu bytes\n", (unsigned long)toc[i].size);
                trace_puts(buf);
            }
        }
    }

    exit(0);
    for (;;);
}

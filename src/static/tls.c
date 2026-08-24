/**
 * @file tls.c
 * @brief Multi-thread-capable TLS for the scl::static loader. See static/tls.h.
 *
 * The soft-TP ABI (`-mtp=soft`, the ARM default) makes every TLS access call
 * `__aeabi_read_tp()` for the thread pointer. We keep the per-module templates
 * and offsets in a small static registry; the OS/RTOS allocates one area per
 * thread and advertises its base. `__aeabi_read_tp` is weak so an RTOS can
 * override it with a per-thread (e.g. register- or TCB-based) implementation.
 */
#include "static/tls.h"
#include <stdlib.h>
#include <string.h>

#define SCL_TLS_MAX_MODULES 16
#define SCL_TLS_ALIGN       8

typedef struct {
    uint32_t offset;      ///< byte offset of this module in the TLS area
    uint32_t tdata_sz;    ///< size of the .tdata image (0 if none)
    uint32_t tls_size;    ///< tdata + tbss, rounded up to alignment
    uint8_t* tdata;       ///< heap copy of the .tdata image (owned)
} scl_tls_module;

static scl_tls_module g_mods[SCL_TLS_MAX_MODULES];
static uint32_t g_count       = 0;
static uint32_t g_next_offset = 0;
static void*    g_thread_ptr  = 0;

static uint32_t round_up(uint32_t v) {
    return (v + SCL_TLS_ALIGN - 1) & ~(SCL_TLS_ALIGN - 1u);
}

uint32_t scl_tls_register(const void* tdata, uint32_t tdata_sz, uint32_t tls_size) {
    if (g_count >= SCL_TLS_MAX_MODULES) return UINT32_MAX;
    if (tdata_sz && !tdata) return UINT32_MAX;

    uint8_t* copy = 0;
    if (tdata_sz) {
        copy = (uint8_t*)malloc(tdata_sz);
        if (!copy) return UINT32_MAX;
        memcpy(copy, tdata, tdata_sz);
    }

    const uint32_t size = round_up(tls_size);
    const uint32_t off  = g_next_offset;
    g_next_offset += size;

    g_mods[g_count].offset   = off;
    g_mods[g_count].tdata_sz = tdata_sz;
    g_mods[g_count].tls_size = size;
    g_mods[g_count].tdata    = copy;
    ++g_count;
    return off;
}

uint32_t scl_tls_module_id(uint32_t offset) {
    for (uint32_t i = 0; i < g_count; ++i)
        if (g_mods[i].offset == offset) return i;   // array index == DTV module id
    return UINT32_MAX;
}

void scl_tls_unregister(uint32_t offset) {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_mods[i].offset == offset) {
            free(g_mods[i].tdata);
            // Leave the slot reserved (offset stays baked into the GOT); mark
            // it inactive so init_area() skips it.
            g_mods[i].tdata    = 0;
            g_mods[i].tdata_sz = 0;
            return;
        }
    }
}

uint32_t scl_tls_area_size(void) { return g_next_offset; }

void scl_tls_init_area(void* area) {
    for (uint32_t i = 0; i < g_count; ++i) {
        const scl_tls_module* m = &g_mods[i];
        if (!m->tdata && m->tls_size == 0) continue;
        uint8_t* dst = (uint8_t*)area + m->offset;
        if (m->tdata_sz) {
            memcpy(dst, m->tdata, m->tdata_sz);
            memset(dst + m->tdata_sz, 0, m->tls_size - m->tdata_sz);
        } else {
            memset(dst, 0, m->tls_size);
        }
    }
}

void* scl_tls_prepare(void) {
    uint32_t sz = scl_tls_area_size();
    if (sz == 0) return 0;
    void* area = malloc(sz);
    if (!area) return 0;
    scl_tls_init_area(area);
    scl_tls_set_pointer(area);
    return area;
}

void scl_tls_set_pointer(void* p) { g_thread_ptr = p; }
void* scl_tls_get_pointer(void)   { return g_thread_ptr; }

void* scl_tls_get_addr(const void* tls_index) {
    const uint32_t* ti = (const uint32_t*)tls_index;
    const uint32_t module = ti[0];   // ti_module (filled by R_ARM_TLS_DTPMOD32)
    const uint32_t offset = ti[1];   // ti_offset (filled by R_ARM_TLS_DTPOFF32)
    if (!g_thread_ptr || module >= g_count) return 0;
    // Variant I: TP == area base; the module's block lives at its area offset.
    return (uint8_t*)g_thread_ptr + g_mods[module].offset + offset;
}

__attribute__((weak)) void* __aeabi_read_tp(void) { return g_thread_ptr; }

__attribute__((weak)) void* __tls_get_addr(const void* ti) { return scl_tls_get_addr(ti); }

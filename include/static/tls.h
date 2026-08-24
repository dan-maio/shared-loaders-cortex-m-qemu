#ifndef SCL_STATIC_TLS_H_
#define SCL_STATIC_TLS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Multi-thread-capable TLS support for the scl::static loader.
 *
 * The loader assigns each loaded module a monotonically increasing byte offset
 * into a shared per-thread "TLS area", and registers a *template* (the module's
 * .tdata image) for it. A thread owns one area of `scl_tls_area_size()` bytes,
 * laid out by `scl_tls_init_area()`, and advertises its base with
 * `scl_tls_set_pointer()`. The soft-TP ABI (`-mtp=soft`) resolves every TLS
 * access through `__aeabi_read_tp()`, which returns that base.
 *
 * Single-thread apps can skip all of that and call `scl_tls_prepare()`, which
 * allocates + initialises + activates one area on the heap.
 */

/// Register a module's TLS: copy @p tdata (tdata_sz bytes) as the module's
/// template, reserve @p tls_size bytes in the area, return the module's offset.
/// Returns UINT32_MAX on failure (no slots / OOM).
uint32_t scl_tls_register(const void* tdata, uint32_t tdata_sz, uint32_t tls_size);

/// Module id (DTV index) for a previously registered @p offset, or UINT32_MAX.
uint32_t scl_tls_module_id(uint32_t offset);

/// Drop a module's TLS template (its offset stays reserved — like a real loader,
/// offsets are never reclaimed because they are baked into the GOT).
void scl_tls_unregister(uint32_t offset);

/// Resolve a global-dynamic tls_index { module, offset } for the current thread.
/// This is the `__tls_get_addr` used by the global-dynamic access model.
void* scl_tls_get_addr(const void* tls_index);

/// Total size of a thread's TLS area (sum of all reserved modules).
uint32_t scl_tls_area_size(void);

/// Initialise one thread's area: copy every registered module's .tdata into
/// @p area at its offset and zero the .tbss. @p area must be
/// scl_tls_area_size() bytes, suitably aligned.
void scl_tls_init_area(void* area);

/// Allocate (heap) + initialise + activate a single TLS area. Convenience for
/// single-thread apps. Returns the area base, or NULL on OOM.
void* scl_tls_prepare(void);

/// Set the current thread's TLS area base (== thread pointer).
void  scl_tls_set_pointer(void* p);

/// Current thread pointer (NULL if none active).
void* scl_tls_get_pointer(void);

#ifdef __cplusplus
}
#endif

#endif // SCL_STATIC_TLS_H_

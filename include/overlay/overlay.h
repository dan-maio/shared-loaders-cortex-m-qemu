/**
 * @file overlay.h
 * @brief Static linker-overlay manager: time-multiplex one RAM window between
 *        modules whose addresses were bound at link time.
 *
 * The linker places N modules at the SAME run address (VMA, the shared window)
 * and stores their bytes back-to-back in flash (each at its own LMA). "Loading"
 * a module is a memcpy LMA -> VMA followed by DSB/ISB (drain the write, flush
 * the pipeline). Only one module is valid in a window at a time; the linker's
 * NOCROSSREFS guarantees modules never reference each other.
 *
 * The application owns the linker script (see src/overlay/ldscripts/
 * overlay-fragment.ld for a documented template) and builds the descriptor
 * table from the symbols it emits (ADDR/LOADADDR/SIZEOF of each module).
 *
 * Loads are traced through the scl_log hook (scl/log.h); with the default
 * no-op hook, loading is silent.
 */
#ifndef SCL_OVERLAY_H_
#define SCL_OVERLAY_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief One overlay module: where it runs, where its bytes live, how big. */
typedef struct scl_overlay_module {
    void*       vma;   /**< Shared window run address (same for every module). */
    const void* lma;   /**< Flash load address of this module's bytes. */
    uint32_t    size;  /**< Module size in bytes (SIZEOF(.ovN)). */
    const char* name;  /**< Short name for tracing; may be NULL. */
} scl_overlay_module;

/** @brief No module resident (fresh window, or before the first load). */
#define SCL_OVERLAY_NONE (-1)

/** @brief One shared window and the table of modules that alias it. */
typedef struct scl_overlay_region {
    const scl_overlay_module* modules; /**< Descriptor table (application-owned). */
    int                       count;   /**< Number of entries in @c modules. */
    int                       resident;/**< Index of the loaded module, or SCL_OVERLAY_NONE. */
} scl_overlay_region;

/** @brief Initialize @p r over @p modules (nothing is copied yet). */
void scl_overlay_init(scl_overlay_region* r,
                      const scl_overlay_module* modules, int count);

/**
 * @brief Make module @p index resident: memcpy its bytes from flash into the
 *        window, then DSB+ISB. No-op if it is already resident.
 * @return 0 on success (or already resident), -1 if @p index is out of range.
 */
int scl_overlay_load(scl_overlay_region* r, int index);

/** @brief Index of the currently resident module, or SCL_OVERLAY_NONE. */
int scl_overlay_current(const scl_overlay_region* r);

#if defined(__cplusplus)
}
#endif
#endif /* SCL_OVERLAY_H_ */

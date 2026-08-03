/**
 * @file fm-iface.h
 * @brief The firmware-main (FM) surface that loadable libs import, shared so FM
 *        and the libraries agree on the struct layout and the trace ABI.
 *
 * FM DEFINES every symbol here and exports them via .dynsym (-Wl,-E); the loader's
 * LinkContext resolves a lib's UND references against main. No symbol here is
 * defined by a library.
 */
#ifndef FM_IFACE_H_
#define FM_IFACE_H_
#include <stdint.h>

/** @brief FM-exported aggregate — the "class" a C lib consumes in test 5. */
struct FmInfo {
    uint32_t id;      /**< Identity tag (FM sets 0x00F1). */
    uint32_t flags;   /**< Arbitrary flags word (FM sets 0x0007). */
};

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Trace facility (defined in FM, imported by every lib) ---- */
/** @brief Trace one event with provenance: "[origin] fn(arg)\n". */
void trace_event(const char* origin, const char* fn, uint32_t arg);
/** @brief Trace "label<decimal v>\n". */
void trace_u32(const char* label, uint32_t v);

/* ---- FM-exported data + callback (tests 2 and 5) ---- */
extern uint32_t fm_var;               /**< FM-owned mutable global (test 5). */
extern const struct FmInfo fm_info;   /**< FM-owned const aggregate (test 5). */
/** @brief FM function a library calls back into (test 2: c1 -> FM). */
uint32_t fm_callback(uint32_t x);

#ifdef __cplusplus
}
#endif
#endif /* FM_IFACE_H_ */

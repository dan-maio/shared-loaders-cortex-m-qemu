#ifndef SCL_STATIC_TLS_H_
#define SCL_STATIC_TLS_H_

#ifdef __cplusplus
extern "C" {
#endif

/// Set the single-thread TLS block base (== the thread pointer). Called by the
/// loader when it allocates a module's TLS block; cleared on unload.
void  scl_tls_set_pointer(void* p);

/// Current thread pointer (nullptr if no TLS module is active).
void* scl_tls_get_pointer(void);

#ifdef __cplusplus
}
#endif

#endif // SCL_STATIC_TLS_H_

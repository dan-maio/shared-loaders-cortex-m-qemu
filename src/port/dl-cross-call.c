/* Thin wrapper: includes the shared cross-call trampoline with dl names. */
#define XIP_CROSS_LOOKUP dl_cross_lookup
#define XIP_CROSS_FN     dl_cross_call
#include "../xip/xip-cross-call.c"

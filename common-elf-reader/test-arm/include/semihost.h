/**
 * @file semihost.h
 * @brief Minimal ARM semihosting output (host console via the debug probe).
 * @note Output-only in 4a; file I/O (SYS_OPEN/READ/SEEK/CLOSE) arrives in 4b.
 *       Calls are no-ops if no semihosting-capable debugger is attached.
 */
#ifndef FW_SEMIHOST_H_
#define FW_SEMIHOST_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Write a NUL-terminated string to the host console (SYS_WRITE0). */
void trace_puts(const char* s);

/** @brief Write one character to the host console (SYS_WRITEC). */
void trace_putc(char c);

/** @brief Open @p path with SYS_OPEN @p mode code (1 == "rb"). Returns handle or -1. */
int  semihost_open(const char* path, int mode);
/** @brief Length in bytes of an open handle, or -1. */
long semihost_flen(int handle);
/** @brief Seek to absolute byte @p pos. Returns 0 on success. */
int  semihost_seek(int handle, long pos);
/** @brief Read @p len bytes into @p buf. Returns bytes NOT read (0 == full read). */
int  semihost_read(int handle, void* buf, int len);
/** @brief Close a handle. Returns 0 on success. */
int  semihost_close(int handle);

/** @brief Read the host command line (SYS_GET_CMDLINE) into @p buf, NUL-terminated
 *         and truncated to @p len. Returns the string length, or -1 if unavailable.
 *         The host sets it via OpenOCD: `arm semihosting_cmdline <args...>`. */
int  semihost_get_cmdline(char* buf, int len);

#if defined(__cplusplus)
}
#endif

#endif /* FW_SEMIHOST_H_ */

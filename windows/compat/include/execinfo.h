// bbport-windows: <execinfo.h> on Windows (CaptureStackBackTrace, windows/compat/posix_compat.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int backtrace(void** buffer, int size);
/* "module+0xoffset [address]" per frame; one malloc'd block, as glibc. */
char** backtrace_symbols(void* const* buffer, int size);
void backtrace_symbols_fd(void* const* buffer, int size, int fd);

#ifdef __cplusplus
}
#endif

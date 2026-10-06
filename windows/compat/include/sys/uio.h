// bbport-windows: <sys/uio.h> on Windows (windows/compat/posix_compat.c).
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
    void* iov_base;
    size_t iov_len;
};

/* Reads/writes that report failure instead of faulting (ReadProcessMemory/WriteProcessMemory).
   bbport uses them on its own process to probe guest memory. */
ptrdiff_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                           const struct iovec* remote, unsigned long riovcnt, unsigned long flags);
ptrdiff_t process_vm_writev(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                            const struct iovec* remote, unsigned long riovcnt, unsigned long flags);

#ifdef __cplusplus
}
#endif

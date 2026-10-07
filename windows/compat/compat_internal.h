// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: helpers shared by the windows/compat implementation files.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stddef.h>

/* errno for a Win32 error code. */
static inline int compat_errno(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_NOT_SUPPORTED: return ENOSYS;
    default: return EINVAL;
    }
}

/* Sets errno from GetLastError() and returns -1. */
static inline int compat_fail(void) {
    errno = compat_errno(GetLastError());
    return -1;
}

/* UTF-8 path to UTF-16 (Win32 wide APIs); 0 and errno on failure. Forward slashes stay. */
static inline int compat_widen(const char* path, wchar_t* out, int capacity) {
    if (!path) {
        errno = EFAULT;
        return 0;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, capacity)) {
        errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : ENOENT;
        return 0;
    }
    return 1;
}

/* gettid() while a handler runs for another thread (signals.c); 0 otherwise. */
extern __thread DWORD compat_tid_override;

/* memfd_create() descriptors (memory.c): their section handle, or NULL for other descriptors. */
HANDLE compat_memfd_section(int fd, unsigned long long* size);

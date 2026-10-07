// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
// bbport-windows: POSIX file functions on Win32: open() in binary mode with POSIX sharing
// (files can be renamed and deleted while open) and directory support, pread/pwrite, stat
// with the Linux struct fields, mkdir with a mode, dirent with d_type, realpath. Paths are
// UTF-8 and go through the wide Win32 functions.
#include "compat_internal.h"

#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#undef open
#undef ftruncate

/* ---- open ------------------------------------------------------------------------------------ */

int bbcompat_open(const char* path, int flags, ...) {
    wchar_t wide[MAX_PATH * 4];
    if (!compat_widen(path, wide, MAX_PATH * 4)) return -1;
    int mode = 0644;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    const DWORD attributes = GetFileAttributesW(wide);
    const int is_directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    if ((flags & O_DIRECTORY) && !is_directory) {
        errno = attributes == INVALID_FILE_ATTRIBUTES ? ENOENT : ENOTDIR;
        return -1;
    }
    DWORD access, disposition, flags_attributes = FILE_ATTRIBUTE_NORMAL;
    int crt = _O_BINARY | (flags & O_CLOEXEC);
    switch (flags & 3) {
    case O_RDONLY: access = GENERIC_READ; crt |= _O_RDONLY; break;
    case O_WRONLY: access = GENERIC_WRITE; crt |= _O_WRONLY; break;
    default: access = GENERIC_READ | GENERIC_WRITE; crt |= _O_RDWR; break;
    }
    if (flags & O_APPEND) crt |= _O_APPEND;
    if ((flags & O_CREAT) && (flags & O_EXCL))
        disposition = CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC))
        disposition = CREATE_ALWAYS;
    else if (flags & O_CREAT)
        disposition = OPEN_ALWAYS;
    else if (flags & O_TRUNC)
        disposition = TRUNCATE_EXISTING;
    else
        disposition = OPEN_EXISTING;
    if (is_directory) {
        if (flags & 3) {
            errno = EISDIR;
            return -1;
        }
        access = FILE_READ_ATTRIBUTES; /* a handle fstat() can query */
        disposition = OPEN_EXISTING;
        flags_attributes = FILE_FLAG_BACKUP_SEMANTICS;
    }
    if ((flags & O_CREAT) && !(mode & 0222)) flags_attributes |= FILE_ATTRIBUTE_READONLY;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, !(flags & O_CLOEXEC)};
    HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                &security, disposition, flags_attributes, NULL);
    if (handle == INVALID_HANDLE_VALUE) return compat_fail();
    const int fd = _open_osfhandle((intptr_t)handle, crt & (_O_RDONLY | _O_WRONLY | _O_RDWR | _O_APPEND | _O_BINARY | _O_NOINHERIT));
    if (fd < 0) {
        CloseHandle(handle);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

/* ---- Positioned I/O ---------------------------------------------------------------------------- */

static HANDLE fd_handle(int fd) {
    const intptr_t handle = _get_osfhandle(fd);
    if (handle == -1 || handle == -2) {
        errno = EBADF;
        return NULL;
    }
    return (HANDLE)handle;
}

static ssize_t positioned(int fd, void* buffer, size_t size, off_t offset, int write) {
    HANDLE handle = fd_handle(fd);
    if (!handle) return -1;
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    /* Overlapped offsets move the file position of synchronous handles: restore it. */
    LARGE_INTEGER zero = {0}, position;
    const BOOL had_position = SetFilePointerEx(handle, zero, &position, FILE_CURRENT);
    size_t done = 0;
    int failed = 0;
    while (done < size) {
        const DWORD chunk = size - done > 0x40000000 ? 0x40000000 : (DWORD)(size - done);
        const uint64_t at = (uint64_t)offset + done;
        OVERLAPPED overlapped = {0};
        overlapped.Offset = (DWORD)at;
        overlapped.OffsetHigh = (DWORD)(at >> 32);
        DWORD n = 0;
        const BOOL ok = write ? WriteFile(handle, (const char*)buffer + done, chunk, &n, &overlapped)
                              : ReadFile(handle, (char*)buffer + done, chunk, &n, &overlapped);
        if (!ok) {
            if (!write && GetLastError() == ERROR_HANDLE_EOF) break;
            failed = 1;
            errno = compat_errno(GetLastError());
            break;
        }
        done += n;
        if (n < chunk) break;
    }
    if (had_position) SetFilePointerEx(handle, position, NULL, FILE_BEGIN);
    return failed && done == 0 ? -1 : (ssize_t)done;
}

ssize_t pread(int fd, void* buffer, size_t size, off_t offset) {
    return positioned(fd, buffer, size, offset, 0);
}

ssize_t pwrite(int fd, const void* buffer, size_t size, off_t offset) {
    return positioned(fd, (void*)buffer, size, offset, 1);
}

int fsync(int fd) {
    HANDLE handle = fd_handle(fd);
    if (!handle) return -1;
    return FlushFileBuffers(handle) || GetLastError() == ERROR_INVALID_HANDLE ? 0 : compat_fail();
}

/* ---- stat ---------------------------------------------------------------------------------------- */

static struct timespec timespec_from(FILETIME time) {
    /* 100 ns ticks since 1601 to the Unix epoch. */
    const int64_t ticks = (int64_t)(((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) - 116444736000000000LL;
    struct timespec ts;
    ts.tv_sec = (time_t)(ticks / 10000000);
    ts.tv_nsec = (long)(ticks % 10000000) * 100;
    return ts;
}

static void fill_stat(struct bbcompat_stat* out, DWORD attributes, uint64_t size, FILETIME accessed,
                      FILETIME modified, FILETIME created, uint64_t links, uint64_t index, uint64_t volume) {
    memset(out, 0, sizeof(*out));
    out->st_dev = volume;
    out->st_ino = index;
    out->st_nlink = links ? links : 1;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        out->st_mode = S_IFDIR | 0755;
    else
        out->st_mode = S_IFREG | ((attributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    out->st_size = (long long)size;
    out->st_blksize = 4096;
    out->st_blocks = (long long)((size + 511) / 512);
    out->st_atim = timespec_from(accessed);
    out->st_mtim = timespec_from(modified);
    out->st_ctim = timespec_from(created);
    out->st_atime = out->st_atim.tv_sec;
    out->st_mtime = out->st_mtim.tv_sec;
    out->st_ctime = out->st_ctim.tv_sec;
}

int bbcompat_stat(const char* path, struct bbcompat_stat* out) {
    wchar_t wide[MAX_PATH * 4];
    if (!compat_widen(path, wide, MAX_PATH * 4)) return -1;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) return compat_fail();
    fill_stat(out, data.dwFileAttributes, ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow,
              data.ftLastAccessTime, data.ftLastWriteTime, data.ftCreationTime, 1, 0, 0);
    return 0;
}

int bbcompat_fstat(int fd, struct bbcompat_stat* out) {
    unsigned long long memfd_size;
    if (compat_memfd_section(fd, &memfd_size)) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        fill_stat(out, 0, memfd_size, now, now, now, 1, 0, 0);
        return 0;
    }
    HANDLE handle = fd_handle(fd);
    if (!handle) return -1;
    const DWORD type = GetFileType(handle);
    if (type != FILE_TYPE_DISK) {
        memset(out, 0, sizeof(*out));
        out->st_mode = type == FILE_TYPE_PIPE ? S_IFIFO | 0600 : S_IFCHR | 0600;
        out->st_nlink = 1;
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) return compat_fail();
    fill_stat(out, info.dwFileAttributes, ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow,
              info.ftLastAccessTime, info.ftLastWriteTime, info.ftCreationTime, info.nNumberOfLinks,
              ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow, info.dwVolumeSerialNumber);
    return 0;
}

int bbcompat_mkdir(const char* path, unsigned int mode) {
    (void)mode;
    wchar_t wide[MAX_PATH * 4];
    if (!compat_widen(path, wide, MAX_PATH * 4)) return -1;
    return CreateDirectoryW(wide, NULL) ? 0 : compat_fail();
}

/* ---- Directories ------------------------------------------------------------------------------- */

struct bbcompat_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int pending; /* data holds an entry not returned yet */
    int fd;
    struct dirent entry;
    char path[MAX_PATH * 4];
};

/* dirfd() descriptors: DIR_FD_BASE + slot. */
#define DIR_FD_BASE 0x50000000
#define DIR_FD_COUNT 64
static struct bbcompat_dir* dir_fds[DIR_FD_COUNT];
static SRWLOCK dir_lock = SRWLOCK_INIT;

DIR* bbcompat_opendir(const char* path) {
    struct bbcompat_dir* dir = calloc(1, sizeof(*dir));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }
    const size_t length = strlen(path);
    if (length + 3 > sizeof(dir->path)) {
        free(dir);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(dir->path, path, length + 1);
    char pattern[MAX_PATH * 4 + 3];
    snprintf(pattern, sizeof(pattern), "%s%s*", path,
             length && (path[length - 1] == '/' || path[length - 1] == '\\') ? "" : "/");
    wchar_t wide[MAX_PATH * 4 + 3];
    if (!compat_widen(pattern, wide, MAX_PATH * 4 + 3)) {
        free(dir);
        return NULL;
    }
    dir->find = FindFirstFileExW(wide, FindExInfoBasic, &dir->data, FindExSearchNameMatch, NULL, 0);
    if (dir->find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        free(dir);
        errno = error == ERROR_DIRECTORY ? ENOTDIR : compat_errno(error);
        return NULL;
    }
    dir->pending = 1;
    dir->fd = -1;
    return dir;
}

struct dirent* bbcompat_readdir(DIR* dir) {
    if (!dir->pending) {
        if (!FindNextFileW(dir->find, &dir->data)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) errno = compat_errno(GetLastError());
            return NULL;
        }
    }
    dir->pending = 0;
    struct dirent* e = &dir->entry;
    if (!WideCharToMultiByte(CP_UTF8, 0, dir->data.cFileName, -1, e->d_name, sizeof(e->d_name), NULL, NULL))
        e->d_name[0] = 0;
    const DWORD attributes = dir->data.dwFileAttributes;
    e->d_type = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? DT_LNK
                : (attributes & FILE_ATTRIBUTE_DIRECTORY)   ? DT_DIR
                                                            : DT_REG;
    e->d_ino = 0;
    e->d_reclen = sizeof(*e);
    return e;
}

void bbcompat_rewinddir(DIR* dir) {
    char path[sizeof(dir->path)];
    memcpy(path, dir->path, sizeof(path));
    DIR* fresh = bbcompat_opendir(path);
    if (!fresh) return;
    FindClose(dir->find);
    dir->find = fresh->find;
    dir->data = fresh->data;
    dir->pending = 1;
    free(fresh);
}

int bbcompat_closedir(DIR* dir) {
    if (dir->fd >= 0) {
        AcquireSRWLockExclusive(&dir_lock);
        dir_fds[dir->fd - DIR_FD_BASE] = NULL;
        ReleaseSRWLockExclusive(&dir_lock);
    }
    FindClose(dir->find);
    free(dir);
    return 0;
}

int bbcompat_dirfd(DIR* dir) {
    if (dir->fd >= 0) return dir->fd;
    AcquireSRWLockExclusive(&dir_lock);
    for (int i = 0; i < DIR_FD_COUNT; i++) {
        if (!dir_fds[i]) {
            dir_fds[i] = dir;
            dir->fd = DIR_FD_BASE + i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&dir_lock);
    if (dir->fd < 0) errno = EMFILE;
    return dir->fd;
}

int bbcompat_fstatat(int dirfd, const char* path, struct bbcompat_stat* out, int flags) {
    (void)flags;
    const int absolute = path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
    if (absolute || dirfd == AT_FDCWD) return bbcompat_stat(path, out);
    const unsigned slot = (unsigned)dirfd - DIR_FD_BASE;
    char joined[MAX_PATH * 8];
    AcquireSRWLockShared(&dir_lock);
    struct bbcompat_dir* dir = dirfd >= DIR_FD_BASE && slot < DIR_FD_COUNT ? dir_fds[slot] : NULL;
    if (dir) snprintf(joined, sizeof(joined), "%s/%s", dir->path, path);
    ReleaseSRWLockShared(&dir_lock);
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    return bbcompat_stat(joined, out);
}

/* ---- realpath ------------------------------------------------------------------------------------ */

char* realpath(const char* path, char* resolved) {
    wchar_t wide[MAX_PATH * 4], full[MAX_PATH * 4];
    if (!compat_widen(path, wide, MAX_PATH * 4)) return NULL;
    const DWORD n = GetFullPathNameW(wide, MAX_PATH * 4, full, NULL);
    if (!n || n >= MAX_PATH * 4) {
        errno = n ? ENAMETOOLONG : compat_errno(GetLastError());
        return NULL;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        errno = ENOENT;
        return NULL;
    }
    char* out = resolved ? resolved : malloc(MAX_PATH * 4);
    if (!out) {
        errno = ENOMEM;
        return NULL;
    }
    /* PATH_MAX (260) bytes are what callers pass. */
    if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, out, resolved ? 260 : MAX_PATH * 4, NULL, NULL)) {
        if (!resolved) free(out);
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (char* p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return out;
}

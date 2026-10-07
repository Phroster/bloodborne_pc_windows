// bbport-windows: <sys/stat.h>. With BB_COMPAT_LINUX_STAT (the C runtime), struct stat has the
// Linux fields (st_atim/st_mtim/st_ctim, st_blocks, st_blksize) and mkdir() takes a mode.
// Other code keeps MinGW's definitions (windows/compat/posix_compat.c).
#pragma once
#include_next <sys/stat.h>
#include <direct.h>
#include <io.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bbcompat_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned long long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long long st_rdev;
    long long st_size;
    long long st_blksize;
    long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    time_t st_atime; /* the seconds of st_atim etc., as the older POSIX names */
    time_t st_mtime;
    time_t st_ctime;
};

int bbcompat_stat(const char* path, struct bbcompat_stat* out);
int bbcompat_fstat(int fd, struct bbcompat_stat* out);
int bbcompat_fstatat(int dirfd, const char* path, struct bbcompat_stat* out, int flags);
int bbcompat_mkdir(const char* path, unsigned int mode);

#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

#ifdef BB_COMPAT_LINUX_STAT
#define stat bbcompat_stat
#define fstat bbcompat_fstat
#define lstat bbcompat_stat
#define fstatat bbcompat_fstatat
#define mkdir bbcompat_mkdir
#endif

#ifdef __cplusplus
}
#endif

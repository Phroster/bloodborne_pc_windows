// bbport-windows: <fcntl.h> with the Linux flags and functions bbport uses. open() always
// opens in binary mode (the CRT default is text mode, which rewrites CR/LF bytes) and can open
// directories (windows/compat/posix_compat.c).
#pragma once
#include_next <fcntl.h>
#include <io.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_DSYNC
#define O_DSYNC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#define O_DIRECTORY 0x200000 /* outside the CRT's _O_* bits */
#define O_CLOEXEC _O_NOINHERIT

#define FALLOC_FL_KEEP_SIZE 0x01
#define FALLOC_FL_PUNCH_HOLE 0x02

int bbcompat_open(const char* path, int flags, ...);
#define open bbcompat_open
/* FALLOC_FL_PUNCH_HOLE on memfd_create() memory: the range reads as zeros again. */
int fallocate(int fd, int mode, off_t offset, off_t length);

#ifdef __cplusplus
}
#endif

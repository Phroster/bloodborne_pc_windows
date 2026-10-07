// bbport-windows: <unistd.h> with the Linux additions bbport uses (windows/compat/posix_compat.c).
#pragma once
#include_next <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#endif

/* The calling thread's id (GetCurrentThreadId). */
pid_t gettid(void);
long sysconf(int name);
int getpagesize(void);
/* Positioned I/O; the file position is left where it was. */
ssize_t pread(int fd, void* buffer, size_t size, off_t offset);
ssize_t pwrite(int fd, const void* buffer, size_t size, off_t offset);
/* Also sizes memfd_create() memory. */
int bbcompat_ftruncate(int fd, off_t length);
#define ftruncate bbcompat_ftruncate
int fsync(int fd);
/* SIGALRM after seconds, handled with the registers of the thread that called alarm(). */
unsigned int alarm(unsigned int seconds);

#ifdef __cplusplus
}
#endif

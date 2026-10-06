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

#ifdef __cplusplus
}
#endif

// bbport-windows: <sys/mman.h> on Windows (windows/compat/posix_compat.c).
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void*)-1)

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15

#define MFD_CLOEXEC 0x1U

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);
int madvise(void* addr, size_t length, int advice);
int memfd_create(const char* name, unsigned int flags);

/* bbport-windows: reserves [start, start + size) for fixed mappings (Windows placeholders).
   Call it early, before other allocations take addresses there; MAP_FIXED mappings of
   memfd_create() memory and anonymous memory in the range then behave as on Linux. */
int bbcompat_reserve_arena(void* start, size_t size);
/* For the exception dispatcher: 1 when an access fault at address is gone (a remap in another
   thread briefly unmapped it) and the instruction should run again. */
int bbcompat_arena_fault_retry(void* address, int write);

#ifdef __cplusplus
}
#endif

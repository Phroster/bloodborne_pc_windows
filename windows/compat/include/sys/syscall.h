// bbport-windows: <sys/syscall.h> on Windows. Linux x86-64 numbers; syscall() implements
// the few bbport makes and fails the rest with ENOSYS (windows/compat/posix_compat.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_arch_prctl 158
#define SYS_gettid 186
#define SYS_tgkill 234
#define SYS_userfaultfd 323
#define SYS_close_range 436

long syscall(long number, ...);

#ifdef __cplusplus
}
#endif

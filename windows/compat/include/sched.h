// bbport-windows: <sched.h> with Linux CPU sets and SCHED_IDLE (windows/compat/posix_compat.c).
#pragma once
#include_next <sched.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

#define CPU_SETSIZE 1024
typedef struct {
    unsigned long long bits[CPU_SETSIZE / 64];
} cpu_set_t;

#define CPU_ZERO(set) __builtin_memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) ((set)->bits[(cpu) / 64] |= 1ULL << ((cpu) % 64))
#define CPU_CLR(cpu, set) ((set)->bits[(cpu) / 64] &= ~(1ULL << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->bits[(cpu) / 64] >> ((cpu) % 64)) & 1)
#define CPU_COUNT(set) bbcompat_cpu_count(set)

int bbcompat_cpu_count(const cpu_set_t* set);
/* The processors the process may run on (its affinity, all processor groups). */
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set);
/* SCHED_IDLE for the calling thread (pid 0): THREAD_PRIORITY_IDLE. Other policies: winpthreads. */
int bbcompat_sched_setscheduler(pid_t pid, int policy, const struct sched_param* param);
#define sched_setscheduler bbcompat_sched_setscheduler

#ifdef __cplusplus
}
#endif

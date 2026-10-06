// bbport-windows: <sys/resource.h> on Windows (windows/compat/posix_compat.c).
#pragma once
#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int id_t;

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD 1

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss; /* KiB, as on Linux */
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

int getrusage(int who, struct rusage* usage);
/* PRIO_PROCESS with a thread id (Linux nice values are per thread): that thread's priority. */
int setpriority(int which, id_t who, int prio);
int getpriority(int which, id_t who);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: condition variables on CLOCK_MONOTONIC over winpthreads, which reads every
// pthread_cond_timedwait deadline as CLOCK_REALTIME. The clock is kept in a spare bit of the
// attribute (winpthreads stores only the process-shared flag there); conditions created with it
// are remembered and wait for the time left until their deadline instead.
#include "compat_internal.h"

#include <stdint.h>
#include <time.h>

#include <pthread.h>

#undef pthread_condattr_setclock
#undef pthread_condattr_getclock
#undef pthread_cond_init
#undef pthread_cond_timedwait
#undef pthread_cond_destroy

#define MONOTONIC_BIT 0x100

/* Open-addressing set of the monotonic conditions' addresses. */
#define SET_SIZE 8192
static pthread_cond_t* monotonic[SET_SIZE];
static SRWLOCK set_lock = SRWLOCK_INIT;
static pthread_cond_t* const removed = (pthread_cond_t*)(uintptr_t)1;

static size_t slot_of(const pthread_cond_t* cond) {
    return (size_t)(((uintptr_t)cond >> 3) * 0x9E3779B97F4A7C15ull >> 51) % SET_SIZE;
}

static int set_insert(pthread_cond_t* cond) {
    AcquireSRWLockExclusive(&set_lock);
    for (size_t i = slot_of(cond), n = 0; n < SET_SIZE; i = (i + 1) % SET_SIZE, n++) {
        if (!monotonic[i] || monotonic[i] == removed || monotonic[i] == cond) {
            monotonic[i] = cond;
            ReleaseSRWLockExclusive(&set_lock);
            return 0;
        }
    }
    ReleaseSRWLockExclusive(&set_lock);
    return ENOMEM;
}

/* Finds cond; removes it as well when erase is set. */
static int set_find(pthread_cond_t* cond, int erase) {
    if (erase)
        AcquireSRWLockExclusive(&set_lock);
    else
        AcquireSRWLockShared(&set_lock);
    int found = 0;
    for (size_t i = slot_of(cond), n = 0; n < SET_SIZE && monotonic[i]; i = (i + 1) % SET_SIZE, n++) {
        if (monotonic[i] == cond) {
            found = 1;
            if (erase) monotonic[i] = removed;
            break;
        }
    }
    if (erase)
        ReleaseSRWLockExclusive(&set_lock);
    else
        ReleaseSRWLockShared(&set_lock);
    return found;
}

int bbcompat_pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock) {
    if (clock == CLOCK_MONOTONIC)
        *attr |= MONOTONIC_BIT;
    else if (clock == CLOCK_REALTIME)
        *attr &= ~MONOTONIC_BIT;
    else
        return EINVAL;
    return 0;
}

int bbcompat_pthread_condattr_getclock(const pthread_condattr_t* attr, clockid_t* clock) {
    *clock = (*attr & MONOTONIC_BIT) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    return 0;
}

int bbcompat_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    pthread_condattr_t plain = attr ? (*attr & ~MONOTONIC_BIT) : 0;
    const int error = pthread_cond_init(cond, attr ? &plain : NULL);
    if (error) return error;
    if (attr && (*attr & MONOTONIC_BIT)) return set_insert(cond);
    set_find(cond, 1); /* a reused address that was monotonic before */
    return 0;
}

int bbcompat_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* deadline) {
    if (!set_find(cond, 0)) return pthread_cond_timedwait(cond, mutex, deadline);
    struct timespec now, left = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t ns = ((int64_t)deadline->tv_sec - now.tv_sec) * 1000000000 + (deadline->tv_nsec - now.tv_nsec);
    if (ns > 0) {
        left.tv_sec = (time_t)(ns / 1000000000);
        left.tv_nsec = (long)(ns % 1000000000);
    }
    return pthread_cond_timedwait_relative_np(cond, mutex, &left);
}

int bbcompat_pthread_cond_destroy(pthread_cond_t* cond) {
    set_find(cond, 1);
    return pthread_cond_destroy(cond);
}

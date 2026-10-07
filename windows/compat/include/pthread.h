// bbport-windows: <pthread.h> (winpthreads) with the glibc names bbport uses.
#pragma once
#include_next <pthread.h>

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* The calling thread's stack (GetCurrentThreadStackLimits) as pthread_attr_getstack reports it. */
int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);

/* Condition variables on CLOCK_MONOTONIC: winpthreads reads every deadline as CLOCK_REALTIME.
   Conditions initialized with such an attribute wait for the remaining time instead. */
int bbcompat_pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock);
int bbcompat_pthread_condattr_getclock(const pthread_condattr_t* attr, clockid_t* clock);
int bbcompat_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int bbcompat_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* deadline);
int bbcompat_pthread_cond_destroy(pthread_cond_t* cond);
#define pthread_condattr_setclock bbcompat_pthread_condattr_setclock
#define pthread_condattr_getclock bbcompat_pthread_condattr_getclock
#define pthread_cond_init bbcompat_pthread_cond_init
#define pthread_cond_timedwait bbcompat_pthread_cond_timedwait
#define pthread_cond_destroy bbcompat_pthread_cond_destroy
#ifdef __cplusplus
}
#endif

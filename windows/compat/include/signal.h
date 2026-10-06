// bbport-windows: <signal.h> with the POSIX parts bbport uses. sigaction() handlers for
// SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP are called from a vectored exception handler
// (windows/compat/posix_compat.c); see <ucontext.h> for the register layout they get.
#pragma once
#include_next <signal.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux numbers for the signals the Windows CRT does not define. */
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#define BB_COMPAT_NSIG 32

#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRERR 2
#define TRAP_BRKPT 1
#define TRAP_TRACE 2

#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#define SS_ONSTACK 1
#define SS_DISABLE 2

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    void* si_addr;
} siginfo_t;

typedef struct {
    void* ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t*, void*);
    sigset_t sa_mask;
    int sa_flags;
};

int sigaction(int sig, const struct sigaction* act, struct sigaction* old);
/* Accepted and ignored: exception handlers run on the faulting thread's stack. */
int sigaltstack(const stack_t* ss, stack_t* old);

static inline int sigemptyset(sigset_t* set) { *set = 0; return 0; }
static inline int sigfillset(sigset_t* set) { *set = (sigset_t)~(sigset_t)0; return 0; }
static inline int sigaddset(sigset_t* set, int sig) { *set |= (sigset_t)1 << sig; return 0; }
static inline int sigdelset(sigset_t* set, int sig) { *set &= ~((sigset_t)1 << sig); return 0; }
static inline int sigismember(const sigset_t* set, int sig) { return (int)((*set >> sig) & 1); }

#ifdef __cplusplus
}
#endif

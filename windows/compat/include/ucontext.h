// bbport-windows: <ucontext.h> on Windows. Signal handlers registered with the compat
// sigaction() get the faulting thread's registers in the Linux x86-64 layout (gregs[REG_*]);
// the dispatcher copies them from and back to the Windows CONTEXT (windows/compat/posix_compat.c).
#pragma once
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long greg_t;
#define NGREG 23
typedef greg_t gregset_t[NGREG];

enum {
    REG_R8 = 0,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_RDI,
    REG_RSI,
    REG_RBP,
    REG_RBX,
    REG_RDX,
    REG_RAX,
    REG_RCX,
    REG_RSP,
    REG_RIP,
    REG_EFL,
    REG_CSGSFS,
    REG_ERR,
    REG_TRAPNO,
    REG_OLDMASK,
    REG_CR2,
};
#define REG_R8 REG_R8
#define REG_R9 REG_R9
#define REG_R10 REG_R10
#define REG_R11 REG_R11
#define REG_R12 REG_R12
#define REG_R13 REG_R13
#define REG_R14 REG_R14
#define REG_R15 REG_R15
#define REG_RDI REG_RDI
#define REG_RSI REG_RSI
#define REG_RBP REG_RBP
#define REG_RBX REG_RBX
#define REG_RDX REG_RDX
#define REG_RAX REG_RAX
#define REG_RCX REG_RCX
#define REG_RSP REG_RSP
#define REG_RIP REG_RIP
#define REG_EFL REG_EFL
#define REG_CSGSFS REG_CSGSFS
#define REG_ERR REG_ERR
#define REG_TRAPNO REG_TRAPNO
#define REG_OLDMASK REG_OLDMASK
#define REG_CR2 REG_CR2

typedef struct {
    gregset_t gregs;
    void* fpregs; /* the Windows CONTEXT's XMM save area */
} mcontext_t;

typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t* uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
    void* uc_win_context; /* bbport-windows: the PCONTEXT being dispatched */
} ucontext_t;

#ifdef __cplusplus
}
#endif

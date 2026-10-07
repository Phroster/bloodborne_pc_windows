// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: Linux signals on Win32. One vectored exception handler turns hardware
// exceptions into the signals bbport's handlers expect (SIGSEGV, SIGBUS, SIGILL, SIGFPE,
// SIGTRAP), with the registers in a Linux-layout ucontext_t, and copies their changes back.
#include "compat_internal.h"
#include <tlhelp32.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>


static struct sigaction actions[BB_COMPAT_NSIG];
static SRWLOCK actions_lock = SRWLOCK_INIT;
static INIT_ONCE handler_once = INIT_ONCE_STATIC_INIT;

/* siglongjmp out of a handler jumps back into dispatch() (still inside the exception
   dispatch), which resumes the thread in jump_trampoline() on its own stack. */
static __thread jmp_buf* dispatch_return;
static __thread void* pending_env;
static __thread int pending_value;

__attribute__((noreturn, used)) static void jump_trampoline(void* env, int value) {
    longjmp(*(jmp_buf*)env, value);
}

void bbcompat_siglongjmp(jmp_buf env, int value) {
    if (dispatch_return) {
        jmp_buf* back = dispatch_return;
        dispatch_return = NULL;
        pending_env = env;
        pending_value = value ? value : 1;
        longjmp(*back, 1);
    }
    longjmp(env, value ? value : 1);
}

static void to_ucontext(const CONTEXT* c, const EXCEPTION_RECORD* record, int sig, ucontext_t* uc) {
    memset(uc, 0, sizeof(*uc));
    greg_t* g = uc->uc_mcontext.gregs;
    g[REG_R8] = (greg_t)c->R8;
    g[REG_R9] = (greg_t)c->R9;
    g[REG_R10] = (greg_t)c->R10;
    g[REG_R11] = (greg_t)c->R11;
    g[REG_R12] = (greg_t)c->R12;
    g[REG_R13] = (greg_t)c->R13;
    g[REG_R14] = (greg_t)c->R14;
    g[REG_R15] = (greg_t)c->R15;
    g[REG_RDI] = (greg_t)c->Rdi;
    g[REG_RSI] = (greg_t)c->Rsi;
    g[REG_RBP] = (greg_t)c->Rbp;
    g[REG_RBX] = (greg_t)c->Rbx;
    g[REG_RDX] = (greg_t)c->Rdx;
    g[REG_RAX] = (greg_t)c->Rax;
    g[REG_RCX] = (greg_t)c->Rcx;
    g[REG_RSP] = (greg_t)c->Rsp;
    g[REG_RIP] = (greg_t)c->Rip;
    g[REG_EFL] = (greg_t)c->EFlags;
    g[REG_CSGSFS] = (greg_t)c->SegCs | ((greg_t)c->SegGs << 16) | ((greg_t)c->SegFs << 32);
    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR: {
        /* Page-fault error code as Linux reports it: present, user, write (1) or fetch (8). */
        const ULONG_PTR kind = record->ExceptionInformation[0];
        g[REG_TRAPNO] = 14;
        g[REG_ERR] = 0x5 | (kind == 1 ? 0x2 : 0) | (kind == 8 ? 0x10 : 0);
        g[REG_CR2] = (greg_t)record->ExceptionInformation[1];
        break;
    }
    case EXCEPTION_BREAKPOINT:
        g[REG_TRAPNO] = 3;
        g[REG_RIP] += 1; /* Linux reports the address after int3; Windows the int3 itself */
        break;
    case EXCEPTION_SINGLE_STEP: g[REG_TRAPNO] = 1; break;
    default: g[REG_TRAPNO] = sig == SIGILL ? 6 : 0; break;
    }
    uc->uc_mcontext.fpregs = (void*)&c->FltSave;
    uc->uc_win_context = (void*)c;
}

static void from_ucontext(const ucontext_t* uc, CONTEXT* c) {
    const greg_t* g = uc->uc_mcontext.gregs;
    c->R8 = (DWORD64)g[REG_R8];
    c->R9 = (DWORD64)g[REG_R9];
    c->R10 = (DWORD64)g[REG_R10];
    c->R11 = (DWORD64)g[REG_R11];
    c->R12 = (DWORD64)g[REG_R12];
    c->R13 = (DWORD64)g[REG_R13];
    c->R14 = (DWORD64)g[REG_R14];
    c->R15 = (DWORD64)g[REG_R15];
    c->Rdi = (DWORD64)g[REG_RDI];
    c->Rsi = (DWORD64)g[REG_RSI];
    c->Rbp = (DWORD64)g[REG_RBP];
    c->Rbx = (DWORD64)g[REG_RBX];
    c->Rdx = (DWORD64)g[REG_RDX];
    c->Rax = (DWORD64)g[REG_RAX];
    c->Rcx = (DWORD64)g[REG_RCX];
    c->Rsp = (DWORD64)g[REG_RSP];
    c->Rip = (DWORD64)g[REG_RIP];
    c->EFlags = (DWORD)g[REG_EFL];
}

static LONG CALLBACK dispatch(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    CONTEXT* context = pointers->ContextRecord;
    siginfo_t info = {0};
    int sig;
    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        sig = SIGSEGV;
        info.si_code = SEGV_ACCERR;
        info.si_addr = (void*)record->ExceptionInformation[1];
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        sig = SIGBUS;
        info.si_code = BUS_ADRERR;
        info.si_addr = (void*)record->ExceptionInformation[1];
        break;
    case EXCEPTION_BREAKPOINT:
        sig = SIGTRAP;
        info.si_code = TRAP_BRKPT;
        info.si_addr = record->ExceptionAddress;
        break;
    case EXCEPTION_SINGLE_STEP:
        sig = SIGTRAP;
        info.si_code = TRAP_TRACE;
        info.si_addr = record->ExceptionAddress;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        sig = SIGILL;
        info.si_addr = record->ExceptionAddress;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
        sig = SIGFPE;
        info.si_addr = record->ExceptionAddress;
        break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    info.si_signo = sig;
    /* BB_FAULT_LOG=N (diagnostic): the first N access faults, with the faulting code address. */
    static long fault_log = -1;
    if (fault_log < 0) {
        const char* value = getenv("BB_FAULT_LOG");
        fault_log = value ? atol(value) : 0;
    }
    static volatile LONG faults_logged;
    if (fault_log > 0 && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        InterlockedIncrement(&faults_logged) <= fault_log)
        fprintf(stderr, "Fault: rip=%p %s %p (thread %lu, rsp=%p)\n", (void*)context->Rip,
                record->ExceptionInformation[0] == 1 ? "write" : record->ExceptionInformation[0] == 8 ? "exec" : "read",
                info.si_addr, GetCurrentThreadId(), (void*)context->Rsp);
    /* A view another thread is remapping (memory.c) was briefly absent: run the access again. */
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->ExceptionInformation[0] != 8 &&
        bbcompat_arena_fault_retry(info.si_addr, record->ExceptionInformation[0] == 1))
        return EXCEPTION_CONTINUE_EXECUTION;

    struct sigaction action;
    AcquireSRWLockShared(&actions_lock);
    action = actions[sig];
    ReleaseSRWLockShared(&actions_lock);
    const int has_info_handler = (action.sa_flags & SA_SIGINFO) && action.sa_sigaction;
    const int has_handler = !(action.sa_flags & SA_SIGINFO) && action.sa_handler != SIG_DFL &&
                            action.sa_handler != SIG_IGN && action.sa_handler != SIG_ERR;
    if (!has_info_handler && !has_handler) return EXCEPTION_CONTINUE_SEARCH;

    ucontext_t uc;
    to_ucontext(context, record, sig, &uc);
    jmp_buf back;
    jmp_buf* const outer = dispatch_return;
    if (setjmp(back)) {
        /* The handler called siglongjmp: continue the thread in jump_trampoline(env, value),
           below the interrupted code's red zone, aligned as right after a call. */
        dispatch_return = outer;
        DWORD64 sp = ((context->Rsp - 128 - 32) & ~(DWORD64)15) - 8;
        context->Rsp = sp;
        context->Rip = (DWORD64)(uintptr_t)jump_trampoline;
        context->Rcx = (DWORD64)(uintptr_t)pending_env;
        context->Rdx = (DWORD64)pending_value;
        context->EFlags &= ~(DWORD)0x100; /* trap flag */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    dispatch_return = &back;
    if (has_info_handler)
        action.sa_sigaction(sig, &info, &uc);
    else
        action.sa_handler(sig);
    dispatch_return = outer;
    if (action.sa_flags & SA_RESETHAND) {
        AcquireSRWLockExclusive(&actions_lock);
        memset(&actions[sig], 0, sizeof(actions[sig]));
        ReleaseSRWLockExclusive(&actions_lock);
    }
    from_ucontext(&uc, context);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static BOOL CALLBACK install_handler(INIT_ONCE* once, void* parameter, void** context) {
    (void)once, (void)parameter, (void)context;
    return AddVectoredExceptionHandler(1, dispatch) != NULL;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    if (sig <= 0 || sig >= BB_COMPAT_NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (!InitOnceExecuteOnce(&handler_once, install_handler, NULL, NULL)) return compat_fail();
    AcquireSRWLockExclusive(&actions_lock);
    if (old) *old = actions[sig];
    if (act) actions[sig] = *act;
    ReleaseSRWLockExclusive(&actions_lock);
    /* Console signals still go through the CRT. */
    if (act && !(act->sa_flags & SA_SIGINFO) &&
        (sig == SIGINT || sig == SIGTERM || sig == SIGABRT || sig == SIGBREAK))
        signal(sig, act->sa_handler);
    return 0;
}

int sigaltstack(const stack_t* ss, stack_t* old) {
    (void)ss;
    if (old) {
        memset(old, 0, sizeof(*old));
        old->ss_flags = SS_DISABLE;
    }
    return 0;
}

/* ---- Signals for other threads: alarm(), the watchdog's thread dump ------------------------- */
/* Linux delivers these on the target thread. Here the target is suspended and the handler runs
   on the calling thread with the target's registers; gettid() reports the target meanwhile. */

__thread DWORD compat_tid_override;

/* The thread's name and host call chain (Windows unwind data; guest code has none, so the chain
   ends at the first guest frame, which the bbport handler then follows by its frame pointers). */
static void print_host_chain(DWORD tid, HANDLE thread, CONTEXT* start) {
    wchar_t* description = NULL;
    char name[64] = "";
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), NULL, NULL);
        LocalFree(description);
    }
    CONTEXT context = *start;
    char line[1024];
    int n = snprintf(line, sizeof(line), "  thread %lu \"%s\":", tid, name);
    for (int depth = 0; depth < 16 && context.Rip && n < (int)sizeof(line) - 64; depth++) {
        HMODULE module = NULL;
        char path[MAX_PATH] = "?";
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)context.Rip, &module);
        if (module) GetModuleFileNameA(module, path, sizeof(path));
        const char* base = strrchr(path, '\\');
        n += snprintf(line + n, sizeof(line) - n, module ? " %s+0x%llx" : " [%s%p]", module ? (base ? base + 1 : path) : "",
                      module ? (void*)(uintptr_t)(context.Rip - (DWORD64)module) : (void*)context.Rip);
        DWORD64 image_base;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (!function) break; /* guest code, or a leaf in a module without unwind data */
        void* handler_data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &frame, NULL);
    }
    fprintf(stderr, "%s\n", line);
}

static void run_for_thread(DWORD tid, int sig) {
    if (tid == GetCurrentThreadId()) return;
    struct sigaction action;
    AcquireSRWLockShared(&actions_lock);
    action = actions[sig];
    ReleaseSRWLockShared(&actions_lock);
    if (!(action.sa_flags & SA_SIGINFO) || !action.sa_sigaction) return;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) return;
    if (SuspendThread(thread) != (DWORD)-1) {
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            print_host_chain(tid, thread, &context);
            EXCEPTION_RECORD record;
            memset(&record, 0, sizeof(record));
            ucontext_t uc;
            to_ucontext(&context, &record, sig, &uc);
            siginfo_t info = {0};
            info.si_signo = sig;
            compat_tid_override = tid;
            action.sa_sigaction(sig, &info, &uc);
            compat_tid_override = 0;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
}

int bbcompat_signal_other_threads(int sig) {
    if (sig <= 0 || sig >= BB_COMPAT_NSIG) {
        errno = EINVAL;
        return -1;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return compat_fail();
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    int count = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
            run_for_thread(entry.th32ThreadID, sig);
            count++;
        }
    }
    CloseHandle(snapshot);
    return count;
}

static SRWLOCK alarm_lock = SRWLOCK_INIT;
static HANDLE alarm_event;
static ULONGLONG alarm_deadline; /* GetTickCount64 milliseconds; 0: disarmed */
static DWORD alarm_target;

static DWORD WINAPI alarm_main(void* parameter) {
    (void)parameter;
    for (;;) {
        AcquireSRWLockShared(&alarm_lock);
        const ULONGLONG deadline = alarm_deadline;
        ReleaseSRWLockShared(&alarm_lock);
        DWORD wait = INFINITE;
        if (deadline) {
            const ULONGLONG now = GetTickCount64();
            wait = deadline <= now ? 0 : deadline - now > 0x7fffffff ? 0x7fffffff : (DWORD)(deadline - now);
        }
        if (WaitForSingleObject(alarm_event, wait) == WAIT_OBJECT_0) continue; /* re-armed */
        AcquireSRWLockExclusive(&alarm_lock);
        const int fire = alarm_deadline && GetTickCount64() >= alarm_deadline;
        const DWORD target = alarm_target;
        if (fire) alarm_deadline = 0;
        ReleaseSRWLockExclusive(&alarm_lock);
        if (fire) run_for_thread(target, SIGALRM);
    }
    return 0;
}

static BOOL CALLBACK start_alarm_thread(INIT_ONCE* once, void* parameter, void** context) {
    (void)once, (void)parameter, (void)context;
    alarm_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!alarm_event) return FALSE;
    HANDLE thread = CreateThread(NULL, 0, alarm_main, NULL, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

unsigned int alarm(unsigned int seconds) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!InitOnceExecuteOnce(&once, start_alarm_thread, NULL, NULL)) return 0;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&alarm_lock);
    const unsigned remaining =
        alarm_deadline > now ? (unsigned)((alarm_deadline - now + 999) / 1000) : 0;
    alarm_deadline = seconds ? now + (ULONGLONG)seconds * 1000 : 0;
    alarm_target = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&alarm_lock);
    SetEvent(alarm_event);
    return remaining;
}

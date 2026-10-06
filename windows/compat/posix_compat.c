// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: the POSIX/Linux functions bbport uses, implemented on Win32. The headers in
// windows/compat/include declare them under their Linux names, so upstream code compiles
// unchanged on Windows.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

/* ---- Memory ------------------------------------------------------------------------------ */

static DWORD win_protect(int prot) {
    switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE: return PAGE_NOACCESS;
    case PROT_READ: return PAGE_READONLY;
    case PROT_WRITE:
    case PROT_READ | PROT_WRITE: return PAGE_READWRITE;
    case PROT_EXEC: return PAGE_EXECUTE;
    case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

static int errno_from_win(DWORD error) {
    switch (error) {
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_ACCESS_DENIED: return EACCES;
    default: return EINVAL;
    }
}

static int fail_win(void) {
    errno = errno_from_win(GetLastError());
    return -1;
}

/* Calls fn for each VirtualQuery region overlapping [address, address + length), clipped. */
typedef int (*RegionFn)(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data);
static int for_each_region(void* address, size_t length, RegionFn fn, void* data) {
    char* at = address;
    char* const end = at + length;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(at, &info, sizeof(info))) return fail_win();
        char* region_end = (char*)info.BaseAddress + info.RegionSize;
        if (region_end > end) region_end = end;
        if (fn(&info, at, region_end, data)) return -1;
        at = region_end;
    }
    return 0;
}

static int region_is_free(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    (void)begin, (void)end;
    *(int*)data &= info->State == MEM_FREE;
    return 0;
}

/* Gives private memory new, zeroed contents and protection (MAP_FIXED over a reservation). */
static int replace_private(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    const int prot = *(const int*)data;
    if (info->State == MEM_FREE || info->Type != MEM_PRIVATE) {
        errno = EINVAL; /* file views and unreserved gaps cannot be replaced in place */
        return -1;
    }
    if (info->State == MEM_COMMIT && !VirtualFree(begin, (size_t)(end - begin), MEM_DECOMMIT))
        return fail_win();
    if (prot != PROT_NONE && !VirtualAlloc(begin, (size_t)(end - begin), MEM_COMMIT, win_protect(prot)))
        return fail_win();
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)offset;
    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (!(flags & MAP_ANONYMOUS) || fd != -1) {
        /* bbport-windows TODO: file and memfd mappings come with the runtime's memory model. */
        errno = ENODEV;
        return MAP_FAILED;
    }
    /* PROT_NONE is a reservation; mprotect() commits it. */
    const DWORD type = prot == PROT_NONE ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT;
    const DWORD protect = win_protect(prot);
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        int free = 1;
        if (for_each_region(addr, length, region_is_free, &free)) return MAP_FAILED;
        if (free) {
            void* p = VirtualAlloc(addr, length, type, protect);
            if (p == addr) return p;
            if (p) VirtualFree(p, 0, MEM_RELEASE); /* rounded down to the 64 KiB granularity */
            errno = p ? EINVAL : errno_from_win(GetLastError());
            return MAP_FAILED;
        }
        if (flags & MAP_FIXED_NOREPLACE) {
            errno = EEXIST;
            return MAP_FAILED;
        }
        return for_each_region(addr, length, replace_private, &prot) ? MAP_FAILED : addr;
    }
    void* p = addr ? VirtualAlloc(addr, length, type, protect) : NULL;
    if (!p) p = VirtualAlloc(NULL, length, type, protect);
    if (!p) {
        errno = errno_from_win(GetLastError());
        return MAP_FAILED;
    }
    return p;
}

static char* allocation_end(void* base) {
    char* at = base;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery(at, &info, sizeof(info)) && info.AllocationBase == base)
        at = (char*)info.BaseAddress + info.RegionSize;
    return at;
}

static int unmap_region(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    char* const* range = data;
    if (info->State == MEM_FREE) return 0;
    if (info->Type == MEM_MAPPED || info->Type == MEM_IMAGE) {
        if (info->Type == MEM_MAPPED && !UnmapViewOfFile(info->AllocationBase)) return fail_win();
        return 0;
    }
    if (info->State == MEM_COMMIT && !VirtualFree(begin, (size_t)(end - begin), MEM_DECOMMIT))
        return fail_win();
    /* Release the reservation once all of it is inside the unmapped range. */
    char* base = info->AllocationBase;
    if (base >= range[0] && allocation_end(base) <= range[1]) VirtualFree(base, 0, MEM_RELEASE);
    return 0;
}

int munmap(void* addr, size_t length) {
    char* range[2] = {addr, (char*)addr + length};
    return for_each_region(addr, length, unmap_region, range);
}

static int protect_region(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    const int prot = *(const int*)data;
    const size_t size = (size_t)(end - begin);
    DWORD old;
    if (info->State == MEM_FREE) {
        errno = ENOMEM;
        return -1;
    }
    if (info->State == MEM_RESERVE) {
        if (prot == PROT_NONE) return 0;
        return VirtualAlloc(begin, size, MEM_COMMIT, win_protect(prot)) ? 0 : fail_win();
    }
    return VirtualProtect(begin, size, win_protect(prot), &old) ? 0 : fail_win();
}

int mprotect(void* addr, size_t length, int prot) {
    return for_each_region(addr, length, protect_region, &prot);
}

static int discard_region(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    (void)data;
    /* MADV_DONTNEED zero-fills private memory; shared mappings keep their contents. */
    if (info->State != MEM_COMMIT || info->Type != MEM_PRIVATE) return 0;
    const size_t size = (size_t)(end - begin);
    if (!VirtualFree(begin, size, MEM_DECOMMIT)) return fail_win();
    return VirtualAlloc(begin, size, MEM_COMMIT, info->Protect) ? 0 : fail_win();
}

int madvise(void* addr, size_t length, int advice) {
    if (advice == MADV_DONTNEED) return for_each_region(addr, length, discard_region, NULL);
    return 0;
}

int memfd_create(const char* name, unsigned int flags) {
    (void)name, (void)flags;
    errno = ENOSYS; /* bbport-windows TODO: section objects, with the runtime's memory model */
    return -1;
}

/* ---- Process memory access --------------------------------------------------------------- */

static ptrdiff_t vm_copy(pid_t pid, const struct iovec* local, unsigned long local_count,
                         const struct iovec* remote, unsigned long remote_count, int write) {
    const int self = pid == 0 || (DWORD)pid == GetCurrentProcessId();
    HANDLE process = self ? GetCurrentProcess()
                          : OpenProcess(write ? PROCESS_VM_WRITE | PROCESS_VM_OPERATION : PROCESS_VM_READ,
                                        FALSE, (DWORD)pid);
    if (!process) {
        errno = ESRCH;
        return -1;
    }
    ptrdiff_t total = 0;
    unsigned long li = 0, ri = 0;
    size_t lo = 0, ro = 0;
    int failed = 0;
    while (li < local_count && ri < remote_count && !failed) {
        size_t n = local[li].iov_len - lo;
        if (remote[ri].iov_len - ro < n) n = remote[ri].iov_len - ro;
        if (n) {
            char* l = (char*)local[li].iov_base + lo;
            char* r = (char*)remote[ri].iov_base + ro;
            SIZE_T done = 0;
            const BOOL ok = write ? WriteProcessMemory(process, r, l, n, &done)
                                  : ReadProcessMemory(process, r, l, n, &done);
            total += (ptrdiff_t)done;
            failed = !ok || done != n;
        }
        lo += n;
        ro += n;
        if (lo == local[li].iov_len) li++, lo = 0;
        if (ro == remote[ri].iov_len) ri++, ro = 0;
    }
    if (!self) CloseHandle(process);
    if (failed && total == 0) {
        errno = EFAULT;
        return -1;
    }
    return total;
}

ptrdiff_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                           const struct iovec* remote, unsigned long riovcnt, unsigned long flags) {
    (void)flags;
    return vm_copy(pid, local, liovcnt, remote, riovcnt, 0);
}

ptrdiff_t process_vm_writev(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                            const struct iovec* remote, unsigned long riovcnt, unsigned long flags) {
    (void)flags;
    return vm_copy(pid, local, liovcnt, remote, riovcnt, 1);
}

/* ---- Resources, priorities, CPUs ---------------------------------------------------------- */

static struct timeval timeval_from(FILETIME time) {
    const uint64_t us = (((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) / 10;
    struct timeval tv;
    tv.tv_sec = (long)(us / 1000000);
    tv.tv_usec = (long)(us % 1000000);
    return tv;
}

int getrusage(int who, struct rusage* usage) {
    memset(usage, 0, sizeof(*usage));
    FILETIME created, exited, kernel, user;
    if (who == RUSAGE_CHILDREN) return 0;
    if (who == RUSAGE_THREAD) {
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return fail_win();
    } else if (who == RUSAGE_SELF) {
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return fail_win();
    } else {
        errno = EINVAL;
        return -1;
    }
    usage->ru_utime = timeval_from(user);
    usage->ru_stime = timeval_from(kernel);
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        usage->ru_maxrss = (long)(counters.PeakWorkingSetSize / 1024);
        usage->ru_minflt = (long)counters.PageFaultCount;
    }
    return 0;
}

static int priority_from_nice(int nice) {
    if (nice >= 19) return THREAD_PRIORITY_IDLE;
    if (nice >= 10) return THREAD_PRIORITY_LOWEST;
    if (nice > 0) return THREAD_PRIORITY_BELOW_NORMAL;
    if (nice == 0) return THREAD_PRIORITY_NORMAL;
    if (nice > -10) return THREAD_PRIORITY_ABOVE_NORMAL;
    return THREAD_PRIORITY_HIGHEST;
}

static int nice_from_priority(int priority) {
    switch (priority) {
    case THREAD_PRIORITY_IDLE: return 19;
    case THREAD_PRIORITY_LOWEST: return 10;
    case THREAD_PRIORITY_BELOW_NORMAL: return 5;
    case THREAD_PRIORITY_ABOVE_NORMAL: return -5;
    case THREAD_PRIORITY_HIGHEST:
    case THREAD_PRIORITY_TIME_CRITICAL: return -10;
    default: return 0;
    }
}

/* Linux nice values are per thread: PRIO_PROCESS with 0 or a thread id means that thread. */
static HANDLE open_thread(id_t who, DWORD access) {
    if (who == 0 || who == GetCurrentThreadId()) return GetCurrentThread();
    return OpenThread(access, FALSE, who);
}

int setpriority(int which, id_t who, int prio) {
    if (which != PRIO_PROCESS) {
        errno = EINVAL;
        return -1;
    }
    HANDLE thread = open_thread(who, THREAD_SET_LIMITED_INFORMATION);
    if (!thread) {
        errno = ESRCH;
        return -1;
    }
    const BOOL ok = SetThreadPriority(thread, priority_from_nice(prio));
    if (thread != GetCurrentThread()) CloseHandle(thread);
    return ok ? 0 : fail_win();
}

int getpriority(int which, id_t who) {
    if (which != PRIO_PROCESS) {
        errno = EINVAL;
        return -1;
    }
    HANDLE thread = open_thread(who, THREAD_QUERY_LIMITED_INFORMATION);
    if (!thread) {
        errno = ESRCH;
        return -1;
    }
    const int priority = GetThreadPriority(thread);
    if (thread != GetCurrentThread()) CloseHandle(thread);
    return nice_from_priority(priority);
}

int bbcompat_cpu_count(const cpu_set_t* set) {
    int count = 0;
    for (size_t i = 0; i < sizeof(set->bits) / sizeof(set->bits[0]); i++)
        count += __builtin_popcountll(set->bits[i]);
    return count;
}

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set) {
    (void)pid;
    memset(set, 0, size < sizeof(*set) ? size : sizeof(*set));
    USHORT groups[16];
    USHORT group_count = 16;
    DWORD_PTR process_mask, system_mask;
    if (GetProcessGroupAffinity(GetCurrentProcess(), &group_count, groups) && group_count > 1) {
        /* Several processor groups (more than 64 logical CPUs): count them all. */
        const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        for (DWORD cpu = 0; cpu < count && cpu < CPU_SETSIZE; cpu++) CPU_SET(cpu, set);
        return 0;
    }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return fail_win();
    for (int cpu = 0; cpu < 64; cpu++)
        if ((process_mask >> cpu) & 1) CPU_SET(cpu, set);
    return 0;
}

#undef sched_setscheduler
int bbcompat_sched_setscheduler(pid_t pid, int policy, const struct sched_param* param) {
    if (policy == SCHED_IDLE || policy == SCHED_BATCH) {
        if (pid != 0 && (DWORD)pid != GetCurrentThreadId()) {
            errno = EPERM;
            return -1;
        }
        const int priority = policy == SCHED_IDLE ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_BELOW_NORMAL;
        return SetThreadPriority(GetCurrentThread(), priority) ? 0 : fail_win();
    }
    return sched_setscheduler(pid, policy, param);
}

/* ---- Environment -------------------------------------------------------------------------- */

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : (errno = EINVAL, -1);
}

int unsetenv(const char* name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    return _putenv_s(name, "") == 0 ? 0 : (errno = EINVAL, -1);
}

/* ---- unistd, syscall ---------------------------------------------------------------------- */

pid_t gettid(void) {
    return (pid_t)GetCurrentThreadId();
}

long sysconf(int name) {
    SYSTEM_INFO info;
    switch (name) {
    case _SC_PAGESIZE:
        GetSystemInfo(&info);
        return (long)info.dwPageSize;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    default: errno = EINVAL; return -1;
    }
}

int getpagesize(void) {
    return (int)sysconf(_SC_PAGESIZE);
}

long syscall(long number, ...) {
    switch (number) {
    case SYS_gettid: return (long)GetCurrentThreadId();
    default: errno = ENOSYS; return -1;
    }
}

/* ---- dlfcn -------------------------------------------------------------------------------- */

static __thread char dl_message[300];
static __thread int dl_failed;
static __thread char dl_module_name[MAX_PATH];

static void dl_fail(const char* what) {
    snprintf(dl_message, sizeof(dl_message), "%s: Windows error %lu", what ? what : "(null)",
             GetLastError());
    dl_failed = 1;
}

void* dlopen(const char* file, int mode) {
    if (!file) return GetModuleHandleW(NULL);
    HMODULE module = (mode & RTLD_NOLOAD) ? GetModuleHandleA(file) : LoadLibraryA(file);
    if (!module) dl_fail(file);
    return module;
}

void* dlsym(void* handle, const char* name) {
    if (handle == RTLD_DEFAULT) {
        HMODULE modules[512];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
            const DWORD count = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
            for (DWORD i = 0; i < count; i++) {
                FARPROC symbol = GetProcAddress(modules[i], name);
                if (symbol) return (void*)symbol;
            }
        }
        dl_fail(name);
        return NULL;
    }
    FARPROC symbol = GetProcAddress((HMODULE)handle, name);
    if (!symbol) dl_fail(name);
    return (void*)symbol;
}

int dlclose(void* handle) {
    if (!handle || handle == GetModuleHandleW(NULL)) return 0;
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

char* dlerror(void) {
    if (!dl_failed) return NULL;
    dl_failed = 0;
    return dl_message;
}

/* dli_fname points to a per-thread buffer, valid until this thread's next dladdr(). */
int dladdr(const void* address, Dl_info* info) {
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module))
        return 0;
    if (!GetModuleFileNameA(module, dl_module_name, sizeof(dl_module_name))) dl_module_name[0] = 0;
    info->dli_fname = dl_module_name;
    info->dli_fbase = module;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

/* ---- execinfo ----------------------------------------------------------------------------- */

int backtrace(void** buffer, int size) {
    if (size <= 0) return 0;
    return RtlCaptureStackBackTrace(1, (DWORD)size, buffer, NULL);
}

static int describe_frame(void* address, char* out, size_t size) {
    HMODULE module;
    char path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module) &&
        GetModuleFileNameA(module, path, sizeof(path))) {
        const char* name = strrchr(path, '\\');
        return snprintf(out, size, "%s+0x%llx [%p]", name ? name + 1 : path,
                        (unsigned long long)((char*)address - (char*)module), address);
    }
    return snprintf(out, size, "[%p]", address);
}

char** backtrace_symbols(void* const* buffer, int size) {
    if (size <= 0) return NULL;
    enum { Line = MAX_PATH + 48 };
    char** lines = malloc((size_t)size * (sizeof(char*) + Line));
    if (!lines) return NULL;
    char* text = (char*)(lines + size);
    for (int i = 0; i < size; i++) {
        lines[i] = text + (size_t)i * Line;
        describe_frame(buffer[i], lines[i], Line);
    }
    return lines;
}

void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
    char line[MAX_PATH + 48];
    for (int i = 0; i < size; i++) {
        int n = describe_frame(buffer[i], line, sizeof(line) - 1);
        if (n < 0) continue;
        if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
        line[n++] = '\n';
        _write(fd, line, (unsigned)n);
    }
}

/* ---- Signals ------------------------------------------------------------------------------ */
/* One vectored exception handler turns hardware exceptions into the Linux signals bbport's
   handlers expect, with the registers in a ucontext_t, and copies their changes back. */

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
    if (!InitOnceExecuteOnce(&handler_once, install_handler, NULL, NULL)) return fail_win();
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

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: the smaller POSIX/Linux functions bbport uses, on Win32: process memory
// access, resource usage, priorities and CPU sets, environment, unistd, syscall, dlfcn,
// execinfo, getrandom, IPv4 text conversion, strcasestr.
#include "compat_internal.h"
#include <psapi.h>
#include <bcrypt.h>

#include <ctype.h>
#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sched.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

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
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return compat_fail();
    } else if (who == RUSAGE_SELF) {
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return compat_fail();
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
    return ok ? 0 : compat_fail();
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
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return compat_fail();
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
        return SetThreadPriority(GetCurrentThread(), priority) ? 0 : compat_fail();
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
    return (pid_t)(compat_tid_override ? compat_tid_override : GetCurrentThreadId());
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


/* ---- Random, IPv4 text, strings ---------------------------------------------------------- */

ssize_t getrandom(void* buffer, size_t length, unsigned int flags) {
    (void)flags;
    for (size_t done = 0; done < length;) {
        const ULONG n = length - done > 0x10000000 ? 0x10000000 : (ULONG)(length - done);
        if (BCryptGenRandom(NULL, (unsigned char*)buffer + done, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            errno = EIO;
            return -1;
        }
        done += n;
    }
    return (ssize_t)length;
}

int bbcompat_inet_pton(int family, const char* text, void* out) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned char bytes[4];
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*text)) return 0;
        unsigned value = 0;
        for (int digits = 0; isdigit((unsigned char)*text); digits++, text++) {
            if (digits == 3 || (digits == 1 && value == 0)) return 0; /* no leading zeros */
            value = value * 10 + (unsigned)(*text - '0');
        }
        if (value > 255 || (i < 3 && *text++ != '.')) return 0;
        bytes[i] = (unsigned char)value;
    }
    if (*text) return 0;
    memcpy(out, bytes, 4);
    return 1;
}

const char* bbcompat_inet_ntop(int family, const void* address, char* out, unsigned int size) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    const unsigned char* b = address;
    char text[16];
    const int n = snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if (n < 0 || (unsigned)n >= size) {
        errno = ENOSPC;
        return NULL;
    }
    memcpy(out, text, (size_t)n + 1);
    return out;
}

char* bbcompat_strcasestr(const char* haystack, const char* needle) {
    const size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (!_strnicmp(haystack, needle, n)) return (char*)haystack;
    return n ? NULL : (char*)haystack;
}

/* ---- Time zone, thread attributes ---------------------------------------------------------- */

long bbcompat_tm_gmtoff(const struct tm* local) {
    /* The broken-down local time read as UTC, minus the time it stands for. */
    struct tm as_utc = *local, as_local = *local;
    return (long)(_mkgmtime(&as_utc) - mktime(&as_local));
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) {
    if (!pthread_equal(thread, pthread_self())) return ENOSYS; /* the calling thread only */
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    int error = pthread_attr_init(attr);
    if (!error) error = pthread_attr_setstack(attr, (void*)low, (size_t)(high - low));
    return error;
}

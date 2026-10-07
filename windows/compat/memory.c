// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: mmap/munmap/mprotect/madvise and memfd_create/ftruncate/fallocate on Win32.
//
// The runtime maps pieces of one shared-memory file (a memfd) at fixed guest addresses, at
// 16 KiB granularity, replacing whatever was there, and the same memory can appear at several
// addresses. On Windows the memfd is a pagefile-backed section, and the guest address range is
// reserved up front as placeholders (bbcompat_reserve_arena, Windows 10 1803+). A fixed mapping
// there splits the placeholders to the exact range and replaces them with a view of the section
// (MapViewOfFile3) or with private memory (VirtualAlloc2). Unmapping returns the range to a
// placeholder; a view unmapped in part is mapped again around the hole, with its protections.
// The scheme follows shadPS4's src/core/address_space.cpp (GPL-2.0-or-later).
#include "compat_internal.h"

#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

/* Resolved at run time: MinGW's import libraries and headers do not all have them. */
typedef PVOID(WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID(WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef BOOL(WINAPI* UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;

static BOOL CALLBACK resolve_placeholder_api(INIT_ONCE* once, void* parameter, void** context) {
    (void)once, (void)parameter, (void)context;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase) return FALSE;
    virtual_alloc2 = (VirtualAlloc2Fn)(void*)GetProcAddress(kernelbase, "VirtualAlloc2");
    map_view3 = (MapViewOfFile3Fn)(void*)GetProcAddress(kernelbase, "MapViewOfFile3");
    unmap_view2 = (UnmapViewOfFile2Fn)(void*)GetProcAddress(kernelbase, "UnmapViewOfFile2");
    return virtual_alloc2 && map_view3 && unmap_view2;
}

static int placeholder_api(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (InitOnceExecuteOnce(&once, resolve_placeholder_api, NULL, NULL)) return 1;
    errno = ENOSYS; /* Windows 10 1803 or newer */
    return 0;
}

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

/* ---- memfd -------------------------------------------------------------------------------- */
/* Descriptors MEMFD_BASE + i; the section is created by ftruncate(), when the size is known. */

#define MEMFD_BASE 0x40000000
#define MEMFD_COUNT 8
typedef struct {
    int used;
    HANDLE section;
    unsigned long long size;
    unsigned char* view; /* whole section, for fallocate() */
} MemFd;
static MemFd memfds[MEMFD_COUNT];
static SRWLOCK memfd_lock = SRWLOCK_INIT;

static MemFd* memfd(int fd) {
    const unsigned index = (unsigned)fd - MEMFD_BASE;
    return fd >= MEMFD_BASE && index < MEMFD_COUNT && memfds[index].used ? &memfds[index] : NULL;
}

HANDLE compat_memfd_section(int fd, unsigned long long* size) {
    MemFd* m = memfd(fd);
    if (!m) return NULL;
    if (size) *size = m->size;
    return m->section;
}

int memfd_create(const char* name, unsigned int flags) {
    (void)name, (void)flags;
    AcquireSRWLockExclusive(&memfd_lock);
    for (int i = 0; i < MEMFD_COUNT; i++) {
        if (!memfds[i].used) {
            memset(&memfds[i], 0, sizeof(memfds[i]));
            memfds[i].used = 1;
            ReleaseSRWLockExclusive(&memfd_lock);
            return MEMFD_BASE + i;
        }
    }
    ReleaseSRWLockExclusive(&memfd_lock);
    errno = EMFILE;
    return -1;
}

int bbcompat_ftruncate(int fd, off_t length) {
    MemFd* m = memfd(fd);
    if (!m) {
        const errno_t error = _chsize_s(fd, (long long)length);
        if (error) errno = error;
        return error ? -1 : 0;
    }
    if (length <= 0) {
        errno = EINVAL;
        return -1;
    }
    AcquireSRWLockExclusive(&memfd_lock);
    int result = 0;
    if (m->section) {
        /* Pagefile-backed sections cannot be resized. */
        if ((unsigned long long)length != m->size) errno = EINVAL, result = -1;
    } else {
        const unsigned long long size = (unsigned long long)length;
        /* SEC_COMMIT: charged against the commit limit up front, physical pages on first use. */
        m->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                        (DWORD)(size >> 32), (DWORD)size, NULL);
        if (m->section)
            m->size = size;
        else
            result = compat_fail();
    }
    ReleaseSRWLockExclusive(&memfd_lock);
    return result;
}

int fallocate(int fd, int mode, off_t offset, off_t length) {
    MemFd* m = memfd(fd);
    if (!m || !(mode & FALLOC_FL_PUNCH_HOLE) || !(mode & FALLOC_FL_KEEP_SIZE)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (!m->section || offset < 0 || length < 0 || (unsigned long long)offset > m->size ||
        (unsigned long long)length > m->size - (unsigned long long)offset) {
        errno = EINVAL;
        return -1;
    }
    AcquireSRWLockExclusive(&memfd_lock);
    if (!m->view) m->view = MapViewOfFile(m->section, FILE_MAP_WRITE, 0, 0, 0);
    unsigned char* view = m->view;
    ReleaseSRWLockExclusive(&memfd_lock);
    if (!view) return compat_fail();
    /* A pagefile section has no hole punching: the range is cleared (bbport-windows TODO:
       skip pages that were never touched). */
    memset(view + offset, 0, (size_t)length);
    return 0;
}

/* ---- Placeholder arena ---------------------------------------------------------------------- */

enum { REGION_FREE, REGION_VIEW, REGION_PRIVATE };
typedef struct {
    uintptr_t start, end;
    int kind;        /* REGION_FREE: a placeholder */
    int reservation; /* placeholders coalesce only within one VirtualAlloc2 reservation */
    HANDLE section;  /* REGION_VIEW */
    uint64_t offset;
} Region;

static Region* regions;
static size_t region_count, region_capacity;
static SRWLOCK arena_lock = SRWLOCK_INIT;
static uintptr_t arena_min, arena_max;

static int in_arena(uintptr_t address, size_t length) {
    return arena_max && address >= arena_min && address < arena_max && length <= arena_max - address;
}

/* First region with end > address. */
static size_t region_at(uintptr_t address) {
    size_t lo = 0, hi = region_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (regions[mid].end <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int region_insert(size_t at, Region region) {
    if (region_count == region_capacity) {
        const size_t capacity = region_capacity ? region_capacity * 2 : 256;
        Region* grown = realloc(regions, capacity * sizeof(*regions));
        if (!grown) {
            errno = ENOMEM;
            return -1;
        }
        regions = grown;
        region_capacity = capacity;
    }
    memmove(regions + at + 1, regions + at, (region_count - at) * sizeof(*regions));
    regions[at] = region;
    region_count++;
    return 0;
}

static void region_erase(size_t at, size_t count) {
    memmove(regions + at, regions + at + count, (region_count - at - count) * sizeof(*regions));
    region_count -= count;
}

int bbcompat_reserve_arena(void* start, size_t size) {
    if (!placeholder_api()) return -1;
    const uintptr_t granularity = 64 * 1024;
    uintptr_t at = ((uintptr_t)start + granularity - 1) & ~(granularity - 1);
    const uintptr_t end = ((uintptr_t)start + size) & ~(granularity - 1);
    static int next_reservation = 1;
    int reserved = 0;
    AcquireSRWLockExclusive(&arena_lock);
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)at, &info, sizeof(info))) break;
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next > end) next = end;
        if (info.State == MEM_FREE) {
            const uintptr_t s = (at + granularity - 1) & ~(granularity - 1);
            const uintptr_t e = next & ~(granularity - 1);
            if (e > s && virtual_alloc2(GetCurrentProcess(), (void*)s, e - s,
                                        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
                const Region region = {s, e, REGION_FREE, next_reservation++, NULL, 0};
                if (region_insert(region_at(s), region) == 0) reserved = 1;
            }
        }
        at = next;
    }
    if (reserved) {
        if (!arena_max || (uintptr_t)start < arena_min) arena_min = (uintptr_t)start;
        if ((uintptr_t)start + size > arena_max) arena_max = (uintptr_t)start + size;
    }
    ReleaseSRWLockExclusive(&arena_lock);
    if (!reserved) errno = ENOMEM;
    return reserved ? 0 : -1;
}

/* Splits placeholder i at address (inside it): [start, address) stays at i. */
static int split_free(size_t i, uintptr_t address) {
    Region right = regions[i];
    if (!VirtualFree((void*)right.start, address - right.start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        return compat_fail();
    right.start = address;
    regions[i].end = address;
    return region_insert(i + 1, right);
}

/* A view of section at offset over placeholder i (exactly its size), then protection prot. */
static int map_view(size_t i, HANDLE section, uint64_t offset, DWORD protect) {
    Region* r = &regions[i];
    const size_t size = r->end - r->start;
    if (!map_view3(section, GetCurrentProcess(), (void*)r->start, offset, size, MEM_REPLACE_PLACEHOLDER,
                   PAGE_EXECUTE_READWRITE, NULL, 0))
        return compat_fail();
    r->kind = REGION_VIEW;
    r->section = section;
    r->offset = offset;
    DWORD old;
    if (protect != PAGE_EXECUTE_READWRITE && !VirtualProtect((void*)r->start, size, protect, &old))
        return compat_fail();
    return 0;
}

/* Returns region i to a placeholder. */
static int unmap_one(size_t i) {
    Region* r = &regions[i];
    const BOOL ok = r->kind == REGION_VIEW
                        ? unmap_view2(GetCurrentProcess(), (void*)r->start, MEM_PRESERVE_PLACEHOLDER)
                        : VirtualFree((void*)r->start, r->end - r->start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (!ok) return compat_fail();
    r->kind = REGION_FREE;
    r->section = NULL;
    r->offset = 0;
    return 0;
}

typedef struct {
    uintptr_t start, end;
    DWORD protect;
} Run;

/* Protections of [start, end), as runs of equal protection. */
static Run* capture_runs(uintptr_t start, uintptr_t end, size_t* count) {
    size_t capacity = 16, n = 0;
    Run* runs = malloc(capacity * sizeof(*runs));
    for (uintptr_t at = start; runs && at < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)at, &info, sizeof(info))) break;
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next > end) next = end;
        if (n == capacity) {
            Run* grown = realloc(runs, (capacity *= 2) * sizeof(*runs));
            if (!grown) {
                free(runs);
                return NULL;
            }
            runs = grown;
        }
        runs[n++] = (Run){at, next, info.Protect};
        at = next;
    }
    *count = n;
    return runs;
}

static void restore_runs(const Run* runs, size_t count, uintptr_t start, uintptr_t end) {
    for (size_t k = 0; k < count; k++) {
        const uintptr_t a = runs[k].start > start ? runs[k].start : start;
        const uintptr_t b = runs[k].end < end ? runs[k].end : end;
        DWORD old;
        if (a < b) VirtualProtect((void*)a, b - a, runs[k].protect, &old);
    }
}

/* Makes [a, b) placeholders; the parts of views outside it stay mapped. */
static int release_range(uintptr_t a, uintptr_t b) {
    for (size_t i = region_at(a); i < region_count && regions[i].start < b;) {
        const Region r = regions[i];
        if (r.kind == REGION_FREE) {
            i++;
            continue;
        }
        const uintptr_t cut_a = r.start > a ? r.start : a;
        const uintptr_t cut_b = r.end < b ? r.end : b;
        const int partial = cut_a != r.start || cut_b != r.end;
        if (partial && r.kind == REGION_PRIVATE) {
            errno = EINVAL; /* private memory is only unmapped whole */
            return -1;
        }
        size_t run_count = 0;
        Run* runs = partial ? capture_runs(r.start, r.end, &run_count) : NULL;
        if (partial && !runs) {
            errno = ENOMEM;
            return -1;
        }
        int result = unmap_one(i);
        if (!result && r.start < cut_a) {
            /* Map the part before the hole again. */
            result = split_free(i, cut_a);
            if (!result) result = map_view(i, r.section, r.offset, PAGE_EXECUTE_READWRITE);
            if (!result) restore_runs(runs, run_count, r.start, cut_a);
            i++;
        }
        if (!result && cut_b < r.end) {
            /* And the part after it. */
            result = split_free(i, cut_b);
            if (!result) result = map_view(i + 1, r.section, r.offset + (cut_b - r.start), PAGE_EXECUTE_READWRITE);
            if (!result) restore_runs(runs, run_count, cut_b, r.end);
        }
        free(runs);
        if (result) return result;
        i = region_at(cut_b);
    }
    return 0;
}

/* One placeholder exactly [a, b) (after release_range); its index, or -1. */
static long carve(uintptr_t a, uintptr_t b) {
    size_t i = region_at(a);
    /* The range must be covered by placeholders of one reservation. */
    uintptr_t at = a;
    for (size_t k = i; at < b; k++) {
        if (k == region_count || regions[k].start > at || regions[k].kind != REGION_FREE ||
            regions[k].reservation != regions[i].reservation) {
            errno = ENOMEM;
            return -1;
        }
        at = regions[k].end;
    }
    if (regions[i].start < a) {
        if (split_free(i, a)) return -1;
        i++;
    }
    size_t j = region_at(b - 1);
    if (regions[j].end > b && split_free(j, b)) return -1;
    if (j > i) {
        if (!VirtualFree((void*)regions[i].start, b - regions[i].start, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
            return compat_fail();
        regions[i].end = b;
        region_erase(i + 1, j - i);
    }
    return (long)i;
}

static void* arena_map(uintptr_t a, size_t length, int prot, int flags, HANDLE section, uint64_t offset) {
    const uintptr_t b = a + length;
    void* result = MAP_FAILED;
    AcquireSRWLockExclusive(&arena_lock);
    if (flags & MAP_FIXED_NOREPLACE) {
        for (size_t i = region_at(a); i < region_count && regions[i].start < b; i++) {
            if (regions[i].kind != REGION_FREE) {
                errno = EEXIST;
                goto done;
            }
        }
    }
    if (release_range(a, b)) goto done;
    const long i = carve(a, b);
    if (i < 0) goto done;
    if (section) {
        if (map_view((size_t)i, section, offset, win_protect(prot))) goto done;
    } else if (prot != PROT_NONE) {
        /* Anonymous PROT_NONE stays a placeholder: any access faults, as it would. */
        if (!virtual_alloc2(GetCurrentProcess(), (void*)a, length, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                            win_protect(prot), NULL, 0)) {
            compat_fail();
            goto done;
        }
        regions[i].kind = REGION_PRIVATE;
    }
    result = (void*)a;
done:
    ReleaseSRWLockExclusive(&arena_lock);
    return result;
}

int bbcompat_arena_fault_retry(void* address, int write) {
    const uintptr_t a = (uintptr_t)address;
    if (!in_arena(a, 1)) return 0;
    /* Waits for a thread that is remapping (a view unmapped in part is briefly absent). */
    AcquireSRWLockShared(&arena_lock);
    const size_t i = region_at(a);
    int retry = 0;
    if (i < region_count && regions[i].start <= a && regions[i].kind != REGION_FREE) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_COMMIT) {
            const DWORD p = info.Protect & 0xff;
            const int readable = p != PAGE_NOACCESS && p != PAGE_EXECUTE;
            const int writable = p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY ||
                                 p == PAGE_EXECUTE_WRITECOPY;
            retry = write ? writable : readable;
        }
    }
    ReleaseSRWLockShared(&arena_lock);
    return retry;
}

/* ---- mmap and friends ----------------------------------------------------------------------- */

static int region_is_free(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data);
typedef int (*RegionFn)(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data);

/* Calls fn for each VirtualQuery region overlapping [address, address + length), clipped. */
static int for_each_region(void* address, size_t length, RegionFn fn, void* data) {
    char* at = address;
    char* const end = at + length;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(at, &info, sizeof(info))) return compat_fail();
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
    if (info->State == MEM_COMMIT && !VirtualFree(begin, (size_t)(end - begin), MEM_DECOMMIT)) return compat_fail();
    if (prot != PROT_NONE && !VirtualAlloc(begin, (size_t)(end - begin), MEM_COMMIT, win_protect(prot)))
        return compat_fail();
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (length == 0 || offset < 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    length = (length + 4095) & ~(size_t)4095;
    HANDLE section = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        unsigned long long size = 0;
        section = compat_memfd_section(fd, &size);
        if (!section) {
            /* bbport-windows TODO: mappings of regular files (the runtime maps none). */
            errno = ENODEV;
            return MAP_FAILED;
        }
        if ((unsigned long long)offset > size || length > size - (unsigned long long)offset) {
            errno = EINVAL;
            return MAP_FAILED;
        }
    }
    const int fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && in_arena((uintptr_t)addr, length)) {
        if ((uintptr_t)addr & 4095) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (!placeholder_api()) return MAP_FAILED;
        return arena_map((uintptr_t)addr, length, prot, flags, section, (uint64_t)offset);
    }
    if (section) {
        /* A view anywhere (the runtime's second view of its memory): 64 KiB aligned offsets. */
        if (fixed || ((uint64_t)offset & 0xffff)) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (!placeholder_api()) return MAP_FAILED;
        void* p = map_view3(section, GetCurrentProcess(), NULL, (uint64_t)offset, length, 0, PAGE_EXECUTE_READWRITE, NULL, 0);
        DWORD old;
        if (!p || (win_protect(prot) != PAGE_EXECUTE_READWRITE && !VirtualProtect(p, length, win_protect(prot), &old))) {
            compat_fail();
            if (p) UnmapViewOfFile(p);
            return MAP_FAILED;
        }
        return p;
    }
    /* Anonymous memory outside the arena. PROT_NONE is a reservation; mprotect() commits it. */
    const DWORD type = prot == PROT_NONE ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT;
    const DWORD protect = win_protect(prot);
    if (fixed) {
        int free_range = 1;
        if (for_each_region(addr, length, region_is_free, &free_range)) return MAP_FAILED;
        if (free_range) {
            void* p = VirtualAlloc(addr, length, type, protect);
            if (p == addr) return p;
            if (p) VirtualFree(p, 0, MEM_RELEASE); /* rounded down to the 64 KiB granularity */
            errno = p ? EINVAL : compat_errno(GetLastError());
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
        errno = compat_errno(GetLastError());
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
        if (info->Type == MEM_MAPPED && !UnmapViewOfFile(info->AllocationBase)) return compat_fail();
        return 0;
    }
    if (info->State == MEM_COMMIT && !VirtualFree(begin, (size_t)(end - begin), MEM_DECOMMIT)) return compat_fail();
    /* Release the reservation once all of it is inside the unmapped range. */
    char* base = info->AllocationBase;
    if (base >= range[0] && allocation_end(base) <= range[1]) VirtualFree(base, 0, MEM_RELEASE);
    return 0;
}

int munmap(void* addr, size_t length) {
    length = (length + 4095) & ~(size_t)4095;
    if (in_arena((uintptr_t)addr, length)) {
        AcquireSRWLockExclusive(&arena_lock);
        const int result = release_range((uintptr_t)addr, (uintptr_t)addr + length);
        ReleaseSRWLockExclusive(&arena_lock);
        return result;
    }
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
        return VirtualAlloc(begin, size, MEM_COMMIT, win_protect(prot)) ? 0 : compat_fail();
    }
    return VirtualProtect(begin, size, win_protect(prot), &old) ? 0 : compat_fail();
}

int mprotect(void* addr, size_t length, int prot) {
    length = (length + 4095) & ~(size_t)4095;
    if (in_arena((uintptr_t)addr, length)) {
        const uintptr_t a = (uintptr_t)addr, b = a + length;
        const DWORD protect = win_protect(prot);
        int result = 0;
        AcquireSRWLockShared(&arena_lock);
        uintptr_t at = a;
        for (size_t i = region_at(a); at < b; i++) {
            if (i == region_count || regions[i].start > at || regions[i].kind == REGION_FREE) {
                errno = ENOMEM; /* unmapped pages, as Linux */
                result = -1;
                break;
            }
            const uintptr_t end = regions[i].end < b ? regions[i].end : b;
            DWORD old;
            if (!VirtualProtect((void*)at, end - at, protect, &old)) {
                result = compat_fail();
                break;
            }
            at = end;
        }
        ReleaseSRWLockShared(&arena_lock);
        return result;
    }
    return for_each_region(addr, length, protect_region, &prot);
}

static int discard_region(const MEMORY_BASIC_INFORMATION* info, char* begin, char* end, void* data) {
    (void)data;
    /* MADV_DONTNEED zero-fills private memory; shared mappings keep their contents. */
    if (info->State != MEM_COMMIT || info->Type != MEM_PRIVATE) return 0;
    const size_t size = (size_t)(end - begin);
    if (!VirtualFree(begin, size, MEM_DECOMMIT)) return compat_fail();
    return VirtualAlloc(begin, size, MEM_COMMIT, info->Protect) ? 0 : compat_fail();
}

int madvise(void* addr, size_t length, int advice) {
    if (advice == MADV_DONTNEED) return for_each_region(addr, length, discard_region, NULL);
    return 0;
}

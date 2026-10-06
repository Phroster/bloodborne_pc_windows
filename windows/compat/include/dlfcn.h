// bbport-windows: <dlfcn.h> on Windows (LoadLibrary/GetProcAddress, windows/compat/posix_compat.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_NOLOAD 0x4
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void*)0)

typedef struct {
    const char* dli_fname; /* module path */
    void* dli_fbase;       /* module base */
    const char* dli_sname; /* always NULL: no symbol names without debug info */
    void* dli_saddr;
} Dl_info;

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
int dladdr(const void* address, Dl_info* info);

#ifdef __cplusplus
}
#endif

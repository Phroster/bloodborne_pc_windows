// bbport-windows: <stdlib.h> with setenv/unsetenv (windows/compat/posix_compat.c).
// No include guard around include_next: libc++'s own stdlib.h wrapper is included in several
// passes, each of which must reach it.
#include_next <stdlib.h>

#ifndef BB_COMPAT_STDLIB_DECLARED
#define BB_COMPAT_STDLIB_DECLARED
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
#ifdef __cplusplus
}
#endif
#endif

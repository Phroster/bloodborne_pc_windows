// bbport-windows: <stdlib.h> with setenv/unsetenv/realpath (windows/compat/posix_compat.c).
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
/* The absolute path of an existing file, or NULL (_fullpath). */
char* realpath(const char* path, char* resolved);
#ifdef __cplusplus
}
#endif
#endif

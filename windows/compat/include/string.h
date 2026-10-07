// bbport-windows: <string.h> with strcasestr. No include guard around include_next: libc++'s
// own string.h wrapper must be reached on every pass.
#include_next <string.h>

#ifndef BB_COMPAT_STRING_DECLARED
#define BB_COMPAT_STRING_DECLARED
#ifdef __cplusplus
extern "C" {
#endif
char* bbcompat_strcasestr(const char* haystack, const char* needle);
#ifndef __cplusplus
#define strcasestr bbcompat_strcasestr
#endif
#ifdef __cplusplus
}
#endif
#endif

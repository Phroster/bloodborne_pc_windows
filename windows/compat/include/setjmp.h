// bbport-windows: <setjmp.h> with POSIX semantics. setjmp/longjmp do not unwind (as glibc):
// unwinding would walk through game code, which has no Windows unwind data. siglongjmp out of
// a signal handler is routed through the exception dispatcher (windows/compat/posix_compat.c).
#pragma once
#ifndef __USE_MINGW_SETJMP_NON_SEH
#define __USE_MINGW_SETJMP_NON_SEH 1
#endif
#include_next <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savemask) setjmp(env)
__attribute__((noreturn)) void bbcompat_siglongjmp(jmp_buf env, int value);
#define siglongjmp(env, value) bbcompat_siglongjmp((env), (value))

#ifdef __cplusplus
}
#endif

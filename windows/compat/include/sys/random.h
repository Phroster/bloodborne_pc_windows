// bbport-windows: <sys/random.h> (BCryptGenRandom, windows/compat/posix_compat.c).
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x1
#define GRND_RANDOM 0x2

ssize_t getrandom(void* buffer, size_t length, unsigned int flags);

#ifdef __cplusplus
}
#endif

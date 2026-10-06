// bbport-windows: <sys/types.h> with sigset_t, which MinGW only defines with _POSIX.
#pragma once
#include_next <sys/types.h>

#ifndef _POSIX
typedef _sigset_t sigset_t;
#endif

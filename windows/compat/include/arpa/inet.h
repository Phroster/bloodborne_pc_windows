// bbport-windows: <arpa/inet.h> without Winsock: byte order and IPv4 text conversion
// (windows/compat/posix_compat.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AF_INET
#define AF_INET 2
#endif

static inline uint16_t bbcompat_htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bbcompat_htonl(uint32_t v) { return __builtin_bswap32(v); }
#define htons bbcompat_htons
#define ntohs bbcompat_htons
#define htonl bbcompat_htonl
#define ntohl bbcompat_htonl

int bbcompat_inet_pton(int family, const char* text, void* out);
const char* bbcompat_inet_ntop(int family, const void* address, char* out, unsigned int size);
#define inet_pton bbcompat_inet_pton
#define inet_ntop bbcompat_inet_ntop

#ifdef __cplusplus
}
#endif

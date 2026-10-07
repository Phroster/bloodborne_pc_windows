// bbport-windows: <dirent.h> with d_type (FindFirstFile, windows/compat/posix_compat.c). Replaces
// MinGW's, which has no d_type.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

struct dirent {
    unsigned long long d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1024]; /* UTF-8 */
};
typedef struct bbcompat_dir DIR;

DIR* bbcompat_opendir(const char* path);
struct dirent* bbcompat_readdir(DIR* dir);
int bbcompat_closedir(DIR* dir);
void bbcompat_rewinddir(DIR* dir);
/* A descriptor fstatat() accepts as the directory. */
int bbcompat_dirfd(DIR* dir);
#define opendir bbcompat_opendir
#define readdir bbcompat_readdir
#define closedir bbcompat_closedir
#define rewinddir bbcompat_rewinddir
#define dirfd bbcompat_dirfd

#ifdef __cplusplus
}
#endif

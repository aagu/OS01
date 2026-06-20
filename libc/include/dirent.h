#ifndef _DIRENT_H
#define _DIRENT_H 1

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* Kernel dirent64 structure for getdents64 syscall */
#ifndef __LINUX_DIRENT64_DEFINED
#define __LINUX_DIRENT64_DEFINED
struct linux_dirent64 {
    uint64_t        d_ino;
    int64_t         d_off;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[];
};
#endif /* __LINUX_DIRENT64_DEFINED */

struct dirent {
    uint64_t d_ino;
    uint64_t d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char    d_name[256];
};

typedef struct {
    int fd;
    unsigned int buf_pos;
    unsigned int buf_end;
    uint64_t pos;
    char buf[4096];
    struct dirent result;
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif

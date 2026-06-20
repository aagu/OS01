#ifndef _UAPI_STAT_H
#define _UAPI_STAT_H

#include <stdint.h>

/* Linux x86_64 stat structure — must match userspace ABI exactly */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    uint64_t __unused[3];
};

/* File type bits */
#define S_IFMT   00170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000

/* Permission bits */
#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRWXO  00007

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)

/* Seek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* fcntl commands */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030

/* access modes */
#define F_OK  0
#define R_OK  4
#define W_OK  2
#define X_OK  1

/* ioctl codes */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCNOTTY   0x5422
#define TIOCGWINSZ  0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* Directory entry for getdents64 */
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

#define DT_UNKNOWN  0
#define DT_REG      8
#define DT_DIR      4
#define DT_CHR      2
#define DT_BLK      6

#endif /* _UAPI_STAT_H */

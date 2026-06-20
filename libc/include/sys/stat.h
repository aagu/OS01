#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux x86_64 stat structure (matches glibc layout) */
struct stat {
    uint64_t st_dev;         /* 0: device */
    uint64_t st_ino;         /* 8: inode */
    uint64_t st_nlink;       /* 16: number of hard links */
    uint32_t st_mode;        /* 24: file mode */
    uint32_t st_uid;         /* 28: user ID */
    uint32_t st_gid;         /* 32: group ID */
    uint32_t __pad0;         /* 36: padding */
    uint64_t st_rdev;        /* 40: device ID (if special file) */
    int64_t  st_size;        /* 48: total size in bytes */
    int64_t  st_blksize;     /* 56: blocksize for I/O */
    int64_t  st_blocks;      /* 64: number of 512B blocks allocated */
    uint64_t st_atime;       /* 72: time of last access */
    uint64_t st_atime_nsec;  /* 80: nanosecond of last access */
    uint64_t st_mtime;       /* 88: time of last modification */
    uint64_t st_mtime_nsec;  /* 96: nanosecond of last modification */
    uint64_t st_ctime;       /* 104: time of last status change */
    uint64_t st_ctime_nsec;  /* 112: nanosecond of last status change */
    uint64_t __unused[3];    /* 120-143: reserved */
};

/* File type macros */
#define S_IFMT   00170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

/* Test macros for file types */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Mask for regular file permissions */
#define S_IRWXUGO (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO   (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO   (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO   (S_IXUSR|S_IXGRP|S_IXOTH)

/* seek whence values */
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

/* fcntl flags */
#define FD_CLOEXEC  1
#define O_CLOEXEC   02000000

/* access modes */
#define F_OK  0
#define R_OK  4
#define W_OK  2
#define X_OK  1

/* dirent types */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

/* ioctl request codes */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCSPGRP   0x5410
#define TIOCGPGRP   0x540F

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* linux_dirent64 for getdents64 */
#ifndef __LINUX_DIRENT64_DEFINED
#define __LINUX_DIRENT64_DEFINED
struct linux_dirent64 {
    uint64_t        d_ino;       /* inode number */
    int64_t         d_off;       /* offset to next dirent */
    unsigned short  d_reclen;    /* length of this record */
    unsigned char   d_type;      /* file type */
    char            d_name[];    /* filename (null-terminated) */
};
#endif /* __LINUX_DIRENT64_DEFINED */

int    stat(const char *path, struct stat *buf);
int    lstat(const char *path, struct stat *buf);
int    fstat(int fd, struct stat *buf);
int64_t lseek(int fd, int64_t offset, int whence);
int    fcntl(int fd, int cmd, ...);
int    ioctl(int fd, unsigned long request, ...);
int    getdents64(int fd, struct linux_dirent64 *buf, unsigned int count);
int    access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */

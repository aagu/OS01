#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Filesystem stat (spec 2026-09-17 §6.4) ────────────────────────────────
 * Canonical struct stat / struct winsize / struct linux_dirent64 / AT_*
 * lookup flags / DT_* dirent types / S_IFMT+ / SEEK_* / F_* fcntl cmds /
 * F_OK access bits / TC* ioctl codes all live in kernel/include/uapi/stat.h
 * (spec §2.2 + §3.2 — single source). This header pulls them in once.
 *
 * libc-unique declarations kept here: S_IFSOCK / S_ISUID / S_ISGID /
 * S_ISVTX, per-group & per-other permission bits, FD_CLOEXEC, O_CLOEXEC,
 * S_IRWXUGO / S_IALLUGO / S_IRUGO / S_IWUGO / S_IXUGO masks, S_ISFIFO /
 * S_ISSOCK predicates, and the user-mode syscall prototypes (stat /
 * lstat / fstat / fstatat / lseek / fcntl / ioctl / getdents64 / access).
 */
#include <uapi/stat.h>

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

/* at-style lookup flags (used by *at() syscalls) and dirent types
 * (DT_*) live in the canonical kernel/include/uapi/stat.h — see
 * spec §2.2 + §3.2. libc keeps its own struct stat, struct winsize,
 * struct linux_dirent64, S_IFSOCK / S_ISUID / S_ISGID / S_ISVTX / per-
 * group & per-other permission bits / FD_CLOEXEC / O_CLOEXEC, and the
 * function declarations below. */
#include <uapi/stat.h>

/* ioctl request codes (libc keeps its own copies — uapi also defines a wider set) */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCSPGRP   0x5410
#define TIOCGPGRP   0x540F

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
int    fstatat(int dirfd, const char *path, struct stat *buf, int flags);
int64_t lseek(int fd, int64_t offset, int whence);
int    fcntl(int fd, int cmd, ...);
int    ioctl(int fd, unsigned long request, ...);
int    getdents64(int fd, struct linux_dirent64 *buf, unsigned int count);
int    access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */

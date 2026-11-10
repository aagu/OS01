/*
 * dirent.c — the directory stream: opendir/readdir/closedir plus the
 * descriptor-oriented entry points dirfd()/fdopendir() (T15).
 *
 * The stream reads the directory through the kernel's getdents64(2).
 * Every record the kernel returns is VALIDATED before it is copied out:
 * the record length must fit the bytes the kernel actually wrote, and the
 * name must be NUL-terminated inside the record.  A record that violates
 * either rule is a corrupted stream, not an end of directory: readdir()
 * answers NULL with errno == EIO instead of walking off the end of the
 * buffer.
 *
 * fd ownership:
 *   - opendir() opens the descriptor itself and owns it;
 *   - fdopendir() takes over a descriptor it was handed — on SUCCESS the
 *     descriptor belongs to the stream and closedir() closes it; on
 *     FAILURE the descriptor still belongs to the CALLER and is left
 *     open (POSIX).
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>

/* The largest name the kernel can hand back.  VFS_NAME_MAX is 256, i.e. a
 * name is at most 255 characters plus the terminating NUL, which is
 * exactly what struct dirent's d_name holds. */
#define DIRENT_NAME_MAX 256

static DIR *dir_alloc(int fd)
{
    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    dir->pos = 0;
    memset(dir->result.d_name, 0, sizeof(dir->result.d_name));
    dir->result.d_ino = 0;
    dir->result.d_off = 0;
    dir->result.d_reclen = 0;
    dir->result.d_type = DT_UNKNOWN;

    return dir;
}

/* Open a directory by name.  The descriptor is opened read-only; the
 * kernel's own answer for "not a directory" is ENOTDIR, and the check
 * below makes the answer ENOTDIR even on a kernel that would have let the
 * open() succeed (a regular file opens fine). */
DIR *opendir(const char *name)
{
    int fd;
    struct stat st;
    DIR *dir;

    if (!name) {
        errno = ENOENT;
        return NULL;
    }

    fd = open(name, O_RDONLY);
    if (fd < 0)
        return NULL;

    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return NULL;
    }

    dir = dir_alloc(fd);
    if (!dir) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    return dir;
}

/* Take over a descriptor.  On failure the descriptor is NOT closed: it
 * still belongs to the caller. */
DIR *fdopendir(int fd)
{
    struct stat st;
    DIR *dir;

    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    if (fstat(fd, &st) != 0) {
        int saved = errno;
        errno = saved;      /* the descriptor stays the caller's */
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;       /* the descriptor stays the caller's */
    }

    dir = dir_alloc(fd);
    if (!dir) {
        int saved = errno;
        errno = saved;      /* the descriptor stays the caller's */
        return NULL;
    }

    return dir;
}

/* The descriptor behind a stream, or -1/EBADF. */
int dirfd(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    return dirp->fd;
}

/* Read the next entry.  A negative return from getdents64 is an error and
 * sets errno; a zero return is the end of the directory and leaves errno
 * UNCHANGED (POSIX). */
struct dirent *readdir(DIR *dir)
{
    if (!dir) {
        errno = EBADF;
        return NULL;
    }

    if (dir->buf_pos >= dir->buf_end) {
        int64_t ret = (int64_t)syscall(SYS_getdents64, (uint64_t)dir->fd,
                                        (uint64_t)dir->buf,
                                        (uint64_t)sizeof(dir->buf));
        if (ret < 0) {
            errno = (int)(-ret);
            return NULL;
        }
        if (ret == 0)
            return NULL;            /* end of directory: errno untouched */
        dir->buf_end = (unsigned int)ret;
        dir->buf_pos = 0;
    }

    {
        const char *base = dir->buf + dir->buf_pos;
        size_t avail = dir->buf_end - dir->buf_pos;
        const struct linux_dirent64 *d;
        size_t name_room;
        size_t name_len;

        /* The header size is the OFFSET of d_name, not sizeof(): the
         * flexible array member starts right after d_type (offset 19),
         * while sizeof() of the struct is padded to 24.  The two
         * conventions differ: a Linux kernel writes
         * reclen = round8(19 + namelen + 1) while OS01's kernel writes
         * round8(sizeof + namelen + 1) — both put the name at offset 19,
         * so only the offset is safe to measure the name against. */
        const size_t header = offsetof(struct linux_dirent64, d_name);

        if (avail < header) {
            errno = EIO;            /* truncated header */
            return NULL;
        }
        d = (const struct linux_dirent64 *)(const void *)base;

        /* The record length must fit what the kernel actually wrote, and
         * a record must be able to hold its own header. */
        if (d->d_reclen < header || d->d_reclen > avail) {
            errno = EIO;
            return NULL;
        }

        /* The name must be NUL-terminated INSIDE the record. */
        name_room = d->d_reclen - header;
        if (name_room == 0 || memchr(d->d_name, '\0', name_room) == NULL) {
            errno = EIO;
            return NULL;
        }
        name_len = strlen(d->d_name);
        if (name_len >= DIRENT_NAME_MAX) {
            errno = EIO;            /* longer than the kernel can produce */
            return NULL;
        }

        memcpy(dir->result.d_name, d->d_name, name_len + 1);
        dir->result.d_ino = d->d_ino;
        /* d_off is the kernel's cookie, NOT a running entry counter. */
        dir->result.d_off = (uint64_t)d->d_off;
        dir->result.d_type = d->d_type;
        dir->result.d_reclen = d->d_reclen;

        dir->buf_pos += d->d_reclen;
        dir->pos++;

        return &dir->result;
    }
}

int closedir(DIR *dir)
{
    int fd;

    if (!dir) {
        errno = EBADF;
        return -1;
    }

    fd = dir->fd;
    dir->fd = -1;
    free(dir);
    return close(fd);
}

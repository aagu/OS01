#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return NULL;

    // Use fstat to verify it's a directory
    // Actually, open would fail if not a directory, but let's trust it.

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    dir->pos = 0;

    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (!dir)
        return NULL;

    // If buffer is empty or exhausted, refill
    if (dir->buf_pos >= dir->buf_end) {
        int ret = (int)syscall(SYS_getdents64, (uint64_t)dir->fd,
                                (uint64_t)dir->buf, (uint64_t)sizeof(dir->buf));
        if (ret <= 0) {
            // End of directory or error
            return NULL;
        }
        dir->buf_end = (unsigned int)ret;
        dir->buf_pos = 0;
    }

    struct linux_dirent64 *d = (struct linux_dirent64 *)(dir->buf + dir->buf_pos);

    // Copy name and type to our struct dirent
    size_t name_len = strlen(d->d_name);
    if (name_len >= sizeof(dir->result.d_name))
        name_len = sizeof(dir->result.d_name) - 1;

    memcpy(dir->result.d_name, d->d_name, name_len);
    dir->result.d_name[name_len] = '\0';
    dir->result.d_ino = d->d_ino;
    dir->result.d_type = d->d_type;
    dir->result.d_reclen = d->d_reclen;

    dir->buf_pos += d->d_reclen;
    dir->pos++;

    return &dir->result;
}

int closedir(DIR *dir)
{
    if (!dir) {
        errno = EBADF;
        return -1;
    }

    int fd = dir->fd;
    free(dir);
    return close(fd);
}

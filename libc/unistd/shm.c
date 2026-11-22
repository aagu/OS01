#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#if !defined(__is_libk)

#define SHM_PREFIX "/tmp/.shm_"
#define SHM_PREFIX_LEN 10
#define SHM_PATH_MAX 256

static int shm_build_path(const char *name, char *out, size_t out_sz)
{
    if (!name || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    const char *p = name;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        errno = EINVAL;
        return -1;
    }
    for (const char *q = p; *q; q++) {
        if (*q == '/') {
            errno = EINVAL;
            return -1;
        }
    }
    if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t nlen = strlen(p);
    if (SHM_PREFIX_LEN + nlen + 1 > out_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, SHM_PREFIX, SHM_PREFIX_LEN);
    memcpy(out + SHM_PREFIX_LEN, p, nlen + 1);
    return 0;
}

int shm_open(const char *name, int oflag, mode_t mode)
{
    char path[SHM_PATH_MAX];
    if (shm_build_path(name, path, sizeof(path)) < 0)
        return -1;

    int acc = oflag & 3;
    if (acc != O_RDONLY && acc != O_RDWR) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, oflag, (int)mode);
    if (fd >= 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int shm_unlink(const char *name)
{
    char path[SHM_PATH_MAX];
    if (shm_build_path(name, path, sizeof(path)) < 0)
        return -1;

    return unlink(path);
}

#endif /* !__is_libk */

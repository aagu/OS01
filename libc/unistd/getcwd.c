#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdlib.h>

char *getcwd(char *buf, size_t size)
{
#if defined(__is_libk)
    (void)buf; (void)size;
    return NULL;
#else
    int allocated = 0;
    if (buf == NULL) {
        // POSIX: allocate buffer for getcwd(NULL, 0)
        size = (size == 0) ? 256 : size;
        buf = malloc(size);
        if (!buf) { errno = ENOMEM; return NULL; }
        allocated = 1;
    }
    int64_t ret = syscall(SYS_getcwd, (uint64_t)buf, size, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        if (allocated) free(buf);
        return NULL;
    }
    return (char *)(uint64_t)ret;
#endif
}

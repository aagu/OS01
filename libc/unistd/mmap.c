#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
    int64_t ret = syscall6(SYS_mmap,
                           (uint64_t)addr, (uint64_t)length,
                           (uint64_t)prot, (uint64_t)flags,
                           (uint64_t)fd, (uint64_t)offset);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    int64_t ret = syscall(SYS_munmap, (uint64_t)addr,
                          (uint64_t)length, 0);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

int mprotect(void *addr, size_t length, int prot)
{
    int64_t ret = syscall(SYS_mprotect, (uint64_t)addr,
                          (uint64_t)length, (uint64_t)prot);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

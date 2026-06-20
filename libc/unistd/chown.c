#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

// Stub: chown not implemented
int chown(const char *path, uid_t owner, gid_t group)
{
    (void)path; (void)owner; (void)group;
    errno = ENOSYS;
    return -1;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd; (void)owner; (void)group;
    errno = ENOSYS;
    return -1;
}

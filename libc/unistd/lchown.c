#include <unistd.h>
#include <errno.h>
int lchown(const char *path, uid_t owner, gid_t group)
    { (void)path; (void)owner; (void)group; errno = ENOSYS; return -1; }

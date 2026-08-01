#include <unistd.h>
#include <errno.h>
int ttyname_r(int fd, char *buf, size_t buflen)
    { (void)fd; (void)buf; (void)buflen; errno = ENOSYS; return -1; }

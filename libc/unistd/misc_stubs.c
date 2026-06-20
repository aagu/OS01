#include <unistd.h>
#include <errno.h>

int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }
int chroot(const char *p) { (void)p; errno = ENOSYS; return -1; }
int ttyname_r(int fd, char *buf, size_t buflen) { (void)fd; (void)buf; (void)buflen; errno = ENOSYS; return -1; }
int settimeofday(const struct timeval *tv, const struct timezone *tz) { (void)tv; (void)tz; errno = ENOSYS; return -1; }

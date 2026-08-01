#include <unistd.h>
#include <errno.h>
int chroot(const char *p) { (void)p; errno = ENOSYS; return -1; }

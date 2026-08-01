#include <unistd.h>
#include <errno.h>
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }

#include <unistd.h>
#include <errno.h>
int setsid(void) { errno = ENOSYS; return -1; }

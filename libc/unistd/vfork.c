#include <unistd.h>
#include <errno.h>
int vfork(void) { errno = ENOSYS; return -1; }

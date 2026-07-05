#include <stdint.h>
#include <sys/syscall.h>
int64_t getpid(void) { return syscall(SYS_getpid, 0, 0, 0); }

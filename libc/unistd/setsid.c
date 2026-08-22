#include <unistd.h>
#include <sys/syscall.h>

int setsid(void) {
    return (int)syscall(SYS_setsid, 0, 0, 0);
}

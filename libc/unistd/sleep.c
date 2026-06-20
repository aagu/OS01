#include <stddef.h>
#include <sys/syscall.h>
unsigned int sleep(unsigned int s) {
    struct { unsigned long s; unsigned long ns; } ts = {s, 0};
    syscall(SYS_nanosleep, (unsigned long)&ts, 0, 0);
    return 0;
}

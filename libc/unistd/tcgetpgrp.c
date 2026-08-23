#include <unistd.h>
#include <sys/ioctl.h>

pid_t tcgetpgrp(int fd) {
    pid_t p = 0;
    if (ioctl(fd, TIOCGPGRP, &p) < 0) return -1;
    return p;
}

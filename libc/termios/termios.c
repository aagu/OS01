#include <sys/types.h>     /* pid_t (termios.h references it without including) */
#include <termios.h>       /* struct termios, B9600, function declarations */
#include <sys/ioctl.h>     /* TCGETS, TCSETS */
#include <sys/syscall.h>   /* syscall(), SYS_ioctl */
#include <errno.h>         /* errno */

int tcgetattr(int fd, struct termios *tio)
{
    int64_t ret = syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCGETS, (uint64_t)tio);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *tio)
{
    (void)actions;
    int64_t ret = syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCSETS, (uint64_t)tio);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int tcflow(int fd, int action)       { (void)fd; (void)action; return 0; }
int tcflush(int fd, int q)           { (void)fd; (void)q; return 0; }

speed_t cfgetispeed(const struct termios *tio) { (void)tio; return B9600; }
speed_t cfgetospeed(const struct termios *tio) { (void)tio; return B9600; }

int cfsetispeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }
int cfsetospeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }

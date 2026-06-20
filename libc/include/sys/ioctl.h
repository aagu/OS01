#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

/* Terminal ioctls */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCNOTTY   0x5422

#ifdef __cplusplus
}
#endif

#endif

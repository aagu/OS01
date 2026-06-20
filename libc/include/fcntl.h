#ifndef _FCNTL_H
#define _FCNTL_H 1

#include <sys/cdefs.h>

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_EXCL      0200
#define O_NONBLOCK  04000
#define O_NOCTTY    0400

#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC  1

#endif

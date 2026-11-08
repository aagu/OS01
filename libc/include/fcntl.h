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

#ifdef __cplusplus
extern "C" {
#endif

/* File-descriptor control.  F_GETFL/F_GETFD take no third argument;
 * F_SETFL/F_SETFD/F_DUPFD/F_DUPFD_CLOEXEC take an int.  Every other
 * command is rejected with EINVAL without a syscall.  Returns the
 * command's value, or -1 with errno set. */
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif

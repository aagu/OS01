#ifndef _POLL_H
#define _POLL_H 1
#include <sys/cdefs.h>
#include <stddef.h>
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
#define POLLIN  1
#define POLLOUT 2
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
#endif

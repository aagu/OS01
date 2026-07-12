#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#include <sys/syscall.h>

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

static inline int futex(int *uaddr, int op, int val)
{
    return (int)syscall(SYS_futex, (uint64_t)uaddr, (uint64_t)op, (uint64_t)val);
}

#endif

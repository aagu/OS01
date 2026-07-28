#ifndef _KERNEL_PTY_H
#define _KERNEL_PTY_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/file.h>
#include <kernel/arch/spinlock.h>

typedef int pid_t;  /* for termios.h userspace declarations */
#include <termios.h>

typedef struct pty_struct {
    pipe_t     *master_to_slave;   // master writes, slave reads
    pipe_t     *slave_to_master;   // slave writes, master reads
    int64_t     pgrp;              // foreground process group
    bool        allocated;         // slot in use
    struct termios term;           // terminal settings (for TCGETS/TCSETS)
} pty_t;

extern spinlock_T pty_lock;

// Forward declaration for ioctl dispatch (defined in pty.c, Task 7+)
int pty_slave_ioctl(pty_t *pty, int cmd, void *arg);

#endif // _KERNEL_PTY_H

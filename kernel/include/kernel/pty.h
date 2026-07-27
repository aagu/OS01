#ifndef _KERNEL_PTY_H
#define _KERNEL_PTY_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/file.h>
#include <kernel/arch/spinlock.h>

typedef struct pty_struct {
    pipe_t     *master_to_slave;   // master writes, slave reads
    pipe_t     *slave_to_master;   // slave writes, master reads
    int64_t     pgrp;              // foreground process group
    bool        allocated;         // slot in use
} pty_t;

extern spinlock_T pty_lock;

#endif // _KERNEL_PTY_H

#ifndef _KERNEL_PTY_H
#define _KERNEL_PTY_H

#define PTY_MAX  8

#include <stdint.h>
#include <stdbool.h>
#include <kernel/file.h>
#include <kernel/arch/spinlock.h>

typedef int pid_t;  /* for termios.h userspace declarations */
#include <termios.h>

typedef struct pty_struct {
    int         index;
    bool        allocated;
    pipe_t     *master_to_slave;   // master writes, slave reads
    pipe_t     *slave_to_master;   // slave writes, master reads
    struct termios term;
    uint16_t    ws_row, ws_col;
    pid_t       pgrp;              // foreground process group
} pty_t;

extern pty_t pty_table[PTY_MAX];
extern spinlock_T pty_lock;

// API
void pty_init(void);
pty_t *pty_alloc(void);

// Called by fd_ioctl (weak stub in file.c overridden by real impl in pty.c)
int pty_slave_ioctl(pty_t *pty, int cmd, void *arg);

#endif // _KERNEL_PTY_H

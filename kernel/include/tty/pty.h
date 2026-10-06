#ifndef _KERNEL_PTY_H
#define _KERNEL_PTY_H

#define PTY_MAX  8

#include <stdint.h>
#include <stdbool.h>
#include <fs/file.h>
#include <arch/spinlock.h>

typedef int pid_t;  /* for termios.h userspace declarations */
#include <termios.h>

typedef struct pty_struct {
    int         index;
    bool        allocated;
    pipe_t     *master_to_slave;   // master writes, slave reads
    pipe_t     *slave_to_master;   // slave writes, master reads
    spinlock_T  state_lock;
    struct termios term;
    uint16_t    ws_row, ws_col;
    uint16_t    ws_xpixel, ws_ypixel;
    union {
        pid_t   pgrp;              // foreground process group
        pid_t   fg_pgrp;
    };
} pty_t;

extern pty_t pty_table[PTY_MAX];
extern spinlock_T pty_lock;

// API
void pty_init(void);
pty_t *pty_alloc(void);

// Called by fd_ioctl (weak stub in file.c overridden by real impl in pty.c)
int pty_ioctl(pty_t *pty, int cmd, void *arg);
int pty_slave_ioctl(pty_t *pty, int cmd, void *arg);
int pty_master_ioctl(pty_t *pty, int cmd, void *arg);

#endif // _KERNEL_PTY_H

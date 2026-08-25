// kernel/driver/pty.c — PTY master/slave implementation
//
// Architecture:
//   /dev/ptmx → ptmx_open allocates a pty_t, registers /dev/pts{N},
//               returns FD_PTY_MASTER file_t.
//   /dev/pts{N} → ptsN_open returns FD_PTY_SLAVE file_t.
//
//   I/O goes through pipe_read_internal / pipe_write_internal via
//   fd_read / fd_write dispatch — no devfs read/write callbacks.
//   Poll goes through fd_poll dispatch.
//   ioctl goes through fd_ioctl → pty_slave_ioctl.

#include <kernel/pty.h>
#include <kernel/file.h>
#include <fs/devfs.h>
#include <kernel/uaccess.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel/task.h>

// ── Forward declarations for ops tables ─────────────────────────
static int ptmx_open(const char *name, file_t **out_file);
static int ptsN_open(const char *name, file_t **out_file);
static const struct devfs_ops pty_slave_ops;

// ═══════════════════════════════════════════════════════════════
//  PTY table
// ═══════════════════════════════════════════════════════════════

pty_t pty_table[PTY_MAX];

// ── pty_alloc: find a free slot and initialise ─────────────────
pty_t *pty_alloc(void)
{
    uint64_t fl = spin_lock_irqsave(&pty_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) {
            pty_t *pty = &pty_table[i];
            memset(pty, 0, sizeof(*pty));
            pty->index = i;
            pty->allocated = true;
            pty->master_to_slave = pipe_alloc();
            pty->slave_to_master = pipe_alloc();
            if (!pty->master_to_slave || !pty->slave_to_master) {
                if (pty->master_to_slave) {
                    pipe_free(pty->master_to_slave);
                    pty->master_to_slave = NULL;
                }
                if (pty->slave_to_master) {
                    pipe_free(pty->slave_to_master);
                    pty->slave_to_master = NULL;
                }
                pty->allocated = false;
                spin_unlock_irqrestore(&pty_lock, fl);
                return NULL;
            }
            // Default termios: raw mode
            pty->term.c_lflag = 0;
            pty->term.c_iflag = ICRNL;
            pty->term.c_oflag = OPOST | ONLCR;
            // c_cflag / c_line / c_ispeed / c_ospeed already zero from memset
            pty->term.c_cc[VMIN] = 1;
            pty->term.c_cc[VTIME] = 0;
            pty->ws_row = 25;
            pty->ws_col = 80;
            pty->pgrp = 0;
            spin_unlock_irqrestore(&pty_lock, fl);
            return pty;
        }
    }
    spin_unlock_irqrestore(&pty_lock, fl);
    return NULL;
}

// ═══════════════════════════════════════════════════════════════
//  pty_slave_ioctl — public, overrides the weak stub in file.c
// ═══════════════════════════════════════════════════════════════

int pty_slave_ioctl(pty_t *pty, int cmd, void *arg)
{
    if (!pty) return -ENODEV;
    switch (cmd) {
    case TCGETS: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct termios), true))
            return -EFAULT;
        if (copy_to_user_ft(arg, &pty->term, sizeof(struct termios)) < 0)
            return -EFAULT;
        return 0;
    }
    case TCSETS:
    case TCSETSW: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct termios), false))
            return -EFAULT;
        struct termios kterm;
        if (copy_from_user_ft(&kterm, arg, sizeof(kterm)) < 0)
            return -EFAULT;
        pty->term = kterm;
        return 0;
    }
    case TIOCGWINSZ: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct winsize), true))
            return -EFAULT;
        struct winsize kws = {0};
        kws.ws_row = pty->ws_row;
        kws.ws_col = pty->ws_col;
        if (copy_to_user_ft(arg, &kws, sizeof(kws)) < 0)
            return -EFAULT;
        return 0;
    }
    case TIOCSWINSZ: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct winsize), false))
            return -EFAULT;
        struct winsize kws;
        if (copy_from_user_ft(&kws, arg, sizeof(kws)) < 0)
            return -EFAULT;
        pty->ws_row = kws.ws_row;
        pty->ws_col = kws.ws_col;
        return 0;
    }
    case TIOCGPGRP: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(pid_t), true))
            return -EFAULT;
        pid_t kp = pty->pgrp;
        if (copy_to_user_ft(arg, &kp, sizeof(kp)) < 0)
            return -EFAULT;
        return 0;
    }
    case TIOCSPGRP: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(pid_t), false))
            return -EFAULT;
        pid_t new_pg;
        if (copy_from_user_ft(&new_pg, arg, sizeof(new_pg)) < 0)
            return -EFAULT;
        pty->pgrp = new_pg;
        return 0;
    }
    case TIOCSCTTY:
        // stub: ctty is set by ptsN_open
        return 0;
    // FIONREAD — busybox ash needs this for get_more_input()
    case FIONREAD: {
        if (!pty->master_to_slave) return -ENODEV;
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(int), true))
            return -EFAULT;
        uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
        int avail = (pty->master_to_slave->head - pty->master_to_slave->tail + PIPE_SIZE) % PIPE_SIZE;
        spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        if (copy_to_user_ft(arg, &avail, sizeof(avail)) < 0)
            return -EFAULT;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

// ═══════════════════════════════════════════════════════════════
//  ptmx_open: allocate PTY, register slave, return FD_PTY_MASTER
// ═══════════════════════════════════════════════════════════════

static int ptmx_open(const char *name, file_t **out_file)
{
    (void)name;
    pty_t *pty = pty_alloc();
    if (!pty) return -ENOMEM;

    // Register the slave device node: "/dev/pts{N}"
    char slave_name[16];
    slave_name[0] = 'p'; slave_name[1] = 't'; slave_name[2] = 's';
    slave_name[3] = '0' + pty->index;
    slave_name[4] = '\0';
    int rc = devfs_register_chrdev(slave_name, pty, &pty_slave_ops);
    if (rc < 0) {
        // Rollback: free pipes and release slot
        if (pty->master_to_slave) pipe_free(pty->master_to_slave);
        if (pty->slave_to_master) pipe_free(pty->slave_to_master);
        pty->master_to_slave = NULL;
        pty->slave_to_master = NULL;
        pty->allocated = false;
        return rc;
    }

    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = FD_PTY_MASTER;
    f->pty = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}

// ═══════════════════════════════════════════════════════════════
//  ptsN_open: return FD_PTY_SLAVE file_t, set controlling terminal
// ═══════════════════════════════════════════════════════════════

static int ptsN_open(const char *name, file_t **out_file)
{
    // Find matching PTY slot by checking name suffix "pts{N}"
    pty_t *pty = NULL;
    int name_len = (int)strlen(name);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) continue;
        // Build expected suffix "pts0" … "pts7"
        char expected[6];
        expected[0] = 'p'; expected[1] = 't'; expected[2] = 's';
        expected[3] = '0' + i;
        expected[4] = '\0';
        int elen = 4;
        if (name_len >= elen &&
            strcmp(name + name_len - elen, expected) == 0) {
            pty = &pty_table[i];
            break;
        }
    }
    if (!pty) return -ENOENT;

    // Set controlling terminal
    current->ctty_type = CTTY_PTY;
    current->ctty = pty;

    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = FD_PTY_SLAVE;
    f->pty = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}

// ── pty_slave_ops: only .open is needed (I/O goes through FD_PTY_SLAVE)
static const struct devfs_ops pty_slave_ops = {
    .open = ptsN_open,
};

// ── ptmx_ops: only .open is needed
static const struct devfs_ops ptmx_ops = {
    .open = ptmx_open,
};

// ═══════════════════════════════════════════════════════════════
//  pty_init: initialise table, register /dev/ptmx
// ═══════════════════════════════════════════════════════════════

void pty_init(void)
{
    memset(pty_table, 0, sizeof(pty_table));
    devfs_register_chrdev("ptmx", NULL, &ptmx_ops);
}

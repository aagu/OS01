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

#include <tty/pty.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <log/log.h>
#include <memory/uaccess.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sched/task.h>
#include <sys/ioctl.h>

#include <uapi/time.h>
#include <selftest/boot_failure_fixture.h>

// ── Forward declarations for ops tables ─────────────────────────
static int ptmx_open(const char *name, file_t **out_file);
static int ptsN_open(const char *name, file_t **out_file);
static const struct devfs_ops pty_slave_ops;

// ── The slave device name for a slot index: "pts0" … "pts7" ────
//
// The name IS the binding: "/dev/pts{N}" always refers to
// &pty_table[N].  The nodes are registered once (pty_init) and never
// re-created, because devfs has no unregister API.
static void pty_slave_name(char *buf, int index)
{
    buf[0] = 'p'; buf[1] = 't'; buf[2] = 's';
    buf[3] = '0' + index;
    buf[4] = '\0';
}

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
            spin_init(&pty->state_lock);
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
            pty->ws_xpixel = 0;
            pty->ws_ypixel = 0;
            pty->pgrp = 0;
            spin_unlock_irqrestore(&pty_lock, fl);
            return pty;
        }
    }
    spin_unlock_irqrestore(&pty_lock, fl);
    return NULL;
}

// ═══════════════════════════════════════════════════════════════
//  pty_ioctl — common ioctl handler for both master and slave
// ═══════════════════════════════════════════════════════════════

int pty_ioctl(pty_t *pty, int cmd, void *arg)
{
    if (!pty) return -ENODEV;
    switch (cmd) {
    case TIOCGWINSZ: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct winsize), true))
            return -EFAULT;
        struct winsize kws;
        uint64_t fl = spin_lock_irqsave(&pty->state_lock);
        kws.ws_row = pty->ws_row;
        kws.ws_col = pty->ws_col;
        kws.ws_xpixel = pty->ws_xpixel;
        kws.ws_ypixel = pty->ws_ypixel;
        spin_unlock_irqrestore(&pty->state_lock, fl);

        ssize_t user_copy_rc = copy_to_user_ft(arg, &kws, sizeof(kws));
        if (user_copy_rc < 0) return user_copy_rc;
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

        bool changed = false;
        pid_t pgrp = 0;
        uint64_t fl = spin_lock_irqsave(&pty->state_lock);
        if (pty->ws_row != kws.ws_row ||
            pty->ws_col != kws.ws_col ||
            pty->ws_xpixel != kws.ws_xpixel ||
            pty->ws_ypixel != kws.ws_ypixel) {
            changed = true;
            pty->ws_row = kws.ws_row;
            pty->ws_col = kws.ws_col;
            pty->ws_xpixel = kws.ws_xpixel;
            pty->ws_ypixel = kws.ws_ypixel;
        }
        pgrp = pty->pgrp;
        spin_unlock_irqrestore(&pty->state_lock, fl);

        // Notify foreground process group if dimensions changed and pgrp is valid
        // (pgrp > 0; pgrp == 0 indicates unassigned foreground group, no signal sent).
        if (changed && pgrp > 0) {
            signal_pgrp(pgrp, SIGWINCH);
        }
        return 0;
    }
    case TIOCGPGRP: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(pid_t), true))
            return -EFAULT;
        pid_t kp;
        uint64_t fl = spin_lock_irqsave(&pty->state_lock);
        kp = pty->pgrp;
        spin_unlock_irqrestore(&pty->state_lock, fl);

        ssize_t user_copy_rc = copy_to_user_ft(arg, &kp, sizeof(kp));
        if (user_copy_rc < 0) return user_copy_rc;
        return 0;
    }
    case TIOCSPGRP: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(pid_t), false))
            return -EFAULT;
        pid_t new_pg;
        if (copy_from_user_ft(&new_pg, arg, sizeof(new_pg)) < 0)
            return -EFAULT;

        uint64_t fl = spin_lock_irqsave(&pty->state_lock);
        pty->pgrp = new_pg;
        spin_unlock_irqrestore(&pty->state_lock, fl);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

// ═══════════════════════════════════════════════════════════════
//  pty_master_ioctl — public, called for FD_PTY_MASTER
// ═══════════════════════════════════════════════════════════════

// Share one locked snapshot path; faultable user copies stay outside the lock.
static int pty_get_termios(pty_t *pty, void *arg)
{
    if (!arg) return -EFAULT;
    if (!syscall_check_user_range((uint64_t)arg,
                                  sizeof(struct termios), true))
        return -EFAULT;
    struct termios kterm;
    uint64_t fl = spin_lock_irqsave(&pty->state_lock);
    memcpy(&kterm, &pty->term, sizeof(kterm));
    spin_unlock_irqrestore(&pty->state_lock, fl);
    ssize_t rc = copy_to_user_ft(arg, &kterm, sizeof(kterm));
    return rc < 0 ? (int)rc : 0;
}

int pty_master_ioctl(pty_t *pty, int cmd, void *arg)
{
    if (!pty) return -ENODEV;
    if (cmd == TCGETS)
        return pty_get_termios(pty, arg);
    return pty_ioctl(pty, cmd, arg);
}

// ═══════════════════════════════════════════════════════════════
//  pty_slave_ioctl — public, overrides the weak stub in file.c
// ═══════════════════════════════════════════════════════════════

int pty_slave_ioctl(pty_t *pty, int cmd, void *arg)
{
    if (!pty) return -ENODEV;
    switch (cmd) {
    case TIOCGWINSZ:
    case TIOCSWINSZ:
    case TIOCGPGRP:
    case TIOCSPGRP:
        return pty_ioctl(pty, cmd, arg);
    case TCGETS:
        return pty_get_termios(pty, arg);
    case TCSETS:
    case TCSETSW: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct termios), false))
            return -EFAULT;
        struct termios kterm;
        if (copy_from_user_ft(&kterm, arg, sizeof(kterm)) < 0)
            return -EFAULT;
        uint64_t fl = spin_lock_irqsave(&pty->state_lock);
        pty->term = kterm;
        spin_unlock_irqrestore(&pty->state_lock, fl);
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
        {
            ssize_t user_copy_rc = copy_to_user_ft(arg, &avail, sizeof(avail));
            if (user_copy_rc < 0) return user_copy_rc;
        }
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
    file_t *f = file_alloc();
    if (!f) return -ENOMEM;

    pty_t *pty = pty_alloc();
    if (!pty) {
        file_free(f);
        return -ENOMEM;
    }

    // The slave node "/dev/pts{N}" is already registered (pty_init) and
    // stays bound to &pty_table[N] for the lifetime of the system, so
    // this open does not touch devfs.  Re-registering it here would
    // fail with -EEXIST on the second master and permanently break
    // every later open("/dev/ptmx").

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
    file_t *f = file_alloc();
    if (!f) return -ENOMEM;

    // Find matching PTY slot by checking name suffix "pts{N}"
    pty_t *pty = NULL;
    int name_len = (int)strlen(name);

    // The scan and the "slave opened" marking happen under pty_lock:
    // a master released in parallel must not reclaim the pty (and its
    // pipes) between the two steps — the reclaim only fires while the
    // slave was never opened.
    uint64_t fl = spin_lock_irqsave(&pty_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) continue;
        // Build expected suffix "pts0" … "pts7"
        char expected[6];
        pty_slave_name(expected, i);
        int elen = 4;
        if (name_len >= elen &&
            strcmp(name + name_len - elen, expected) == 0) {
            pty = &pty_table[i];
            break;
        }
    }
    // Mark the slave end as opened: a master released without a slave
    // reclaims the pty slot (see the PTY branch of file_free()).
    if (pty) pty->slave_opened = true;
    spin_unlock_irqrestore(&pty_lock, fl);

    if (!pty) {
        file_free(f);
        return -ENOENT;
    }

    // Set controlling terminal
    current->ctty_type = CTTY_PTY;
    current->ctty = pty;

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
//  pty_init: initialise table, register /dev/ptmx and the slave nodes
// ═══════════════════════════════════════════════════════════════

int pty_init(void)
{
    if (boot3_test_fail_site("ptmx")) {
        return -ENOMEM;
    }

    memset(pty_table, 0, sizeof(pty_table));
    int rc = devfs_register_chrdev("ptmx", NULL, &ptmx_ops);
    if (rc != 0) {
        log_err("pty: failed to register /dev/ptmx (%d)\n", rc);
        return rc;
    }

    // The slave nodes are PERMANENT bindings: "/dev/pts{N}" always
    // refers to &pty_table[N], and the slot index never changes for a
    // given node.  They are registered ONCE, here, because devfs has no
    // unregister API: a node created on the first master open outlived
    // that PTY, so the next master open for the same index failed with
    // -EEXIST and every later open("/dev/ptmx") failed — a terminal that
    // restarted could never get its PTY back.
    //
    // Opening a slave whose slot is not allocated still fails with
    // -ENOENT (see ptsN_open), so the user-visible behaviour is
    // unchanged: only the node's lifetime changed.
    for (int i = 0; i < PTY_MAX; i++) {
        char slave_name[8];
        pty_slave_name(slave_name, i);
        rc = devfs_register_chrdev(slave_name, &pty_table[i],
                                   &pty_slave_ops);
        if (rc != 0) {
            log_err("pty: failed to register /dev/%s (%d)\n",
                    slave_name, rc);
            return rc;
        }
    }
    return 0;
}

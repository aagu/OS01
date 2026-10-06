/*
 * kernel/driver/fb_test.c
 *
 * Controlled framebuffer fault-injection surface (QEMU resolution switcher
 * Task 9).  This whole translation unit is compiled and linked ONLY when
 * FB_RESOLUTION_TEST=1; the production kernel never references it.
 *
 * It provides /dev/fbtest with per-open private state:
 *
 *   - ARM_MISMATCH / ARM_ROLLBACK_FAILURE arm a one-shot register-readback
 *     fault for (target_pid, token).  The next layout-changing SET for the
 *     matching PID consumes it; GET never consumes, and another PID's SET
 *     neither consumes nor faults.  The physical register programming in
 *     bga.c is untouched — only the verification readback is corrupted.
 *   - HOLD_WRITER acquires a real writer lease through the production
 *     fb_writer_begin()/fb_writer_end() API, so a SET observes a real drain
 *     timeout.  release_file auto-releases a still-held lease.
 *   - ARM_TERMINAL_ENOMEM arms a one-shot ENOMEM for the next terminal
 *     resize prepare of the target PID; CONSUME_TERMINAL_ENOMEM (issued by
 *     terminal_display.c) reads and clears it.
 *   - SNAPSHOT is a pure-output exception returning struct fb_test_snapshot
 *     (state + DISPI regs 0..10 + active writer count), sampled under the
 *     display mutex with the DISPI index restored.
 */

#include <driver/fb_test.h>
#include <driver/fb_state.h>
#include <driver/bga.h>
#include <uapi/fb_test.h>
#include <uapi/fb.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <memory/uaccess.h>
#include <memory/slab.h>
#include <arch/spinlock.h>
#include <sched/task.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* ── Armed-fault registry ── */
enum fb_test_reg_kind {
    FB_TEST_REG_MISMATCH = 0,
    FB_TEST_REG_ROLLBACK_FAILURE = 1,
};

typedef struct {
    bool     valid;
    int      kind;
    int32_t  target_pid;
    uint64_t token;
} fb_test_pending_t;

static spinlock_T g_test_lock;
static bool       g_test_lock_ready;

/* The x86 spinlock is inverted (0 = held); a BSS-zero lock must never be
 * used before spin_init.  fb_state.c reaches this module from
 * fb_bootstrap_state() and fb_mark_failed(), both of which can run before
 * fb_test_init() (the device-node registration site).  The very first call
 * happens on the BSP before SMP/userland, so a plain init-once flag is
 * race-free here. */
static void fb_test_lock_ensure(void)
{
    if (!g_test_lock_ready) {
        spin_init(&g_test_lock);
        g_test_lock_ready = true;
    }
}

static fb_test_pending_t g_pending_reg;
static fb_test_pending_t g_pending_term;

/* Active register transaction, valid only between fb_test_set_begin() and
 * fb_test_set_end().  Readback corruption is applied at most once per
 * phase. */
typedef struct {
    bool active;
    bool apply_fault;
    bool rollback_fault;
    bool apply_done;
    bool rollback_done;
} fb_test_txn_t;

static fb_test_txn_t g_txn;

/* ── Per-open private state ── */
typedef struct {
    int32_t    owner_pid;
    bool       hold_held;
    fb_lease_t hold_lease;
} fb_test_file_t;

/* ── Pending-state management ── */
void fb_test_reset(void)
{
    fb_test_lock_ensure();
    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    memset(&g_pending_reg, 0, sizeof(g_pending_reg));
    memset(&g_pending_term, 0, sizeof(g_pending_term));
    memset(&g_txn, 0, sizeof(g_txn));
    spin_unlock_irqrestore(&g_test_lock, flags);
}

void fb_test_clear_pending(void)
{
    fb_test_reset();
}

/* ── SET-transaction hooks (called from fb_state.c) ── */
void fb_test_set_begin(int32_t pid)
{
    fb_test_lock_ensure();
    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    g_txn.active = true;
    g_txn.apply_fault = false;
    g_txn.rollback_fault = false;
    g_txn.apply_done = false;
    g_txn.rollback_done = false;
    if (g_pending_reg.valid && g_pending_reg.target_pid == pid) {
        g_txn.apply_fault = true;
        g_txn.rollback_fault = (g_pending_reg.kind == FB_TEST_REG_ROLLBACK_FAILURE);
        g_pending_reg.valid = false;
    }
    spin_unlock_irqrestore(&g_test_lock, flags);
}

void fb_test_set_end(void)
{
    fb_test_lock_ensure();
    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    memset(&g_txn, 0, sizeof(g_txn));
    spin_unlock_irqrestore(&g_test_lock, flags);
}

/* ── Readback filter (called from bga.c verification) ── */
uint16_t fb_test_filter_readback(int step, uint16_t index, uint16_t value)
{
    fb_test_lock_ensure();
    uint16_t out = value;
    if (index != VBE_DISPI_INDEX_X_OFFSET) {
        return out;
    }

    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    if (g_txn.active) {
        if (step == FB_TEST_STEP_APPLY && g_txn.apply_fault && !g_txn.apply_done) {
            out = (uint16_t)(value ^ 0x0001u);
            g_txn.apply_done = true;
        } else if (step == FB_TEST_STEP_ROLLBACK && g_txn.rollback_fault &&
                   !g_txn.rollback_done) {
            out = (uint16_t)(value ^ 0x0001u);
            g_txn.rollback_done = true;
        }
    }
    spin_unlock_irqrestore(&g_test_lock, flags);
    return out;
}

/* ── Terminal-ENOMEM consumer (called from terminal_display.c) ── */
int fb_test_consume_terminal_enomem(int32_t pid)
{
    fb_test_lock_ensure();
    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    int rc = 0;
    if (g_pending_term.valid && g_pending_term.target_pid == pid) {
        g_pending_term.valid = false;
        rc = 1;
    }
    spin_unlock_irqrestore(&g_test_lock, flags);
    return rc;
}

/* ── Per-file lifecycle ── */
static int fb_test_open(const char *name, file_t **out_file)
{
    (void)name;
    if (!out_file) {
        return -EINVAL;
    }

    file_t *f = file_alloc();
    if (!f) {
        return -ENOMEM;
    }

    fb_test_file_t *st = (fb_test_file_t *)kmalloc(sizeof(*st));
    if (!st) {
        free(f);
        return -ENOMEM;
    }
    memset(st, 0, sizeof(*st));
    st->owner_pid = (int32_t)current->pid;

    f->dev_private = st;
    *out_file = f;
    return 0;
}

static void fb_test_release_file(file_t *f)
{
    if (!f || !f->dev_private) {
        return;
    }
    fb_test_file_t *st = (fb_test_file_t *)f->dev_private;
    if (st->hold_held) {
        fb_writer_end(&st->hold_lease);
        st->hold_held = false;
    }

    /* Drop any fault this file armed for its own PID, so a later process
     * that reuses the PID cannot consume a stale fault.  Faults armed for a
     * different target PID (e.g. the terminal-ENOMEM fault armed by the
     * helper for the terminal) are left in place. */
    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    if (g_pending_reg.valid && g_pending_reg.target_pid == st->owner_pid) {
        g_pending_reg.valid = false;
    }
    if (g_pending_term.valid && g_pending_term.target_pid == st->owner_pid) {
        g_pending_term.valid = false;
    }
    spin_unlock_irqrestore(&g_test_lock, flags);

    kfree(st);
    f->dev_private = NULL;
}

/* ── Request validation ── */
static bool fb_test_req_valid(const struct fb_test_req *req)
{
    return req->version == 1 &&
           req->reserved[0] == 0 && req->reserved[1] == 0 &&
           req->reserved64 == 0;
}

/* ── SNAPSHOT (pure output) ── */
static int fb_test_snapshot_to_user(void *arg)
{
    if (!arg) {
        return -EFAULT;
    }
    if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_test_snapshot), true)) {
        return -EFAULT;
    }

    struct fb_test_snapshot snap;
    memset(&snap, 0, sizeof(snap));

    fb_control_lock();
    int rc_regs = bga_sample_regs(snap.regs);
    fb_snapshot_t fs;
    int rc_state = fb_snapshot_read(&fs);
    snap.active_writers = (uint64_t)fb_active_writers_count();
    if (rc_state == 0) {
        snap.state = fs.state;
    }
    fb_control_unlock();

    if (rc_regs != 0) {
        return rc_regs;
    }
    if (rc_state < 0) {
        return rc_state;
    }

    snap.reserved = 0;
    if (copy_to_user_ft(arg, &snap, sizeof(snap)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int fb_test_arm_register(int cmd, const struct fb_test_req *req,
                                fb_test_file_t *st)
{
    if (req->token == 0) {
        return -EINVAL;
    }
    int32_t target = req->target_pid ? (int32_t)req->target_pid : st->owner_pid;

    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    if (g_pending_reg.valid) {
        spin_unlock_irqrestore(&g_test_lock, flags);
        return -EBUSY;
    }
    g_pending_reg.valid = true;
    g_pending_reg.kind = (cmd == FBIOTEST_ARM_MISMATCH)
                             ? FB_TEST_REG_MISMATCH
                             : FB_TEST_REG_ROLLBACK_FAILURE;
    g_pending_reg.target_pid = target;
    g_pending_reg.token = req->token;
    spin_unlock_irqrestore(&g_test_lock, flags);
    return 0;
}

static int fb_test_arm_terminal(const struct fb_test_req *req, fb_test_file_t *st)
{
    int32_t target = req->target_pid ? (int32_t)req->target_pid : st->owner_pid;

    uint64_t flags = spin_lock_irqsave(&g_test_lock);
    g_pending_term.valid = true;
    g_pending_term.kind = 0;
    g_pending_term.target_pid = target;
    g_pending_term.token = req->token;
    spin_unlock_irqrestore(&g_test_lock, flags);
    return 0;
}

static int fb_test_ioctl_file(file_t *f, int cmd, void *arg)
{
    if (!f || !f->dev_private) {
        return -EINVAL;
    }
    fb_test_file_t *st = (fb_test_file_t *)f->dev_private;

    if (cmd == FBIOTEST_SNAPSHOT) {
        return fb_test_snapshot_to_user(arg);
    }

    if (!arg) {
        return -EFAULT;
    }
    if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_test_req), false)) {
        return -EFAULT;
    }
    struct fb_test_req req;
    if (copy_from_user_ft(&req, arg, sizeof(req)) < 0) {
        return -EFAULT;
    }
    if (!fb_test_req_valid(&req)) {
        return -EINVAL;
    }

    switch (cmd) {
    case FBIOTEST_ARM_MISMATCH:
    case FBIOTEST_ARM_ROLLBACK_FAILURE:
        return fb_test_arm_register(cmd, &req, st);

    case FBIOTEST_HOLD_WRITER: {
        if (st->hold_held) {
            return -EBUSY;
        }
        fb_lease_t lease;
        int rc = fb_writer_begin(&lease, 0);
        if (rc < 0) {
            return rc;
        }
        st->hold_lease = lease;
        st->hold_held = true;
        return 0;
    }

    case FBIOTEST_RELEASE_WRITER:
        if (st->hold_held) {
            fb_writer_end(&st->hold_lease);
            st->hold_held = false;
        }
        return 0;

    case FBIOTEST_ARM_TERMINAL_ENOMEM:
        return fb_test_arm_terminal(&req, st);

    case FBIOTEST_CONSUME_TERMINAL_ENOMEM:
        /* Positive return (1) tells the caller to inject ENOMEM once. */
        return fb_test_consume_terminal_enomem(req.target_pid
                                                   ? (int32_t)req.target_pid
                                                   : st->owner_pid);

    default:
        return -ENOTTY;
    }
}

const struct devfs_ops fb_test_ops = {
    .open         = fb_test_open,
    .ioctl_file   = fb_test_ioctl_file,
    .release_file = fb_test_release_file,
};

/* ── Node registration ── */
int fb_test_init(void)
{
    static bool initialized = false;
    if (initialized) {
        return 0;
    }
    fb_test_lock_ensure();
    initialized = true;
    /* The x86 device-node registration site logs a failure; fb_test.c
     * stays free of logging so it host-links without the kernel log
     * backend. */
    return devfs_register_chrdev("fbtest", NULL, &fb_test_ops);
}

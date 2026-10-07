// Kernel selftest for VINTR → signal_pgrp delivery.
// Strategy: create a kernel_thread that sleeps in INTERRUPTIBLE state,
// demote its PF_KTHREAD so signal_pgrp delivers to it, set its pgrp and
// dev_tty->fg_pgrp to the same value, inject VINTR via tty_push_input,
// verify thread's signal bit has SIGINT and thread state moved to RUNNING.
//
// v4 fixes: no ASSERT (panics via hlt loop), no debug_test (doesn't exist),
// no enum task_state (state is volatile int64_t), returns void,
// serial_printk for diagnostics. Follows kernel/selftest/test_kthread_self_reap.c
// pattern EXACTLY: non-static void entry + explicit call from task_init()
// (NOT the SELFTEST() macro — its .selftest_table section is never consumed
// by selftest_run_all(), which only runs explicitly-registered tests).
//
// v5 fix E4/E5: registered via explicit extern + call from task_init() after
// scheduler_ok=1 (kernel_thread + schedule() work). get_dev_tty() is set by
// kernel/core/main.c (tty_set_dev_tty(console)) BEFORE task_init(), so
// get_dev_tty() is non-NULL here; if the boot sequence ever changes this test
// silently skips via the dev_tty==NULL branch.

#if defined(OS01_SELFTEST)

#include <sched/task.h>
#include <tty/tty.h>
#include <core/printk.h>
#include <arch/irq.h>

static volatile int vintr_seen = 0;
static volatile int vintr_thread_pid = 0;
static int vintr_thread_release;

static uint64_t vintr_thread_fn(uint64_t arg) {
    (void)arg;
    vintr_thread_pid = current->pid;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&task_list_lock);
        if ((current->signal & (1ULL << SIGINT)) ||
            __atomic_load_n(&vintr_thread_release, __ATOMIC_ACQUIRE)) {
            if (current->signal & (1ULL << SIGINT))
                __atomic_store_n(&vintr_seen, 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&task_list_lock, flags);
            break;
        }
        current->state = TASK_INTERRUPTIBLE;
        spin_unlock_irqrestore(&task_list_lock, flags);
        schedule();
    }
    while (!__atomic_load_n(&vintr_thread_release, __ATOMIC_ACQUIRE))
        schedule();
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    current->flags |= PF_KTHREAD;
    spin_unlock_irqrestore(&task_list_lock, flags);
    return 0;
}

// NOTE: non-static int (called from task_init via extern). NOT registered via
// SELFTEST() macro. Matches test_kthread_self_reap / test_fd_refcount.
// Returns 0 on PASS, nonzero on FAIL; the caller (task_init) reports it via
// selftest_record_late().
int test_tty_vintr(void)
{
    int rc = -1;
    serial_printk("[selftest] test_tty_vintr... ");
    vintr_seen = 0;
    vintr_thread_pid = 0;
    vintr_thread_release = 0;

    tty_t *dev_tty = get_dev_tty();
    if (!dev_tty) {
        serial_printk("FAIL: get_dev_tty returned NULL\n");
        return -1;
    }

    // 1. Spawn kernel thread via create_kthread() (returns task_t*;
    //    kernel_thread() returns only the pid — casting that to a pointer
    //    would #PF on first deref).
    task_t *t = create_kthread(vintr_thread_fn, 0, "vintr_test");
    if (!t) {
        serial_printk("FAIL: create_kthread returned NULL\n");
        return -1;
    }

    // 2. Spin until thread is in TASK_INTERRUPTIBLE (bounded)
    int spin1 = 0;
    while ((t->state != TASK_INTERRUPTIBLE || vintr_thread_pid == 0) && spin1++ < 10000) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }
    if (t->state != TASK_INTERRUPTIBLE || vintr_thread_pid == 0) {
        serial_printk("FAIL: thread did not become ready\n");
        goto out;
    }

    // 3. v3 D1 fix: kernel_thread creates PF_KTHREAD. signal_pgrp skips
    //    PF_KTHREAD per spec §3.3. Demote so this fixture is a valid target.

    // 4. Set t->pgrp = unique value, tty->fg_pgrp = same value
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    t->flags &= ~PF_KTHREAD;
    t->pgrp = t->pid;
    spin_unlock_irqrestore(&task_list_lock, flags);

    uint64_t f = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
    dev_tty->fg_pgrp = t->pid;
    spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, f);

    // 5. Inject VINTR char (0x03 = Ctrl-C) into tty
    tty_push_input(dev_tty, 0x03);

    // 6. Wait for thread to wake and ack (bounded)
    int spin2 = 0;
    while (!__atomic_load_n(&vintr_seen, __ATOMIC_ACQUIRE) && spin2++ < 10000) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }

    if (!__atomic_load_n(&vintr_seen, __ATOMIC_ACQUIRE)) {
        serial_printk("FAIL: thread did not receive SIGINT\n");
    } else {
        serial_printk("PASS\n");
        rc = 0;
    }

    // Cleanup
    uint64_t f2 = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
    dev_tty->fg_pgrp = 0;
    spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, f2);
out:;
    uint64_t cleanup_flags = spin_lock_irqsave(&task_list_lock);
    __atomic_store_n(&vintr_thread_release, 1, __ATOMIC_RELEASE);
    t->signal |= (1ULL << SIGINT);
    task_wake(t);
    spin_unlock_irqrestore(&task_list_lock, cleanup_flags);
    return rc;
}

#endif // OS01_SELFTEST

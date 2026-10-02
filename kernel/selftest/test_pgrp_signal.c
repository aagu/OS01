#if defined(OS01_SELFTEST)

#include <sched/task.h>
#include <core/printk.h>
#include <arch/irq.h>
#include <errno.h>

static volatile int pgrp_thread_ready = 0;
static volatile int pgrp_thread_pid = 0;
static int pgrp_thread_release;
static int pgrp_thread_restored;
static int pgrp_abort_go;

static uint64_t pgrp_thread_fn(uint64_t arg) {
    // Regression fixture holds the worker before its first sleep.
    if (arg)
        while (!__atomic_load_n(&pgrp_abort_go, __ATOMIC_ACQUIRE))
            schedule();
    pgrp_thread_pid = current->pid;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&task_list_lock);
        if ((current->signal & (1ULL << SIGUSR1)) ||
            __atomic_load_n(&pgrp_thread_release, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&task_list_lock, flags);
            break;
        }
        current->state = TASK_INTERRUPTIBLE;
        __atomic_store_n(&pgrp_thread_ready, 1, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&task_list_lock, flags);
        schedule();
    }
    // Remain alive while the harness examines task fields. Restore kernel
    // ownership before returning through do_exit's address-space cleanup.
    while (!__atomic_load_n(&pgrp_thread_release, __ATOMIC_ACQUIRE))
        schedule();
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    current->flags |= PF_KTHREAD;
    spin_unlock_irqrestore(&task_list_lock, flags);
    __atomic_store_n(&pgrp_thread_restored, 1, __ATOMIC_RELEASE);
    return 0;
}

static bool pgrp_wait_restored(void)
{
    for (int i = 0; i < 10000; ++i) {
        if (__atomic_load_n(&pgrp_thread_restored, __ATOMIC_ACQUIRE))
            return true;
        arch_local_irq_enable();
        for (volatile int delay = 0; delay < 1000; ++delay);
        arch_local_irq_disable();
    }
    return false;
}

// NOTE: non-static void (called from task_init via extern). NOT registered via
// the SELFTEST() macro — its .selftest_table section is never consumed by
// selftest_run_all(), which only runs explicitly-registered tests. Matches
// test_kthread_self_reap / test_fd_refcount / test_tty_vintr.
void test_pgrp_signal(void)
{
    serial_printk("[selftest] test_pgrp_signal: start\n");
    pgrp_thread_ready = 0;
    pgrp_thread_pid = 0;
    pgrp_thread_release = 0;
    pgrp_thread_restored = 0;

    // create_kthread() returns the task_t* (kernel_thread() only returns the
    // pid; casting that int to a pointer would #PF on first deref).
    task_t *t = create_kthread(pgrp_thread_fn, 0, "pgrp_test");
    if (!t) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: create_kthread returned NULL\n");
        return;
    }

    // Wait for thread to register pid and reach INTERRUPTIBLE
    int spin_count = 0;
    while ((!__atomic_load_n(&pgrp_thread_ready, __ATOMIC_ACQUIRE) || t->state != TASK_INTERRUPTIBLE) && spin_count++ < 10000) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }
    if (!__atomic_load_n(&pgrp_thread_ready, __ATOMIC_ACQUIRE) || t->state != TASK_INTERRUPTIBLE) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: thread did not become ready\n");
        goto out;
    }

    // v3 D1 fix: kernel_thread sets PF_KTHREAD; signal_pgrp skips it per
    // spec §3.3. Demote so this fixture becomes a valid signal target.

    // Make thread its own pgrp leader
    uint64_t f1 = spin_lock_irqsave(&task_list_lock);
    t->flags &= ~PF_KTHREAD;
    t->pgrp = t->pid;
    spin_unlock_irqrestore(&task_list_lock, f1);

    int prev_signal = (int)(t->signal & (1ULL << SIGUSR1));
    int64_t prev_state = t->state;  // v4 fix E3: volatile int64_t, not enum

    // Assertion 1: signal_pgrp(0, ...) is silent no-op (returns 0)
    if (signal_pgrp(0, SIGUSR1) != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(0,..) != 0\n");
        goto out;
    }

    // Assertion 2: signal_pgrp with no matching pgrp returns -ESRCH
    if (signal_pgrp(99999, SIGUSR1) != -ESRCH) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(99999,..) != -ESRCH\n");
        goto out;
    }

    // Assertion 3: signal_pgrp(self.pid, SIGUSR1) hits the target
    if (signal_pgrp(t->pid, SIGUSR1) != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(target,..) != 0\n");
        goto out;
    }

    // Assertion 4: SIGUSR1 bit set on target's signal field
    if ((t->signal & (1ULL << SIGUSR1)) == 0 || prev_signal != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: SIGUSR1 bit not set (prev=%d cur=%llx)\n",
                      prev_signal, (unsigned long long)t->signal);
        goto out;
    }

    // Assertion 5: target was TASK_INTERRUPTIBLE → moved to TASK_RUNNING
    if (prev_state != TASK_INTERRUPTIBLE || t->state != TASK_RUNNING) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: state not moved (prev=%lld cur=%lld)\n",
                      (long long)prev_state, (long long)t->state);
        goto out;
    }

    serial_printk("[selftest] test_pgrp_signal: PASS\n");
out:;
    // Wake failed fixtures too, then let the worker restore PF_KTHREAD.
    uint64_t cleanup_flags = spin_lock_irqsave(&task_list_lock);
    __atomic_store_n(&pgrp_thread_release, 1, __ATOMIC_RELEASE);
    t->signal |= (1ULL << SIGUSR1);
    task_wake(t);
    spin_unlock_irqrestore(&task_list_lock, cleanup_flags);
    if (!pgrp_wait_restored()) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: cleanup timeout\n");
        return;
    }

    // Release before the worker has ever published TASK_INTERRUPTIBLE.
    pgrp_thread_restored = 0;
    pgrp_abort_go = 0;
    pgrp_thread_release = 0;
    t = create_kthread(pgrp_thread_fn, 1, "pgrp_abort_test");
    if (!t) {
        serial_printk("[selftest] pgrp early cleanup: FAIL: create\n");
        return;
    }
    cleanup_flags = spin_lock_irqsave(&task_list_lock);
    __atomic_store_n(&pgrp_thread_release, 1, __ATOMIC_RELEASE);
    t->signal |= (1ULL << SIGUSR1);
    task_wake(t);
    spin_unlock_irqrestore(&task_list_lock, cleanup_flags);
    __atomic_store_n(&pgrp_abort_go, 1, __ATOMIC_RELEASE);
    serial_printk("[selftest] pgrp early cleanup: %s\n",
                  pgrp_wait_restored() ? "PASS" : "FAIL: worker slept after release");
}

#endif // OS01_SELFTEST

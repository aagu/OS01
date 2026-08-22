#if defined(OS01_SELFTEST)

#include <kernel/task.h>
#include <kernel/printk.h>
#include <kernel/selftest.h>
#include <kernel/sched.h>

static volatile int pgrp_thread_ready = 0;
static volatile int pgrp_thread_pid = 0;

static int pgrp_thread_fn(void *arg) {
    (void)arg;
    pgrp_thread_pid = current->pid;
    for (;;) {
        current->state = TASK_INTERRUPTIBLE;
        __sync_synchronize();
        pgrp_thread_ready = 1;
        schedule();
        if (current->signal & (1ULL << SIGUSR1)) break;
    }
    return 0;
}

static int test_pgrp_signal(void)
{
    serial_printk("[selftest] test_pgrp_signal: start\n");
    pgrp_thread_ready = 0;
    pgrp_thread_pid = 0;

    task_t *t = (task_t *)kernel_thread(pgrp_thread_fn, NULL, "pgrp_test");
    if (!t) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: kernel_thread returned NULL\n");
        return -1;
    }

    // Wait for thread to register pid and reach INTERRUPTIBLE
    int spin_count = 0;
    while ((!pgrp_thread_ready || t->state != TASK_INTERRUPTIBLE) && spin_count++ < 10000) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }
    if (!pgrp_thread_ready || t->state != TASK_INTERRUPTIBLE) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: thread did not become ready\n");
        return -1;
    }

    // v3 D1 fix: kernel_thread sets PF_KTHREAD; signal_pgrp skips it per
    // spec §3.3. Demote so this fixture becomes a valid signal target.
    t->flags &= ~PF_KTHREAD;

    // Make thread its own pgrp leader
    uint64_t f1 = spin_lock_irqsave(&task_list_lock);
    t->pgrp = t->pid;
    spin_unlock_irqrestore(&task_list_lock, f1);

    int prev_signal = (int)(t->signal & (1ULL << SIGUSR1));
    int64_t prev_state = t->state;  // v4 fix E3: volatile int64_t, not enum

    // Assertion 1: signal_pgrp(0, ...) is silent no-op (returns 0)
    if (signal_pgrp(0, SIGUSR1) != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(0,..) != 0\n");
        return -1;
    }

    // Assertion 2: signal_pgrp with no matching pgrp returns -ESRCH
    if (signal_pgrp(99999, SIGUSR1) != -ESRCH) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(99999,..) != -ESRCH\n");
        return -1;
    }

    // Assertion 3: signal_pgrp(self.pid, SIGUSR1) hits the target
    if (signal_pgrp(t->pid, SIGUSR1) != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: signal_pgrp(target,..) != 0\n");
        return -1;
    }

    // Assertion 4: SIGUSR1 bit set on target's signal field
    if ((t->signal & (1ULL << SIGUSR1)) == 0 || prev_signal != 0) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: SIGUSR1 bit not set (prev=%d cur=%llx)\n",
                      prev_signal, (unsigned long long)t->signal);
        return -1;
    }

    // Assertion 5: target was TASK_INTERRUPTIBLE → moved to TASK_RUNNING
    if (prev_state != TASK_INTERRUPTIBLE || t->state != TASK_RUNNING) {
        serial_printk("[selftest] test_pgrp_signal: FAIL: state not moved (prev=%lld cur=%lld)\n",
                      (long long)prev_state, (long long)t->state);
        return -1;
    }

    // Cleanup: SIGKILL so thread exits deterministically
    t->signal |= (1ULL << SIGKILL);

    serial_printk("[selftest] test_pgrp_signal: PASS\n");
    return 0;
}

// v4 fix E4: register via SELFTEST macro (NOT a tests[] array or TEST_NAME)
SELFTEST(pgrp_signal);

#endif // OS01_SELFTEST

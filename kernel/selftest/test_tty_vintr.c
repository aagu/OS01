// VINTR -> signal_pgrp -> interruptible worker wakeup.
// Runs from task_init's BSP idle task. A runnable worker that merely calls
// schedule() while waiting for this observer will starve idle on its CPU.
// Workers must block while waiting for release; yielding is not sleeping.
#if defined(OS01_SELFTEST)

#include <sched/task.h>
#include <tty/tty.h>
#include <core/printk.h>
#include <arch/irq.h>
#include <percpu/percpu.h>
#include <time/timer.h>
#include <stddef.h>

static int vintr_ready;
static int vintr_seen;
static int vintr_thread_release;
static int vintr_restored;

static uint64_t vintr_thread_fn(uint64_t arg)
{
    (void)arg;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&task_list_lock);
        if (__atomic_load_n(&vintr_thread_release, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&task_list_lock, flags);
            break;
        }
        if (current->signal & (1ULL << SIGINT)) {
            // Check the wake transition here: on another CPU the observer
            // may only see our subsequent release-wait sleep state.
            __atomic_store_n(&vintr_seen,
                             current->state == TASK_RUNNING ? 1 : -1,
                             __ATOMIC_RELEASE);
            current->state = TASK_UNINTERRUPTIBLE;
        } else {
            current->state = TASK_INTERRUPTIBLE;
            __atomic_store_n(&vintr_ready, 1, __ATOMIC_RELEASE);
        }
        // Release and sleep publication share task_list_lock with cleanup,
        // including the case where cleanup precedes our first sleep.
        spin_unlock_irqrestore(&task_list_lock, flags);
        schedule();
    }
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    current->flags |= PF_KTHREAD;
    spin_unlock_irqrestore(&task_list_lock, flags);
    __atomic_store_n(&vintr_restored, 1, __ATOMIC_RELEASE);
    return 0;
}

// Idle cannot use a blocking wait. Give local workers CPU time, and let
// timer IRQs advance the deadline after schedule() returns with IRQs off.
static bool vintr_wait(const int *event)
{
    uint64_t start = jiffies;
    while (!__atomic_load_n(event, __ATOMIC_ACQUIRE) && jiffies - start < 100) {
        arch_local_irq_enable();
        schedule();
        arch_local_irq_enable();
    }
    arch_local_irq_disable();
    return __atomic_load_n(event, __ATOMIC_ACQUIRE) != 0;
}

static void vintr_dump(task_t *t, const char *phase)
{
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    serial_printk("[selftest] tty_vintr %s: cpu=%u state=%lld on_cpu=%u "
                  "on_rq=%d schedules=%lu\n", phase, t->cpu,
                  (long long)t->state, (unsigned)t->on_cpu, (int)t->on_rq,
                  (unsigned long)percpu_data[t->cpu].schedule_count);
    spin_unlock_irqrestore(&task_list_lock, flags);
}

static int test_tty_vintr_on_cpu(unsigned cpu, bool early_release)
{
    int rc = -1;
    uint64_t irq_flags = arch_local_irq_save();
    vintr_ready = vintr_seen = vintr_thread_release = vintr_restored = 0;
    tty_t *tty = get_dev_tty();
    if (!tty) {
        serial_printk("[selftest] tty_vintr: FAIL: no console TTY\n");
        arch_local_irq_restore(irq_flags);
        return -1;
    }

    // Pin before publication so the same-CPU regression is deterministic,
    // even with SMP load balancing. The worker stays alive until release.
    int pid = kernel_thread_on_cpu(vintr_thread_fn, 0, cpu);
    task_t *t = NULL;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *candidate = (task_t *)((char *)pos - offsetof(task_t, list));
        if (candidate->pid == pid) {
            t = candidate;
            break;
        }
        pos = task_list_next(pos);
    }
    spin_unlock_irqrestore(&task_list_lock, flags);
    if (!t) {
        serial_printk("[selftest] tty_vintr: FAIL: create worker pid=%d\n", pid);
        arch_local_irq_restore(irq_flags);
        return -1;
    }
    if (early_release) {
        rc = 0;
        goto out;
    }
    if (!vintr_wait(&vintr_ready)) {
        vintr_dump(t, "ready timeout");
        goto out;
    }

    flags = spin_lock_irqsave(&task_list_lock);
    bool sleeping = t->state == TASK_INTERRUPTIBLE;
    t->flags &= ~PF_KTHREAD;  // signal_pgrp skips kernel threads
    t->pgrp = t->pid;
    spin_unlock_irqrestore(&task_list_lock, flags);
    if (!sleeping) {
        vintr_dump(t, "not interruptible");
        goto out;
    }

    flags = spin_lock_irqsave(&tty->fg_pgrp_lock);
    pid_t saved_pgrp = tty->fg_pgrp;
    tty->fg_pgrp = pid;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, flags);
    tty_push_input(tty, 0x03);

    // Regression: force the worker to run before the BSP idle observer
    // releases it. A RUNNING release-wait loop would never return to idle.
    schedule();
    if (vintr_wait(&vintr_seen) &&
        __atomic_load_n(&vintr_seen, __ATOMIC_ACQUIRE) == 1)
        rc = 0;
    else
        vintr_dump(t, "signal/wakeup failed");

    flags = spin_lock_irqsave(&tty->fg_pgrp_lock);
    tty->fg_pgrp = saved_pgrp;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, flags);
out:
    flags = spin_lock_irqsave(&task_list_lock);
    __atomic_store_n(&vintr_thread_release, 1, __ATOMIC_RELEASE);
    task_wake(t);
    spin_unlock_irqrestore(&task_list_lock, flags);
    // Do not reuse fixture globals until the worker has stopped using them
    // and restored kernel ownership before do_exit's address-space cleanup.
    if (!vintr_wait(&vintr_restored)) {
        // The worker may finish and self-reap just after the deadline.
        serial_printk("[selftest] tty_vintr cleanup timeout: pid=%d\n", pid);
        rc = -1;
    }
    arch_local_irq_restore(irq_flags);
    return rc;
}

// Explicit late-test registration in task_init(), after scheduler_ok=1.
int test_tty_vintr(void)
{
    unsigned local = cpu_id();
    serial_printk("[selftest] test_tty_vintr... ");
    if (test_tty_vintr_on_cpu(local, false) != 0)
        return -1;
    serial_printk("same-CPU PASS; ");
    if (num_cpus > 1) {
        if (test_tty_vintr_on_cpu((local + 1) % num_cpus, false) != 0)
            return -1;
        serial_printk("cross-CPU PASS; ");
    }
    if (test_tty_vintr_on_cpu(local, true) != 0)
        return -1;
    serial_printk("early cleanup PASS\n");
    return 0;
}

#endif // OS01_SELFTEST

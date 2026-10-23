#include <sync/mutex.h>
#include <core/printk.h>
#include <sched/task.h>

#include <arch/irq.h>
#include <time/timer.h>

static mutex_t test_mtx;
static int shared_counter;
static int workers_done;

static uint64_t mutex_test_thread(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&test_mtx);
        shared_counter++;
        // Force contention even on one CPU: the peer must wait while this
        // owner yields, then wake and retry after unlock.
        if ((i & 31) == 0) {
            schedule();
            arch_local_irq_enable();
        }
        mutex_unlock(&test_mtx);
    }
    __atomic_add_fetch(&workers_done, 1, __ATOMIC_RELEASE);
    return 0;
}

int test_kernel_mutex(void)
{
    mutex_init(&test_mtx);
    shared_counter = 0;
    workers_done = 0;

    // Do not look up ephemeral task pointers: a fast worker can exit and
    // self-reap before create_kthread's post-publication lookup.
    int t1 = kernel_thread(mutex_test_thread, 0, PF_KTHREAD);
    int t2 = kernel_thread(mutex_test_thread, 0, PF_KTHREAD);

    if (t1 < 0 || t2 < 0) {
        serial_printk("[selftest] kernel mutex: FAIL (kthread create)\n");
        return -1;
    }

    uint64_t start = jiffies;
    uint64_t irq_flags = arch_local_irq_save();
    while (__atomic_load_n(&workers_done, __ATOMIC_ACQUIRE) < 2 &&
           jiffies - start < 500) {
        arch_local_irq_enable();
        schedule();
        arch_local_irq_enable();
    }
    arch_local_irq_restore(irq_flags);

    // Only read the protected counter after both final unlocks. An acquire
    // of the workers' release sequence publishes their completed writes.
    if (__atomic_load_n(&workers_done, __ATOMIC_ACQUIRE) == 2 &&
        shared_counter == 2000) {
        serial_printk("[selftest] kernel mutex: PASS (counter=%d)\n",
                      shared_counter);
        return 0;
    }
    serial_printk("[selftest] kernel mutex: FAIL (done=%d owner=%lld elapsed=%lu)\n",
                  __atomic_load_n(&workers_done, __ATOMIC_ACQUIRE),
                  (long long)__atomic_load_n(&test_mtx.owner, __ATOMIC_RELAXED),
                  (unsigned long)(jiffies - start));
    return -1;
}

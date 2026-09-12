#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <kernel/task.h>

static mutex_t test_mtx;
static volatile int shared_counter = 0;

static uint64_t mutex_test_thread(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&test_mtx);
        shared_counter++;
        mutex_unlock(&test_mtx);
    }
    return 0;
}

void test_kernel_mutex(void)
{
    mutex_init(&test_mtx);
    shared_counter = 0;

    task_t *t1 = create_kthread(mutex_test_thread, 0, "mutex-test-1");
    task_t *t2 = create_kthread(mutex_test_thread, 0, "mutex-test-2");

    if (!t1 || !t2) {
        serial_printk("[selftest] kernel mutex: FAIL (kthread create)\n");
        return;
    }

    int spins = 0;
    while (shared_counter < 2000 && spins < 1000000) {
        schedule();
        spins++;
    }

    if (shared_counter == 2000)
        serial_printk("[selftest] kernel mutex: PASS (counter=%d)\n",
                      shared_counter);
    else
        serial_printk("[selftest] kernel mutex: FAIL (counter=%d)\n",
                      shared_counter);
}

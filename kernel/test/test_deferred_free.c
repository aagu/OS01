#if defined(OS01_SELFTEST)

#include <kernel/deferred_free.h>
#include <kernel/slab.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/pmm.h>

// ── Test 1: Basic deferred kfree ──────────────────────────────
// Allocate N items, defer-free them, verify the slab total_free
// returns to baseline after the reaper drains the queue.

static int test_deferred_free_basic(void)
{
    // baseline: free count in the 64-byte slab cache (index 1)
    struct Slab_Cache *sc = &kmalloc_cache_size[1]; // size=64
    uint64_t baseline_free = sc->total_free;

    // Allocate 4 items
    void *ptrs[4];
    for (int i = 0; i < 4; i++) {
        ptrs[i] = kmalloc(64);
        if (!ptrs[i]) {
            serial_printk("[selftest] deferred_free_basic: "
                          "kmalloc(64) #%d failed\n", i);
            return -1;
        }
    }

    // Defer-free them
    for (int i = 0; i < 4; i++)
        deferred_kfree(ptrs[i]);

    // The reaper should drain them within a few schedule() ticks.
    // Spin for up to 100 schedule() calls, checking total_free.
    int spins = 0;
    while (sc->total_free < baseline_free && spins < 100) {
        schedule();
        spins++;
    }

    if (sc->total_free < baseline_free) {
        serial_printk("[selftest] deferred_free_basic: "
                      "total_free=%lu < baseline=%lu after %d spins\n",
                      (unsigned long)sc->total_free,
                      (unsigned long)baseline_free, spins);
        return -1;
    }

    return 0;
}

// ── Test 2: Kernel thread exit — stack reclamation ───────────
// A kthread allocates stack via create_kthread → do_exit, then
// the reaper frees stack_alloc_base.  Verify PMM free page count
// before and after doesn't drop catastrophically.

static uint64_t test_kthread_exiter(uint64_t arg)
{
    (void)arg;
    do_exit(0);
    return 0; // unreachable
}

static int test_deferred_free_kthread(void)
{
    extern struct Physical_Memory_Manager PMMngr;
    uint64_t pages_before = PMMngr.zones_struct->page_free_count;

    for (int i = 0; i < 3; i++) {
        task_t *kt = create_kthread(test_kthread_exiter, 0,
                                    "selftest-exiter");
        if (!kt) {
            serial_printk("[selftest] deferred_free_kthread: "
                          "create_kthread failed\n");
            return -1;
        }
        // Let the kthread run to completion (it calls do_exit → ZOMBIE)
        int spins = 0;
        while (kt->state == TASK_RUNNING && spins < 1000) {
            schedule();
            spins++;
        }
        // Let the zombie reaper process it, then the reaper drain
        for (int j = 0; j < 20; j++)
            schedule();
    }

    // After all 3 kthreads are reaped, pages should be near baseline.
    uint64_t pages_after = PMMngr.zones_struct->page_free_count;
    if (pages_after + 4 < pages_before) {
        serial_printk("[selftest] deferred_free_kthread: "
                      "pages_before=%lu pages_after=%lu (leak > 4 pages)\n",
                      (unsigned long)pages_before,
                      (unsigned long)pages_after);
        return -1;
    }

    return 0;
}

// ── Entry point (called from task_init()) ─────────────────────

void test_deferred_free(void)
{
    int ok = 0, fail = 0;

    serial_printk("[selftest] deferred_free_basic... ");
    if (test_deferred_free_basic() == 0) { ok++; serial_printk("PASS\n"); }
    else { fail++; serial_printk("FAIL\n"); }

    serial_printk("[selftest] deferred_free_kthread... ");
    if (test_deferred_free_kthread() == 0) { ok++; serial_printk("PASS\n"); }
    else { fail++; serial_printk("FAIL\n"); }

    serial_printk("[selftest] deferred_free: %d passed, %d failed\n",
                  ok, fail);
}

#endif // OS01_SELFTEST

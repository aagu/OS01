#if defined(OS01_SELFTEST)

#include <sched/task.h>
#include <memory/slab.h>      // kmalloc_cache_size[] — assert object-level reclaim
#include <time/timer.h>     // jiffies — time-bounded wait (cross-CPU safe)
#include <arch/irq.h>  // arch_local_irq_enable — schedule() returns with IRQs off
#include <arch/atomic.h>   // cross-CPU "exiter actually ran" progress counter
#include <core/printk.h>

// Bumped by each exiter right before do_exit so the wait loop can tell
// "kthread never got scheduled" (slow vCPU under TCG) apart from "ran
// but its stack was not reclaimed" (a real self-reap bug).
static volatile uint64_t exiter_ran;

// kthread that exits immediately. Its thread/fpu_save/stack must be
// reclaimed by __switch_to's PF_SELF_REAP epilogue, NOT by a reaper.
static uint64_t self_reap_exiter(uint64_t arg)
{
    (void)arg;
    arch_atomic_inc(&exiter_ran);
    do_exit(0);
    return 0; // unreachable
}

// Spawn N exiting kthreads, yield until their 64KB stacks are reclaimed,
// then assert the slab object count returned to baseline.
//
// A kthread's stack_alloc_base is malloc(sizeof(task_union)+STACK_SIZE)
// == 65536 bytes → the 65536-byte slab cache (index 11). total_using is
// the object-level "in use" counter: +1 per kmalloc, -1 per kfree. It
// returns to baseline only if every stack was actually kfree'd, and it is
// immune to slab page retention. Do NOT assert on PMMngr page_free_count:
// slab.c:260 only returns a slab's 2MB page to the PMM when using_count==0
// && total_free >= color_count*3/2, so pages may legitimately stay cached
// even though every stack object was freed → a page-count assertion would
// false-fail.
//
// The wait MUST be time-bounded (jiffies), not bounded by a schedule()
// count: sched_pick_cpu() may place the 8 kthreads on OTHER CPUs, and
// this selftest runs on the boot CPU inside task_init (before the idle
// loop). The boot CPU's schedule() never runs another CPU's rq, and its
// schedule() calls mostly hit the idle preemption guard and return in
// microseconds — so a fixed number of schedule() spins would exhaust in
// ~100µs while the other CPU is still in hlt waiting for a 10ms tick, and
// total_using would never drop. A jiffies budget lets the other CPUs'
// ticks actually schedule and reap them.
static int test_kthread_self_reap_once(void)
{
    struct Slab_Cache *sc = &kmalloc_cache_size[11]; // 65536-byte cache
    uint64_t using_before = sc->total_using;

    for (int i = 0; i < 8; i++) {
        int pid = kernel_thread(self_reap_exiter, 0, PF_KTHREAD);
        if (pid < 0) {
            serial_printk("[selftest] kthread_self_reap: "
                          "kernel_thread #%d failed\n", i);
            return -1;
        }
    }

    // Yield until every exiter has actually run (exiter_ran == 8) AND its
    // 64KB stack was reclaimed by the __switch_to PF_SELF_REAP epilogue
    // (each __switch_to kfree's the stack on the way out → total_using--).
    // total_using is read locklessly: another CPU's kfree bumps it under
    // slab_lock, but on x86_64 an aligned u64 read is atomic and this is a
    // benign progress check, not a correctness dependency (the final
    // comparison is also just a leak detector).
    //
    // The budget is 500 jiffies (5s at 100 Hz): under QEMU TCG with
    // -smp 8 (no KVM in CI) the APs' scheduling rounds are throttled by
    // host CPU contention while user-space init boots concurrently, and a
    // 1s window was observed to cut some AP exiters off before they had
    // run even once (CI: "leaked stacks: using_before=8 using_after=10").
    uint64_t start = jiffies;
    while ((arch_atomic_read(&exiter_ran) < 8 ||
            sc->total_using > using_before) &&
           jiffies - start < 500) {
        schedule();
        arch_local_irq_enable();   // schedule() returns with IRQs off — re-enable so jiffies advances
    }

    uint64_t ran = arch_atomic_read(&exiter_ran);
    if (ran < 8) {
        // Exiters exist but never got scheduled within the budget —
        // a scheduling/timing problem, not a reclaim one. Report it
        // distinctly so it cannot be mistaken for a stack leak.
        serial_printk("[selftest] kthread_self_reap: "
                      "only %lu of 8 exiters ran within budget "
                      "(using_before=%lu using_after=%lu)\n",
                      (unsigned long)ran,
                      (unsigned long)using_before,
                      (unsigned long)sc->total_using);
        return -1;
    }
    if (sc->total_using > using_before) {
        serial_printk("[selftest] kthread_self_reap: "
                      "leaked stacks: using_before=%lu using_after=%lu\n",
                      (unsigned long)using_before,
                      (unsigned long)sc->total_using);
        return -1;
    }
    return 0;
}

int test_kthread_self_reap(void)
{
    int ok = 0, fail = 0;
    serial_printk("[selftest] kthread_self_reap... ");
    if (test_kthread_self_reap_once() == 0) { ok++; serial_printk("PASS\n"); }
    else { fail++; serial_printk("FAIL\n"); }
    serial_printk("[selftest] kthread_self_reap: %d passed, %d failed\n",
                  ok, fail);
    return fail;
}

#endif // OS01_SELFTEST

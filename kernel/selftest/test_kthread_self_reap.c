#if defined(OS01_SELFTEST)

#include <sched/task.h>
#include <sched/internal.h>    // sched_self_reap_stack_count — reclaim counter
#include <memory/slab.h>       // kmalloc — the noise allocation
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
// then assert that every stack really came back.
//
// ── Why the reclaim counter, not the slab occupancy ─────────────
// A kthread's stack_alloc_base is malloc(sizeof(task_union)+STACK_SIZE)
// == 65536 bytes → the 65536-byte slab cache (index 11).  That cache is
// SHARED with user-space task unions (exec/fork allocate the same
// 65536 bytes), so the cache's global "objects in use" count moves
// whenever an unrelated task allocates.  The previous version of this
// test compared that global count before/after the measurement window,
// and CI duly failed with "leaked stacks: using_before=8 using_after=9":
// the +1 was user-space init's exec racing the selftest, not a leak.
//
// The measurement is therefore the PF_SELF_REAP path's OWN reclaim
// counter (sched_self_reap_stack_count()): it counts the stacks THIS
// path freed.  An unrelated allocation cannot move it, and a stack that
// is never freed never bumps it — so a real leak is still detected.
//
// The wait MUST be time-bounded (jiffies), not bounded by a schedule()
// count: sched_pick_cpu() may place the 8 kthreads on OTHER CPUs, and
// this selftest runs on the boot CPU inside task_init (before the idle
// loop). The boot CPU's schedule() never runs another CPU's rq, and its
// schedule() calls mostly hit the idle preemption guard and return in
// microseconds — so a fixed number of schedule() spins would exhaust in
// ~100µs while the other CPU is still in hlt waiting for a 10ms tick,
// and the stacks would never be reclaimed. A jiffies budget lets the
// other CPUs' ticks actually schedule and reap them.
static int test_kthread_self_reap_once(void)
{
    // Noise: one 64 KiB slab object (the same cache the kthread stacks
    // come from) held across the whole measurement window.  It stands
    // in for the concurrent user-space exec/fork that used to poison a
    // global-count comparison, and makes the immunity to unrelated
    // allocations a DETERMINISTIC property of this test instead of a
    // race that only CI could lose.
    void *noise = kmalloc(sizeof(union task_union) + STACK_SIZE);

    uint64_t reaped_before = sched_self_reap_stack_count();

    for (int i = 0; i < 8; i++) {
        int pid = kernel_thread(self_reap_exiter, 0, PF_KTHREAD);
        if (pid < 0) {
            serial_printk("[selftest] kthread_self_reap: "
                          "kernel_thread #%d failed\n", i);
            if (noise) kfree(noise);
            return -1;
        }
    }

    // Yield until every exiter has actually run (exiter_ran == 8) AND
    // its 64KB stack was reclaimed by the __switch_to PF_SELF_REAP
    // epilogue (each __switch_to kfree's the stack on the way out and
    // bumps the reclaim counter).
    //
    // The counter is read locklessly: another CPU's reclaim bumps it
    // with an atomic add, and on x86_64 an aligned u64 read is atomic
    // too — this is a benign progress check, not a correctness
    // dependency.
    //
    // The budget is 500 jiffies (5s at 100 Hz): under QEMU TCG with
    // -smp 8 (no KVM in CI) the APs' scheduling rounds are throttled by
    // host CPU contention while user-space init boots concurrently, and a
    // 1s window was observed to cut some AP exiters off before they had
    // run even once (CI: "leaked stacks: using_before=8 using_after=10").
    uint64_t start = jiffies;
    while ((arch_atomic_read(&exiter_ran) < 8 ||
            sched_self_reap_stack_count() - reaped_before < 8) &&
           jiffies - start < 500) {
        schedule();
        arch_local_irq_enable();   // schedule() returns with IRQs off — re-enable so jiffies advances
    }

    uint64_t ran    = arch_atomic_read(&exiter_ran);
    uint64_t reaped = sched_self_reap_stack_count() - reaped_before;
    int rc = 0;

    if (ran < 8) {
        // Exiters exist but never got scheduled within the budget —
        // a scheduling/timing problem, not a reclaim one. Report it
        // distinctly so it cannot be mistaken for a stack leak.
        serial_printk("[selftest] kthread_self_reap: "
                      "only %lu of 8 exiters ran within budget "
                      "(reaped=%lu/8)\n",
                      (unsigned long)ran,
                      (unsigned long)reaped);
        rc = -1;
    } else if (reaped < 8) {
        serial_printk("[selftest] kthread_self_reap: "
                      "leaked stacks: %lu of 8 reclaimed by the "
                      "PF_SELF_REAP path\n",
                      (unsigned long)reaped);
        rc = -1;
    }

    if (noise) kfree(noise);
    return rc;
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

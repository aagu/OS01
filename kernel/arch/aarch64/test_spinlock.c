/* aarch64 Task 2a single-core spinlock benchmark + Task 4b 4-core
 * spinlock benchmark + AP secondary-idle entry point.
 *
 * The single-core code is unchanged from Task 2a — Task 4b only
 * adds the multi-core path used by smp_boot_aps() and the AP entry
 * point called by head.S's secondary trampoline.
 *
 * 4-core benchmark protocol (spec §2.1 v11):
 *   - All four cores run `smp_bench_iter()` once each, executing
 *     `spin_lock(&bench_lock); benchmark_total++; spin_unlock(&bench_lock);`
 *     1,000,000 times (the counter is a PLAIN non-atomic ++ inside
 *     the lock — losing updates is the whole point of the test).
 *   - BSP (`test_spinlock_smp`) release-stores `benchmark_go = 1`,
 *     runs the same loop locally, then release-stores
 *     `benchmark_done[0] = 1` (v11 fix — the BSP MUST signal its own
 *     completion or the acquire-poll never sees all-done), and polls
 *     done[1..N-1] with a `arch_cycle_counter()` deadline.
 *   - PASS iff all done[i]==1 AND benchmark_total == active_cpu_count * 1M.
 *   - On timeout: FAIL and print unfinished cores + actual total.
 *
 * AP entry point `secondary_idle(cpu_id)` is the C-side tail of
 * head.S's secondary trampoline: wait for benchmark_go, run the
 * benchmark, signal done, then wfi.
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/aarch64/spinlock.h>
#include "aarch64_percpu.h"

/* Forward from pl011.c. */
void kputs(const char *s);

/* Forward from dtb.c — used by the test to size the active core
 * count (independent of smp_boot_aps()'s internal copy). */
extern uint32_t dtb_cpu_count(void);

/* Per-iteration count shared between the BSP path and the AP path. */
#define ITERATIONS_PER_CORE   1000000UL

/* Deadline = ~30 s at the QEMU virt Generic Timer frequency (62.5
 * MHz).  Generous — 4 cores × 1M LDAXR/STLXR pairs at ~100 cycles each
 * is ~400 Mcycles total; we leave ~5x slack. */
#define SMP_DEADLINE_CYCLES   (62UL * 1000UL * 1000UL * 30UL)

/* ── Single-core benchmark (Task 2a) ───────────────────────────────── */

static const char PASS_LINE[] = "[spinlock] single-core 1M PASS\n";
static const char FAIL_HDR[]   = "[spinlock] single-core FAIL got=";
static const char FAIL_TLR[]   = " want=1000000\n";

static void kputu(uint64_t v)
{
    char buf[24];
    int i = 0;
    if (v == 0) {
        kputs("0");
        return;
    }
    while (v > 0) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }
    while (i > 0) {
        char s[2];
        s[0] = buf[--i];
        s[1] = '\0';
        kputs(s);
    }
}

void test_spinlock(void)
{
    /* Single-core: a static local is enough; nothing else runs. */
    static spinlock_T lk;
    static uint64_t   counter;

    spin_init(&lk);
    counter = 0;

    for (uint64_t i = 0; i < 1000000UL; i++) {
        spin_lock(&lk);
        counter++;
        spin_unlock(&lk);
    }

    long got = spin_trylock(&lk);
    if (got == 0) {
        kputs("[spinlock] single-core FAIL trylock=0\n");
        return;
    }
    counter++;
    spin_unlock(&lk);

    uint64_t flags = spin_lock_irqsave(&lk);
    counter++;
    spin_unlock_irqrestore(&lk, flags);

    if (counter == 1000002UL) {
        kputs(PASS_LINE);
    } else {
        kputs(FAIL_HDR);
        kputu(counter);
        kputs(FAIL_TLR);
    }
}

/* ── 4-core benchmark (Task 4b) ───────────────────────────────────── */

/* Read this CPU's logical id (from TPIDR_EL1 set by head.S). */
static inline uint32_t this_cpu_id(void)
{
    uint64_t p;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(p));
    /* self pointer is offset 0; cpu_id is offset 8. */
    return *(volatile uint32_t *)(p + 8);
}

/* Run `n` lock/incr/unlock iterations, then release-store done[id]=1. */
static void smp_bench_iter(uint32_t cpu_id, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        spin_lock(&bench_lock);
        /* Plain non-atomic ++ — only protected by the lock.  Lost
         * updates (failing the spec's "total == 4,000,000" check) are
         * what this benchmark is designed to catch. */
        benchmark_total = benchmark_total + 1;
        spin_unlock(&bench_lock);
    }
    /* Release-store — must be stlr, not plain volatile write. */
    bench_done_set(cpu_id, 1);
}

/* secondary_idle(cpu_id) — AP entry point called from head.S. */
void secondary_idle(uint32_t cpu_id)
{
    /* Spin until the BSP releases us. */
    while (bench_go_get() == 0) {
        arch_cpu_pause();
    }
    smp_bench_iter(cpu_id, ITERATIONS_PER_CORE);

    /* Done; wfi forever.  CNTP tick is per-CPU but the BSP is the only
     * core expected to print [tick] in phase 1. */
    for (;;) {
        __asm__ __volatile__("dsb sy" ::: "memory");
        arch_cpu_halt();
    }
}

/* BSP path.  Called from smp_boot_aps() after all APs are online.
 * Returns 1 on PASS, 0 on FAIL. */
void test_spinlock_smp(void)
{
    /* Release-store 1 → all four cores (incl. us) start. */
    bench_go_set(1);

    /* Run the BSP's own 1M iterations. */
    uint32_t my_id = this_cpu_id();
    smp_bench_iter(my_id, ITERATIONS_PER_CORE);

    /* Wait for everyone else with a deadline. */
    uint64_t start = arch_cycle_counter();
    uint64_t deadline = start + SMP_DEADLINE_CYCLES;
    uint32_t active = dtb_cpu_count();
    bool done_seen[NR_CPUS];
    for (uint32_t i = 0; i < NR_CPUS; i++) {
        done_seen[i] = (i < active) ? false : true;
    }
    /* BSP already wrote its own done[my_id]; mark it seen. */
    done_seen[my_id] = true;

    bool all_done;
    for (;;) {
        all_done = true;
        for (uint32_t i = 0; i < active; i++) {
            if (!done_seen[i]) {
                if (bench_done_get(i) != 0) {
                    done_seen[i] = true;
                } else {
                    all_done = false;
                }
            }
        }
        if (all_done) break;
        if ((int64_t)(arch_cycle_counter() - deadline) > 0) break;
        arch_cpu_pause();
    }

    /* Read the (non-atomic) total inside the lock so we get a
     * consistent value. */
    uint32_t total;
    {
        spin_lock(&bench_lock);
        total = benchmark_total;
        spin_unlock(&bench_lock);
    }

    /* Print per-core done status. */
    for (uint32_t i = 0; i < active; i++) {
        kputs("[spinlock] cpu ");
        kputu(i);
        kputs(": done=");
        kputu(done_seen[i] ? ITERATIONS_PER_CORE : 0);
        kputs("/");
        kputu(ITERATIONS_PER_CORE);
        kputs("\n");
    }
    kputs("[spinlock] total=");
    kputu(total);
    kputs(" (active_cpus=");
    kputu(active);
    kputs(" × ");
    kputu(ITERATIONS_PER_CORE);
    if (all_done && total == (uint32_t)(active * ITERATIONS_PER_CORE)) {
        kputs(", PASS)\n");
    } else {
        kputs(", FAIL)\n");
        /* Print unfinished list for diagnostics. */
        kputs("[spinlock] FAIL diagnostics: unfinished=");
        bool any = false;
        for (uint32_t i = 0; i < active; i++) {
            if (!done_seen[i]) {
                if (any) kputs(",");
                kputu(i);
                any = true;
            }
        }
        if (!any) kputs("(none)");
        kputs(" actual_total=");
        kputu(total);
        kputs("\n");
    }
}

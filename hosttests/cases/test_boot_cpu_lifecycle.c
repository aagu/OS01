/* hosttests/cases/test_boot_cpu_lifecycle.c — BOOT-1 construction contract.
 *
 * Links the REAL kernel/percpu/percpu.c and the REAL percpu_t and pins
 * the construction/publication separation (spec §4 invariant 1):
 *
 *   construct_cpu_stays_offline
 *     percpu_init() only PREPARES data.  After it returns for CPU1:
 *     online == 0 and scheduler_ok == 0 — a boot-wait helper must not
 *     mistake a constructed CPU for a running one — while self, CPU
 *     identity and the runqueue/rq_lock scaffolding are valid.
 *
 *   construct_preserves_tail_layout
 *     The ipi_ready/tlb_ack_gen tail keeps its one-shot publication
 *     semantics: the wipe skips the last 8 bytes, the runtime online
 *     word is wiped (so a stale publication can never leak through a
 *     re-construction), and sizeof(percpu_t) stays pinned at 144.
 *     The test never constructs an already-published object — it seeds
 *     sentinel values, exactly like the boot code's fresh BSS.
 *
 * The mock (boot_cpu_runtime.h) replaces only arch_set_percpu_base;
 * every construction step asserted here is the production algorithm.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <percpu/percpu.h>

#include "test_framework.h"

/* Spy surface shared with the production object (see mock header). */
int boot_cpu_test_base_install_calls;
void *boot_cpu_test_last_base;

/* Consumer-side view: cross-CPU readers acquire, so the case reads the
 * same way a BSP waiter would. */
static uint32_t read_online(const percpu_t *cpu)
{
    return __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE);
}

static uint32_t read_scheduler_ok(const percpu_t *cpu)
{
    return __atomic_load_n(&cpu->scheduler_ok, __ATOMIC_ACQUIRE);
}

static void construct_cpu_stays_offline(void)
{
    TEST_SUITE("boot_cpu_lifecycle construct_cpu_stays_offline");

    percpu_init(1, 0x02);

    assert_true(read_online(&percpu_data[1]) == 0);
    assert_true(read_scheduler_ok(&percpu_data[1]) == 0);

    /* Identity scaffolding valid after construction. */
    assert_true(percpu_data[1].self == (uint64_t)&percpu_data[1]);
    assert_true(percpu_data[1].cpu_id == 1);
    assert_true(percpu_data[1].arch_processor_id == 0x02);

    /* Runqueue scaffolding valid: empty rbtree, unlocked rq_lock. */
    assert_true(percpu_data[1].run_queue.rb_node == NULL);
    assert_true(percpu_data[1].rq_lock.lock == 1L);

    /* Construction is pure data preparation: no machine base install. */
    assert_true(boot_cpu_test_base_install_calls == 0);
}

static void construct_preserves_tail_layout(void)
{
    TEST_SUITE("boot_cpu_lifecycle construct_preserves_tail_layout");

    /* Sentinel-seed the tail and the online word on a never-published
     * entry, exactly like fresh BSS plus hypothetical stale state. */
    percpu_data[2].ipi_ready  = 0xABCD1234u;
    percpu_data[2].tlb_ack_gen = 0x55667788u;
    percpu_data[2].online      = 0xDEADBEEFu;

    percpu_init(2, 0x22);

    /* Runtime online is wiped: a constructed CPU is never online, and
     * no stale publication can leak through construction. */
    assert_true(read_online(&percpu_data[2]) == 0);
    assert_true(read_scheduler_ok(&percpu_data[2]) == 0);

    /* Tail keeps its one-shot publication semantics: the wipe skips it. */
    assert_true(percpu_data[2].ipi_ready  == 0xABCD1234u);
    assert_true(percpu_data[2].tlb_ack_gen == 0x55667788u);

    /* Layout pins from spec §4 invariant 2. */
    assert_true(sizeof(percpu_t) == 144);
    assert_true(offsetof(percpu_t, ipi_ready) == sizeof(percpu_t) - 8);
    assert_true(offsetof(percpu_t, cpu_id) == 16);
}

int main(void)
{
    construct_cpu_stays_offline();
    construct_preserves_tail_layout();
    printf("\ntest_boot_cpu_lifecycle: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

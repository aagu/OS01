/* hosttests/cases/test_x86_smp_boot_sync.c — BOOT-1 bounded wait helper.
 *
 * Links the REAL kernel/arch/x86_64/smp/boot_sync.c and pins the wait
 * contract the x86 BSP two-stage handshake leans on (spec §6):
 *
 *   - each round: one acquire read first, non-zero ⇒ success;
 *   - otherwise one pause; at most `budget` rounds;
 *   - after the final pause there is NO extra read — a publication that
 *     lands after the last pause is NOT observed;
 *   - NULL flag or zero budget ⇒ failure without touching the flag;
 *   - the helper reads no current-task state, allocates nothing and
 *     never masks IRQs (it is a pure poll loop over the flag).
 *
 * The mock (x86_smp_boot_sync_runtime.h) counts pauses and publishes
 * the flag at a configured pause index; the polling algorithm asserted
 * here is the production one.
 */
#include <stdint.h>
#include <stdbool.h>

#include <arch/x86_64/smp/boot_sync.h>

#include "test_framework.h"

/* Spy surface shared with the production object. */
long boot_sync_test_pause_count;
long boot_sync_test_publish_at_pause;
uint32_t *boot_sync_test_flag;

static uint32_t test_flag;

static void reset_mock(uint32_t initial_flag_value)
{
    test_flag = initial_flag_value;
    boot_sync_test_flag = &test_flag;
    boot_sync_test_pause_count = 0;
    boot_sync_test_publish_at_pause = -1;
}

static void wait_initial_ready(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_initial_ready");
    reset_mock(1);
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 100) == true);
    assert_true(boot_sync_test_pause_count == 0);
}

static void wait_publish_on_first_pause(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_publish_on_first_pause");
    reset_mock(0);
    boot_sync_test_publish_at_pause = 1;
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 100) == true);
    assert_true(boot_sync_test_pause_count == 1);
}

static void wait_never_ready(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_never_ready");
    reset_mock(0);
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 5) == false);
    /* Exactly budget pauses, one per round. */
    assert_true(boot_sync_test_pause_count == 5);
}

static void wait_zero_budget(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_zero_budget");
    reset_mock(1);   /* flag IS set — proves zero budget never reads it */
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 0) == false);
    assert_true(boot_sync_test_pause_count == 0);
}

static void wait_last_check_ready(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_last_check_ready");
    /* Publish after pause #(B-1): the final round's read sees it. */
    reset_mock(0);
    boot_sync_test_publish_at_pause = 4;
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 5) == true);
    assert_true(boot_sync_test_pause_count == 4);
}

static void wait_publish_after_last_pause(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_publish_after_last_pause");
    /* Publish exactly at the final pause: no read follows it, so the
     * helper must report failure despite the flag now being set. */
    reset_mock(0);
    boot_sync_test_publish_at_pause = 5;
    assert_true(x86_smp_wait_u32_acquire(&test_flag, 5) == false);
    assert_true(boot_sync_test_pause_count == 5);
    assert_true(__atomic_load_n(&test_flag, __ATOMIC_ACQUIRE) == 1);
}

static void wait_null_flag(void)
{
    TEST_SUITE("x86_smp_boot_sync wait_null_flag");
    reset_mock(0);
    boot_sync_test_flag = NULL;   /* mock must not deref either */
    assert_true(x86_smp_wait_u32_acquire(NULL, 5) == false);
    assert_true(boot_sync_test_pause_count == 0);
}

static void boot_sync_constants(void)
{
    TEST_SUITE("x86_smp_boot_sync constants");
    assert_true(X86_SMP_READY_SPIN_LIMIT == 5000000u);
}

int main(void)
{
    wait_initial_ready();
    wait_publish_on_first_pause();
    wait_never_ready();
    wait_zero_budget();
    wait_last_check_ready();
    wait_publish_after_last_pause();
    wait_null_flag();
    boot_sync_constants();
    printf("\ntest_x86_smp_boot_sync: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

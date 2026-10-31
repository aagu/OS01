/* test_sched_notify_aarch64_backend.c — P1 Task 1: the REAL aarch64
 * sched_notify backend (kernel/arch/aarch64/cpu/sched_notify.c) keeps
 * the P1 polling contract: the notify hook is an explicit no-op, the
 * default mask is the constant 1, and the scan policy is BSP-only
 * (cpu_id() == 0).
 *
 * The backend is plain freestanding C (no arch intrinsics), so it is
 * host-compiled with the shared mock preinclude; this TU supplies the
 * fixture globals.  It must NOT define the arch hooks — those come
 * from the production object.
 */

#include "test_framework.h"
#include "../mock/sched_notify_test_runtime.h"
#include <errno.h>

int main(void)
{
    TEST_SUITE("aarch64 notify backend is the documented polling no-op");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 4;
    mock_sched_this_cpu = &percpu_data[0];

    /* The hook must return without touching any fixture state: a
     * caller-side canary proves the P1 polling contract (the common
     * layer owns the need_resched publication). */
    percpu_data[2].need_resched = 0x5A;
    arch_sched_notify_remote(2);
    assert_eq(0x5A, (int)percpu_data[2].need_resched);
    assert_eq(0, mock_ipi_send_calls);

    TEST_SUITE("aarch64 default mask is the constant 1");
    assert_eq(1ULL, arch_kthread_default_mask());

    TEST_SUITE("aarch64 scan policy is BSP-only");
    mock_sched_this_cpu = &percpu_data[0];
    assert_true(arch_sched_scan_blockers() == true);
    mock_sched_this_cpu = &percpu_data[3];
    assert_true(arch_sched_scan_blockers() == false);

    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_sched_notify_aarch64_backend",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

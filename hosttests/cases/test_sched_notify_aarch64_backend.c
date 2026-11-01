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
#include <setjmp.h>

/* The kick fail-stop: the production loop never returns, so the test
 * escapes through a longjmp from the mock halt. */
extern int mock_halt_armed;
extern void (*mock_halt_escape)(void);
static jmp_buf halt_jb;
static void halt_escape(void) { longjmp(halt_jb, 1); }

int main(void)
{
    TEST_SUITE("aarch64 kick delivers SGI4 to an IPI-ready remote target");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 4;
    mock_sched_this_cpu = &percpu_data[0];
    percpu_data[2].ipi_ready = 1;
    percpu_data[2].need_resched = 1;   /* published by the common layer */

    arch_sched_notify_remote(2);
    assert_eq(1, mock_ipi_broadcast_calls);
    assert_eq(IPI_VECTOR_RESCHED, mock_broadcast_vector);
    assert_eq(0x41, (int)mock_broadcast_vector);
    assert_eq(1ULL << 2, mock_broadcast_mask);
    /* the common layer's need_resched publication precedes the send */
    assert_eq(1, (int)mock_broadcast_need_at_call);
    assert_eq(0, mock_ipi_send_calls);   /* the x86 backend is untouched */

    TEST_SUITE("kick requires ipi_ready, NOT scheduler_ok");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 4;
    percpu_data[1].ipi_ready = 1;
    percpu_data[1].scheduler_ok = 0;   /* old work loop, takes SGIs */
    mock_ipi_broadcast_calls = 0;
    arch_sched_notify_remote(1);
    assert_eq(1, mock_ipi_broadcast_calls);
    assert_eq(1ULL << 1, mock_broadcast_mask);

    TEST_SUITE("kick to a not-IPI-ready target fail-stops without sending");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 4;
    percpu_data[3].ipi_ready = 0;
    mock_ipi_broadcast_calls = 0;
    mock_halt_armed = 1;
    mock_halt_escape = halt_escape;
    if (setjmp(halt_jb) == 0) {
        arch_sched_notify_remote(3);
        assert_true(0);                   /* must not return */
    }
    mock_halt_escape = NULL;
    assert_eq(-1, mock_halt_armed);       /* the stop was reached */
    assert_eq(0, mock_ipi_broadcast_calls);

    TEST_SUITE("local kick returns without an SGI");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 4;
    percpu_data[0].ipi_ready = 1;
    mock_ipi_broadcast_calls = 0;
    arch_sched_notify_remote(0);
    assert_eq(0, mock_ipi_broadcast_calls);

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

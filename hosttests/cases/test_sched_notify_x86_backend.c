/* test_sched_notify_x86_backend.c — P1 Task 1: the REAL x86_64
 * sched_notify backend (kernel/arch/x86_64/cpu/sched_notify.c) sends
 * the resched IPI to the target's arch_processor_id on vector 0x41,
 * and keeps the x86 default-mask / scan policy contract.
 *
 * The production object is host-compiled with the same mock preinclude
 * as the common binary; this TU supplies the fixture globals and the
 * ipi_send spy.  It must NOT define the arch hooks themselves — those
 * come from the production object.
 */

#include "test_framework.h"
#include "../mock/sched_notify_test_runtime.h"
#include <errno.h>

int mock_ipi_send_calls;
uint32_t mock_ipi_dest_apic_id;
uint8_t mock_ipi_vector;
static void reset_ipi_spy(void)
{
    mock_ipi_send_calls = 0;
    mock_ipi_dest_apic_id = 0;
    mock_ipi_vector = 0;
}

int main(void)
{
    TEST_SUITE("x86 notify backend targets arch_processor_id on 0x41");
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = i;
    num_cpus = 3;
    percpu_data[2].arch_processor_id = 9;   /* APIC id != logical id */

    reset_ipi_spy();
    arch_sched_notify_remote(2);
    assert_eq(1, mock_ipi_send_calls);
    assert_eq(9, (int)mock_ipi_dest_apic_id);
    assert_eq(IPI_VECTOR_RESCHED, mock_ipi_vector);
    assert_eq(0x41, (int)mock_ipi_vector);

    TEST_SUITE("x86 default mask covers the whole live topology");
    num_cpus = 4;
    assert_eq(0xFULL, arch_kthread_default_mask());
    num_cpus = NR_CPUS;
    assert_eq(~0ULL, arch_kthread_default_mask());

    TEST_SUITE("x86 scan policy is unconditional");
    assert_true(arch_sched_scan_blockers() == true);

    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_sched_notify_x86_backend",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

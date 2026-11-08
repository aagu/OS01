/* hosttests/cases/test_boot_services_mechanisms.c — BOOT-5 Task 3 mechanisms test.
 *
 * Verifies arch_sched_boot_prepare and arch_sched_boot_idle mechanisms:
 *   - arch_sched_boot_prepare calls set_tss64 with TSS64_Table and 10 params,
 *     and updates init_tss[0].rsp0.
 *   - arch_sched_boot_idle:
 *     need_resched == 0 halts without schedule;
 *     need_resched == 1 halts -> schedule -> IRQ-enable -> halts again.
 */

#include "test_framework.h"
#include <percpu/percpu.h>
#include <arch/boot.h>
#include <arch/x86_64/tss.h>
#include <sched/task.h>
#include "../mock/boot_services_mechanisms_runtime.h"

/* Real externs from task_arch.c and sched_boot.c */
extern void arch_sched_boot_prepare(void);
extern void arch_sched_boot_idle(void) __attribute__((noreturn));
extern struct tss_struct init_tss[NR_CPUS];
extern thread_t init_thread;

/* Mock TSS64 table */
uint32_t TSS64_Table[128];

/* Mock runtime state */
set_tss64_record_t g_set_tss64_rec;
int g_arch_cpu_halt_calls = 0;
int g_schedule_calls = 0;
int g_arch_local_irq_enable_calls = 0;
bool g_irq_enabled = true;
jmp_buf g_mechanisms_exit_jmp;

/* Mock percpu */
union task_union init_task_union;
percpu_t percpu_data[NR_CPUS];
static unsigned mock_cpu = 0;
void *arch_this_cpu_ptr(void) { return &percpu_data[mock_cpu]; }

/* Mock set_tss64 */
void mock_set_tss64(void *Table, unsigned long rsp0, unsigned long rsp1, unsigned long rsp2,
                    unsigned long ist1, unsigned long ist2, unsigned long ist3,
                    unsigned long ist4, unsigned long ist5, unsigned long ist6, unsigned long ist7)
{
    g_set_tss64_rec.call_count++;
    g_set_tss64_rec.table = (uint32_t *)Table;
    g_set_tss64_rec.rsp0 = rsp0;
    g_set_tss64_rec.rsp1 = rsp1;
    g_set_tss64_rec.rsp2 = rsp2;
    g_set_tss64_rec.ist1 = ist1;
    g_set_tss64_rec.ist2 = ist2;
    g_set_tss64_rec.ist3 = ist3;
    g_set_tss64_rec.ist4 = ist4;
    g_set_tss64_rec.ist5 = ist5;
    g_set_tss64_rec.ist6 = ist6;
    g_set_tss64_rec.ist7 = ist7;
}

/* Mock arch_cpu_halt */
void arch_cpu_halt(void)
{
    g_arch_cpu_halt_calls++;
    if (g_arch_cpu_halt_calls == 1) {
        /* First halt: need_resched is 0. Sched should not be called.
         * Set need_resched to 1 for next iteration, simulate IRQs disabled. */
        assert_eq(0, g_schedule_calls);
        percpu_data[mock_cpu].need_resched = 1;
        g_irq_enabled = false;
    } else if (g_arch_cpu_halt_calls == 2) {
        /* Second halt: schedule should have been called and IRQ re-enabled. */
        assert_eq(1, g_schedule_calls);
        assert_true(g_irq_enabled);
        longjmp(g_mechanisms_exit_jmp, 1);
    }
}

/* Mock schedule */
void schedule(void)
{
    g_schedule_calls++;
    /* Simulate schedule clearing need_resched and returning with IRQs disabled */
    percpu_data[mock_cpu].need_resched = 0;
    g_irq_enabled = false;
}

/* Mock arch_local_irq_enable */
void arch_local_irq_enable(void)
{
    g_arch_local_irq_enable_calls++;
    g_irq_enabled = true;
}

void idle_resume(void) {}

static void test_arch_sched_boot_prepare(void)
{
    memset(&g_set_tss64_rec, 0, sizeof(g_set_tss64_rec));
    memset(&init_tss[0], 0, sizeof(init_tss[0]));
    init_thread.rsp0 = 0x9000;
    init_tss[0].rsp1 = 0x1111;
    init_tss[0].rsp2 = 0x2222;
    init_tss[0].ist1 = 0x1001;
    init_tss[0].ist2 = 0x1002;
    init_tss[0].ist3 = 0x1003;
    init_tss[0].ist4 = 0x1004;
    init_tss[0].ist5 = 0x1005;
    init_tss[0].ist6 = 0x1006;
    init_tss[0].ist7 = 0x1007;

    arch_sched_boot_prepare();

    assert_eq(1, g_set_tss64_rec.call_count);
    assert_true(g_set_tss64_rec.table == TSS64_Table);
    assert_eq(0x9000, g_set_tss64_rec.rsp0);
    assert_eq(0x1111, g_set_tss64_rec.rsp1);
    assert_eq(0x2222, g_set_tss64_rec.rsp2);
    assert_eq(0x1001, g_set_tss64_rec.ist1);
    assert_eq(0x1002, g_set_tss64_rec.ist2);
    assert_eq(0x1003, g_set_tss64_rec.ist3);
    assert_eq(0x1004, g_set_tss64_rec.ist4);
    assert_eq(0x1005, g_set_tss64_rec.ist5);
    assert_eq(0x1006, g_set_tss64_rec.ist6);
    assert_eq(0x1007, g_set_tss64_rec.ist7);
    assert_eq(0x9000, init_tss[0].rsp0);
}

static void test_arch_sched_boot_idle(void)
{
    g_arch_cpu_halt_calls = 0;
    g_schedule_calls = 0;
    g_arch_local_irq_enable_calls = 0;
    g_irq_enabled = true;
    percpu_data[mock_cpu].need_resched = 0;

    if (setjmp(g_mechanisms_exit_jmp) == 0) {
        arch_sched_boot_idle();
    }

    assert_eq(2, g_arch_cpu_halt_calls);
    assert_eq(1, g_schedule_calls);
    assert_eq(1, g_arch_local_irq_enable_calls);
    assert_true(g_irq_enabled);
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_mechanisms");

    test_arch_sched_boot_prepare();
    test_arch_sched_boot_idle();

    TEST_RESULTS();
    return 0;
}

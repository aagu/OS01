/* @os01-test
 * id: test_unwind_x86_stack
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: pr
 * scenarios: []
 */
#include <test_framework.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define timeval os01_uapi_timeval
#define timespec os01_uapi_timespec
#include <percpu/percpu.h>
#include <sched/task.h>
#undef timeval
#undef timespec

#include <arch/regs.h>
#include <arch/unwind.h>
#include <arch/x86_64/unwind.h>
#include <arch/x86_64/tss.h>

extern const char x86_unwind_load_next_fp[];
extern const char x86_unwind_load_ret_pc[];
extern const char x86_unwind_load_fail[];

__asm__(
    ".section .rodata\n"
    ".globl x86_unwind_load_next_fp\n"
    "x86_unwind_load_next_fp:\n"
    "    .space 8\n"
    ".globl x86_unwind_load_ret_pc\n"
    "x86_unwind_load_ret_pc:\n"
    "    .space 8\n"
    ".globl x86_unwind_load_fail\n"
    "x86_unwind_load_fail:\n"
    "    .space 8\n"
);

percpu_t percpu_data[NR_CPUS];
unsigned int TSS64_Table[26] __attribute__((aligned(16)));
struct tss_struct init_tss[NR_CPUS];
union task_union init_task_union __attribute__((aligned(32768)));

bool x86_unwind_load_pair(uintptr_t addr, struct unwind_frame *out)
{
    (void)addr;
    (void)out;
    return false;
}

static void reset_fixtures(void)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    memset(TSS64_Table, 0, sizeof(TSS64_Table));
    memset(init_tss, 0, sizeof(init_tss));
    memset(&init_task_union, 0, sizeof(init_task_union));
}

TEST_FUNC(test_x86_bsp_unpublished_and_ap_unpublished)
{
    reset_fixtures();
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = 0x08;
    regs.rip = UINT64_C(0xffff800000101234);
    regs.rsp = (uint64_t)(uintptr_t)&init_task_union + STACK_SIZE - 0x100;
    regs.rbp = regs.rsp + 0x20;

    struct unwind_cursor c;
    /* BSP (cpu 0) unpublished accepts init_task_union */
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[0]);
    assert_true(c.kernel_origin);
    assert_true(c.stack_low < c.stack_high);
    assert_eq((uintptr_t)&init_task_union + STACK_SIZE, c.stack_high);

    /* AP (cpu 1) unpublished with saved SP in BSP init_task_union -> PC-only */
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_true(c.kernel_origin);
    assert_eq(c.stack_low, c.stack_high);

    /* Invalid GS base (misaligned or outside percpu_data[]) -> PC-only */
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[0] + 8);
    assert_eq(c.stack_low, c.stack_high);
    x86_unwind_init_with_gs(&c, &regs, true, 0);
    assert_eq(c.stack_low, c.stack_high);
}

TEST_FUNC(test_x86_published_ap_and_ist_and_invalid_tss)
{
    reset_fixtures();
    const uint64_t ap_rsp0 = UINT64_C(0xffff800020008000);
    percpu_data[1].cpu_id = 1;
    percpu_data[1].tss_hw = &init_tss[1];
    init_tss[1].rsp0 = ap_rsp0;
    __atomic_store_n(&percpu_data[1].online, 1, __ATOMIC_RELEASE);

    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = 0x08;
    regs.rip = UINT64_C(0xffff800000105555);
    /* Saved RSP inside AP's task stack (different from IST current RSP!) */
    regs.rsp = ap_rsp0 - 0x400;
    regs.rbp = ap_rsp0 - 0x300;

    struct unwind_cursor c;
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_true(c.kernel_origin);
    assert_true(c.stack_low < c.stack_high);
    assert_eq((uintptr_t)ap_rsp0, c.stack_high);

    /* AP saved SP falling in BSP init_task_union -> PC-only */
    regs.rsp = (uint64_t)(uintptr_t)&init_task_union + STACK_SIZE - 0x100;
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_eq(c.stack_low, c.stack_high);

    /* Invalid TSS pointer (e.g. AP pointing to TSS64_Table or NULL) -> PC-only */
    regs.rsp = ap_rsp0 - 0x400;
    percpu_data[1].tss_hw = TSS64_Table;
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_eq(c.stack_low, c.stack_high);

    percpu_data[1].tss_hw = NULL;
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_eq(c.stack_low, c.stack_high);

    /* Invalid TSS rsp0 (non-canonical / misaligned) -> PC-only */
    percpu_data[1].tss_hw = &init_tss[1];
    init_tss[1].rsp0 = ap_rsp0 + 16; /* not STACK_SIZE aligned */
    x86_unwind_init_with_gs(&c, &regs, true, (uintptr_t)&percpu_data[1]);
    assert_eq(c.stack_low, c.stack_high);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_x86_bsp_unpublished_and_ap_unpublished),
    TEST_ENTRY(test_x86_published_ap_and_ist_and_invalid_tss),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("unwind-x86-stack");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

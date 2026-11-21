/* @os01-test
 * id: test_unwind_aarch64
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

#undef __x86_64__
#define __aarch64__ 1
#define timeval os01_uapi_timeval
#define timespec os01_uapi_timespec
#include <percpu/percpu.h>
#include <sched/task.h>
#undef timeval
#undef timespec

#include <arch/regs.h>
#include <arch/unwind.h>
#include <arch/aarch64/unwind.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/boot_layout.h>

extern const char aarch64_unwind_load_next_fp[];
extern const char aarch64_unwind_load_ret_pc[];
extern const char aarch64_unwind_load_fail[];

__asm__(
    ".section .rodata\n"
    ".globl aarch64_unwind_load_next_fp\n"
    "aarch64_unwind_load_next_fp:\n"
    "    .space 8\n"
    ".globl aarch64_unwind_load_ret_pc\n"
    "aarch64_unwind_load_ret_pc:\n"
    "    .space 8\n"
    ".globl aarch64_unwind_load_fail\n"
    "aarch64_unwind_load_fail:\n"
    "    .space 8\n"
);

percpu_t percpu_data[NR_CPUS];

uint64_t aarch64_boot_stack_top_addr(uint32_t cpu)
{
    return UINT64_C(0x40081000) + (uint64_t)cpu * UINT64_C(0x1000);
}

bool aarch64_unwind_load_pair(uintptr_t addr, struct unwind_frame *out)
{
    (void)addr;
    (void)out;
    return false;
}

static struct pt_regs make_a64_regs(uint64_t spsr, uint64_t elr, uint64_t x0)
{
    struct pt_regs r;
    memset(&r, 0x5a, sizeof(r));
    r.spsr_el1 = spsr;
    r.elr_el1 = elr;
    r.x0 = x0;
    return r;
}

static bool only_elr_changed(const struct pt_regs *before,
                             const struct pt_regs *after,
                             uint64_t expected_elr)
{
    struct pt_regs expected = *before;
    expected.elr_el1 = expected_elr;
    return memcmp(&expected, after, sizeof(expected)) == 0;
}

static uint64_t make_dabt_esr(uint64_t fsc, uint64_t extra_iss)
{
    return (AARCH64_EC_DATA_ABORT_SAME << AARCH64_ESR_EC_SHIFT) |
           (fsc & AARCH64_ESR_ISS_FSC_MASK) | extra_iss;
}

TEST_FUNC(test_a64_fixup_accepts_valid_read_aborts)
{
    const uintptr_t fp = UINT64_C(0xffff000040100000);
    const uint64_t fail_pc = (uint64_t)(uintptr_t)aarch64_unwind_load_fail;
    const uint64_t pc0 = (uint64_t)(uintptr_t)aarch64_unwind_load_next_fp;
    const uint64_t pc1 = (uint64_t)(uintptr_t)aarch64_unwind_load_ret_pc;

    static const uint64_t valid_fscs[] = {
        0x04, 0x05, 0x06, 0x07,
        0x09, 0x0a, 0x0b,
        0x0d, 0x0e, 0x0f,
    };

    for (size_t i = 0; i < sizeof(valid_fscs) / sizeof(valid_fscs[0]); i++) {
        uint64_t esr = make_dabt_esr(valid_fscs[i], 0);
        struct pt_regs r0 = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc0, fp);
        struct pt_regs b0 = r0;
        assert_true(aarch64_unwind_fixup(&r0, esr, fp));
        assert_true(only_elr_changed(&b0, &r0, fail_pc));

        struct pt_regs r1 = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc1, fp);
        struct pt_regs b1 = r1;
        assert_true(aarch64_unwind_fixup(&r1, esr, fp + 8));
        assert_true(only_elr_changed(&b1, &r1, fail_pc));
    }
}

TEST_FUNC(test_a64_fixup_rejects_invalid_aborts)
{
    const uintptr_t fp = UINT64_C(0xffff000040100000);
    const uint64_t pc0 = (uint64_t)(uintptr_t)aarch64_unwind_load_next_fp;
    const uint64_t pc1 = (uint64_t)(uintptr_t)aarch64_unwind_load_ret_pc;
    const uint64_t ok_esr = make_dabt_esr(0x07, 0);

    /* EL0 */
    struct pt_regs r_el0 = make_a64_regs(AARCH64_SPSR_MODE_EL0T, pc0, fp);
    struct pt_regs b_el0 = r_el0;
    assert_false(aarch64_unwind_fixup(&r_el0, ok_esr, fp));
    assert_eq(0, memcmp(&b_el0, &r_el0, sizeof(r_el0)));

    /* FnV, WnR, CM, S1PTW */
    static const uint64_t bad_iss[] = {
        AARCH64_ESR_FNV_BIT,
        AARCH64_ESR_ISS_WNR,
        AARCH64_ESR_ISS_CM,
        AARCH64_ESR_ISS_S1PTW,
    };
    for (size_t i = 0; i < sizeof(bad_iss) / sizeof(bad_iss[0]); i++) {
        struct pt_regs r = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc0, fp);
        struct pt_regs b = r;
        assert_false(aarch64_unwind_fixup(&r, make_dabt_esr(0x07, bad_iss[i]), fp));
        assert_eq(0, memcmp(&b, &r, sizeof(r)));
    }

    /* External abort / unsupported FSC (0x08, 0x0c, 0x10, 0x11) */
    static const uint64_t bad_fscs[] = { 0x08, 0x0c, 0x10, 0x11 };
    for (size_t i = 0; i < sizeof(bad_fscs) / sizeof(bad_fscs[0]); i++) {
        struct pt_regs r = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc0, fp);
        struct pt_regs b = r;
        assert_false(aarch64_unwind_fixup(&r, make_dabt_esr(bad_fscs[i], 0), fp));
        assert_eq(0, memcmp(&b, &r, sizeof(r)));
    }

    /* FAR mismatch */
    struct pt_regs r_far = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc0, fp);
    struct pt_regs b_far = r_far;
    assert_false(aarch64_unwind_fixup(&r_far, ok_esr, fp + 8));
    assert_eq(0, memcmp(&b_far, &r_far, sizeof(r_far)));

    /* Unknown PC */
    struct pt_regs r_pc = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc0 + 4, fp);
    struct pt_regs b_pc = r_pc;
    assert_false(aarch64_unwind_fixup(&r_pc, ok_esr, fp));
    assert_eq(0, memcmp(&b_pc, &r_pc, sizeof(r_pc)));

    /* x0 + 8 overflow on load 1 */
    struct pt_regs r_ovf = make_a64_regs(AARCH64_SPSR_MODE_EL1H, pc1, UINT64_MAX - 3);
    struct pt_regs b_ovf = r_ovf;
    assert_false(aarch64_unwind_fixup(&r_ovf, ok_esr, (uintptr_t)((UINT64_MAX - 3) + 8)));
    assert_eq(0, memcmp(&b_ovf, &r_ovf, sizeof(r_ovf)));
}

TEST_FUNC(test_a64_stack_ownership_boot_and_task)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    aarch64_unwind_publish_task_stack(0, 0);
    aarch64_unwind_publish_task_stack(1, 0);

    const uintptr_t bsp_boot_top = (uintptr_t)(AARCH64_PAGE_OFFSET + aarch64_boot_stack_top_addr(0));
    const uintptr_t ap_low_sp = (uintptr_t)aarch64_boot_stack_top_addr(1) - 0x100;

    uintptr_t low = 0, high = 0;

    /* Invalid TPIDR */
    assert_false(aarch64_unwind_resolve_stack(0, bsp_boot_top - 0x100, &low, &high));
    assert_false(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[0] + 8, bsp_boot_top - 0x100, &low, &high));

    /* Adjacent slot (CPU 1 TPIDR with CPU 0 boot SP) */
    assert_false(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[1], bsp_boot_top - 0x100, &low, &high));

    /* AP low identity-mapped SP */
    assert_false(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[1], ap_low_sp, &low, &high));

    /* Valid CPU 0 high-half boot SP */
    assert_true(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[0], bsp_boot_top - 0x100, &low, &high));
    assert_eq(bsp_boot_top, high);

    /* Published task stack and lower-bound geometry check */
    const uintptr_t task_base = UINT64_C(0xffff000050000000);
    const uintptr_t expected_low = (task_base + sizeof(task_t) + 256u + 15u) & ~(uintptr_t)15u;
    aarch64_unwind_publish_task_stack(0, task_base);

    /* Stale SP on boot stack is rejected once task stack is published */
    assert_false(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[0], bsp_boot_top - 0x100, &low, &high));

    /* Lower boundary: expected_low accepted, expected_low - 8 rejected */
    assert_true(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[0], expected_low, &low, &high));
    assert_eq(expected_low, low);
    assert_eq(task_base + STACK_SIZE, high);
    assert_false(aarch64_unwind_resolve_stack((uintptr_t)&percpu_data[0], expected_low - 8, &low, &high));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_a64_fixup_accepts_valid_read_aborts),
    TEST_ENTRY(test_a64_fixup_rejects_invalid_aborts),
    TEST_ENTRY(test_a64_stack_ownership_boot_and_task),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("unwind-aarch64");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

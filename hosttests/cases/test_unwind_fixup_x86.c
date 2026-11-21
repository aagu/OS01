/* @os01-test
 * id: test_unwind_fixup_x86
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: pr
 * scenarios: []
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
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

percpu_t percpu_data[NR_CPUS];
unsigned int TSS64_Table[26];
struct tss_struct init_tss[NR_CPUS];
union task_union init_task_union;

extern const char x86_unwind_load_next_fp[];
extern const char x86_unwind_load_ret_pc[];
extern const char x86_unwind_load_fail[];

bool x86_unwind_load_pair(uintptr_t addr, struct unwind_frame *out)
{
    (void)addr;
    (void)out;
    return false;
}

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

static struct pt_regs make_regs(uint64_t cs, uint64_t rip, uint64_t rdi)
{
    struct pt_regs r;
    memset(&r, 0x5a, sizeof(r));
    r.cs = cs;
    r.rip = rip;
    r.rdi = rdi;
    return r;
}

static bool only_rip_changed(const struct pt_regs *before,
                             const struct pt_regs *after,
                             uint64_t expected_rip)
{
    struct pt_regs expected = *before;
    expected.rip = expected_rip;
    return memcmp(&expected, after, sizeof(expected)) == 0;
}

TEST_FUNC(test_fixup_accepts_cpl0_read_pf_both_loads)
{
    const uintptr_t fp = UINT64_C(0xffff800012345670);
    const uint64_t fail_pc = (uint64_t)(uintptr_t)x86_unwind_load_fail;

    /* Load 0 (next_fp) with error_code = 0 */
    struct pt_regs r1 = make_regs(0x08, (uint64_t)(uintptr_t)x86_unwind_load_next_fp, fp);
    struct pt_regs b1 = r1;
    assert_true(x86_unwind_fixup(&r1, 0, fp));
    assert_true(only_rip_changed(&b1, &r1, fail_pc));

    /* Load 0 with error_code = 1 (P bit set) */
    struct pt_regs r2 = make_regs(0x08, (uint64_t)(uintptr_t)x86_unwind_load_next_fp, fp);
    struct pt_regs b2 = r2;
    assert_true(x86_unwind_fixup(&r2, 1, fp));
    assert_true(only_rip_changed(&b2, &r2, fail_pc));

    /* Load 1 (return_pc at fp + 8) with error_code = 0 and 1 */
    struct pt_regs r3 = make_regs(0x08, (uint64_t)(uintptr_t)x86_unwind_load_ret_pc, fp);
    struct pt_regs b3 = r3;
    assert_true(x86_unwind_fixup(&r3, 0, fp + 8));
    assert_true(only_rip_changed(&b3, &r3, fail_pc));

    struct pt_regs r4 = make_regs(0x08, (uint64_t)(uintptr_t)x86_unwind_load_ret_pc, fp);
    struct pt_regs b4 = r4;
    assert_true(x86_unwind_fixup(&r4, 1, fp + 8));
    assert_true(only_rip_changed(&b4, &r4, fail_pc));
}

TEST_FUNC(test_fixup_rejects_non_matching_faults)
{
    const uintptr_t fp = UINT64_C(0xffff800012345670);
    const uint64_t pc0 = (uint64_t)(uintptr_t)x86_unwind_load_next_fp;
    const uint64_t pc1 = (uint64_t)(uintptr_t)x86_unwind_load_ret_pc;

    /* Flip CPL (user mode cs=0x1b) */
    struct pt_regs r_cpl = make_regs(0x1b, pc0, fp);
    struct pt_regs b_cpl = r_cpl;
    assert_false(x86_unwind_fixup(&r_cpl, 0, fp));
    assert_eq(0, memcmp(&b_cpl, &r_cpl, sizeof(r_cpl)));

    /* Flip W/R (bit 1), U/S (bit 2), RSVD (bit 3), I/D (bit 4), PK (bit 5) */
    static const uint64_t bad_errs[] = { 0x2, 0x4, 0x8, 0x10, 0x20, 0x3 };
    for (size_t i = 0; i < sizeof(bad_errs) / sizeof(bad_errs[0]); i++) {
        struct pt_regs r = make_regs(0x08, pc0, fp);
        struct pt_regs b = r;
        assert_false(x86_unwind_fixup(&r, bad_errs[i], fp));
        assert_eq(0, memcmp(&b, &r, sizeof(r)));
    }

    /* Address mismatch on load 0 and load 1 */
    struct pt_regs r_addr0 = make_regs(0x08, pc0, fp);
    struct pt_regs b_addr0 = r_addr0;
    assert_false(x86_unwind_fixup(&r_addr0, 0, fp + 8));
    assert_eq(0, memcmp(&b_addr0, &r_addr0, sizeof(r_addr0)));

    struct pt_regs r_addr1 = make_regs(0x08, pc1, fp);
    struct pt_regs b_addr1 = r_addr1;
    assert_false(x86_unwind_fixup(&r_addr1, 0, fp));
    assert_eq(0, memcmp(&b_addr1, &r_addr1, sizeof(r_addr1)));

    /* RIP mismatch */
    struct pt_regs r_rip = make_regs(0x08, pc0 + 4, fp);
    struct pt_regs b_rip = r_rip;
    assert_false(x86_unwind_fixup(&r_rip, 0, fp));
    assert_eq(0, memcmp(&b_rip, &r_rip, sizeof(r_rip)));

    /* rdi + 8 overflow on load 1 */
    const uint64_t near_max = UINT64_MAX - 3;
    struct pt_regs r_ovf = make_regs(0x08, pc1, near_max);
    struct pt_regs b_ovf = r_ovf;
    assert_false(x86_unwind_fixup(&r_ovf, 0, (uintptr_t)(near_max + 8)));
    assert_eq(0, memcmp(&b_ovf, &r_ovf, sizeof(r_ovf)));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fixup_accepts_cpl0_read_pf_both_loads),
    TEST_ENTRY(test_fixup_rejects_non_matching_faults),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("unwind-fixup-x86");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

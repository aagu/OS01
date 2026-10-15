/*
 * test_thermal_fixup.c — kernel/monitor thermal plan, Task 1 fixup host fixture.
 *
 * Scope of THIS host fixture (monitor plan Task 1, step 1):
 *   production code under test:  arch_msr_gp_fixup(regs, error_code)
 *                                — pure C logic; no #GP needs to be raised
 *                                  in the host build (rdmsr raises #GP in
 *                                  ring 0 only, not under the host loader).
 *
 *   Symbols that must be COMPILED into the fixture by linking the REAL
 *   kernel/arch/x86_64/cpu/msr_safe.{S,c} production objects (see
 *   hosttests/Makefile target).  This guarantees the C/asm source is the
 *   shipping source — there is no host-side clone.
 *
 * Asserted (each per-test cases below):
 *   1. The marked RIP / CPL0 / err==0 triple matches → returns true AND
 *      rewrites regs->rip to arch_rdmsr_safe_fail (and ONLY rip).
 *   2. Any of {unmarked RIP, user CPL, err != 0, NULL regs} → returns
 *      false AND leaves every byte of *regs unchanged.
 *   3. Boundary: regs->rip == arch_rdmsr_safe_try BUT CPL ≠ 0 → false,
 *      regs unchanged.
 *   4. Boundary: regs->rip == arch_rdmsr_safe_try BUT err != 0 → false,
 *      regs unchanged.
 *
 * Not asserted by this fixture:
 *   - The dynamic *value preservation through a real #GP (the assembly's
 *     "fixup path stores nothing" guarantee).  That requires executing
 *     rdmsr under ring 0; it is the kernel-side selftest's job, see
 *     kernel/selftest/test_msr_safe.c, gated on
 *     KERNEL_RUN_MSRS_SAFE_SELFTEST=1.
 *   - The unmarked-RIP user-mode segfault path.  Already handled by
 *     kernel/arch/x86_64/intr/trap.c (user #GP → SIGSEGV via
 *     kill_current_user_task); no host assertion duplicates it.
 *
 * Negative tests (NEG_* markers) are deliberate: if the fixup function
 * ever loosens its matching criteria, these turn red before a real
 * regression crashes the kernel.  Removing them violates plan step 4
 * ("保留一般#GP的host负例证据").
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/x86_64/msr_safe.h>
#include <arch/x86_64/regs.h>   /* pt_regs_t (production layout) */

/* ── Production-derived label addresses ────────────────────────
 * The host binary links kernel/arch/x86_64/cpu/msr_safe.S which exports
 * arch_rdmsr_safe_try and arch_rdmsr_safe_fail.  We treat them as opaque
 * pointers; the test never inspects the instruction bytes — only ever
 * takes the address & compares for equality. */
extern const char arch_rdmsr_safe_try[];
extern const char arch_rdmsr_safe_fail[];

/* ── Test scratch ─────────────────────────────────────────────── */

static pt_regs_t mk_regs(uint64_t rip, uint64_t cs, uint64_t errcode)
{
    pt_regs_t r;
    memset(&r, 0, sizeof(r));
    /* Fill with a deterministic non-zero pattern so the test catches any
     * field the fixup is NOT supposed to touch. */
    r.rip    = rip;
    r.cs     = cs;
    r.errcode = errcode;
    r.rdi    = 0x1111111111111111ULL;
    r.rsi    = 0x2222222222222222ULL;
    r.rdx    = 0x3333333333333333ULL;
    r.rcx    = 0x4444444444444444ULL;
    r.rflags = 0x5555555555555555ULL;
    r.func   = (uint64_t)(uintptr_t)0xCAFEBABEDEADBEEFULL;
    return r;
}

static int regs_equal(const pt_regs_t *a, const pt_regs_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* ── Test cases ───────────────────────────────────────────────── */

static void test_marked_rip_kernel_cs_no_err_redirects(void)
{
    TEST_SUITE("fixup");
    pt_regs_t r = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_try,
                          X86_KERNEL_CS, 0);
    bool rc = arch_msr_gp_fixup(&r, 0);

    assert_true(rc);
    assert_true(r.rip == (uint64_t)(uintptr_t)arch_rdmsr_safe_fail);
    /* Only rip must change — every other slot, including func, is
     * explicitly NOT permitted to be rewritten (plan: "命中只把RIP移到
     * error返回标签,其余返回false"). */
    pt_regs_t expected = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_try,
                                 X86_KERNEL_CS, 0);
    expected.rip = (uint64_t)(uintptr_t)arch_rdmsr_safe_fail;
    assert_true(regs_equal(&expected, &r));
}

static void test_unmarked_rip_returns_false_and_preserves_regs(void)
{
    pt_regs_t r = mk_regs(0xdeadbeefcafebabeULL, X86_KERNEL_CS, 0);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, 0);

    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

static void test_user_cs_returns_false_and_preserves_regs(void)
{
    pt_regs_t r = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_try,
                          /* X86_USER_CS = 0x2B, plus RPL=3 already; the
                           * field's CPL bits are `cs & 3` per x86 SDM */
                          X86_USER_CS, 0);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, 0);

    /* Spec: unmarked user CS must NOT trigger the fixup — user #GP
     * continues down the original do_general_protection path so the
     * fault is delivered to the user task as SIGSEGV.  Verified by
     * memcmp: regs unchanged. */
    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

static void test_nonzero_err_returns_false_and_preserves_regs(void)
{
    pt_regs_t r = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_try,
                          X86_KERNEL_CS, /* GP error code selector bit */
                          0x4 /* TI=1, IDT=0 — selector index in LDT */);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, r.errcode);

    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

static void test_user_cs_with_err_returns_false_and_preserves_regs(void)
{
    /* Compound non-match: both CPL=3 AND err!=0. */
    pt_regs_t r = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_try,
                          X86_USER_CS, 0x1234);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, r.errcode);

    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

static void test_attempted_fail_label_returns_false(void)
{
    /* Even if a wild pointer matches the FAIL label, we must NOT redirect:
     * only the TRY label is the marked faulting instruction. */
    pt_regs_t r = mk_regs((uint64_t)(uintptr_t)arch_rdmsr_safe_fail,
                          X86_KERNEL_CS, 0);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, 0);

    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

static void test_null_regs_returns_false(void)
{
    /* Defensive: a bogus regs=NULL must not crash the host build. */
    bool rc = arch_msr_gp_fixup((pt_regs_t *)0, 0);
    assert_false(rc);
}

static void test_user_rip_unrelated_returns_false(void)
{
    /* Sanity: pseudo-random RIP in user CS does nothing.  This is the
     * canonical "general #GP from userspace" the kernel keeps delivering
     * as SIGSEGV. */
    pt_regs_t r = mk_regs(0x00007f1234567890ULL, X86_USER_CS, 0);
    pt_regs_t saved = r;
    bool rc = arch_msr_gp_fixup(&r, 0);

    assert_false(rc);
    assert_true(regs_equal(&saved, &r));
}

/* ── Final summary ──────────────────────────────────────────────
 * The framework tracks pass/fail on per-assertion counters; print a one-
 * line summary so the .elf exit code reflects overall health. */
int main(void)
{
    test_marked_rip_kernel_cs_no_err_redirects();
    test_unmarked_rip_returns_false_and_preserves_regs();
    test_user_cs_returns_false_and_preserves_regs();
    test_nonzero_err_returns_false_and_preserves_regs();
    test_user_cs_with_err_returns_false_and_preserves_regs();
    test_attempted_fail_label_returns_false();
    test_null_regs_returns_false();
    test_user_rip_unrelated_returns_false();

    printf("\n  == thermal_fixup summary: %d/%d passed, %d failed ==\n",
           __test_stats.passed, __test_stats.total, __test_stats.failed);
    return __test_stats.failed == 0 ? 0 : 1;
}

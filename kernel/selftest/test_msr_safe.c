#if defined(OS01_SELFTEST)
/* kernel/selftest/test_msr_safe.c
 *
 * Thermal plan, Task 1 — in-kernel safe-RDMSR selftest.
 *
 * The host fixture (hosttests/cases/test_thermal_fixup.c) already
 * proves the assembly fixup is byte-equality strict and the C
 * helper preserves the caller's *value on -EIO.  This body covers
 * the in-kernel half that no host test can: the helper hooked into
 * the live #GP handler with a fabricated regs struct, the
 * non-marked-RIP path passing through unchanged, and a real
 * arch_rdmsr_safe against a known-good MSR.
 *
 * Gated behind KERNEL_RUN_MSRS_SAFE_SELFTEST:
 *   - on QEMU without the production MSR model this is benign
 *     (0x10 / TSC), but running it before the implementation is
 *     wired would loop on the negative case forever.
 *   - on real hardware it is the only way to actually trigger the
 *     unmarked-RIP path being passed through unchanged.
 */

#include <arch/x86_64/msr_safe.h>     /* arch_rdmsr_safe, arch_msr_gp_fixup,
                                          arch_rdmsr_safe_try/fail */
#include <arch/x86_64/regs.h>          /* pt_regs_t, X86_KERNEL_CS */
#include <arch/irq.h>                  /* arch_local_irq_save/restore */
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include <core/printk.h>

static int test_msr_safe_known_good(void)
{
    /* IA32_TSC_DEADLINE (0x6E0) is implemented in QEMU's default
     * CPU model, but IA32_TSC (0x10) is the cleanest universal
     * choice — it exists on every x86_64 since the Pentium and is
     * the same MSR cpuid uses.  arch_rdmsr_safe must return 0 with
     * *value unchanged-from-input. */
    uint64_t value = 0xDEADBEEFCAFEBABEULL;
    int ret = arch_rdmsr_safe(0x10U /* IA32_TSC */, &value);
    if (ret != 0) {
        serial_printk("[selftest] msr_safe_known_good: arch_rdmsr_safe(0x10) returned %d\n", ret);
        return -1;
    }
    /* value was touched: success path writes EDX:EAX to *value. */
    if ((value >> 32) == 0xDEADBEEF && (value & 0xffffffff) == 0xCAFEBABE) {
        serial_printk("[selftest] msr_safe_known_good: *value untouched despite rc=0\n");
        return -1;
    }
    return 0;
}

static int test_msr_safe_known_bad(void)
{
    /* 0xFFFFFFFF is universally unimplemented; the assembly's rdmsr
     * raises #GP, arch_msr_gp_fixup recognizes the marked RIP,
     * redirects to fail, and the C continuation returns -EIO WITHOUT
     * writing *value. */
    const uint64_t kSentinel = 0x1122334455667788ULL;
    uint64_t value = kSentinel;
    int ret = arch_rdmsr_safe(0xFFFFFFFFU, &value);
    if (ret != -EIO) {
        serial_printk("[selftest] msr_safe_known_bad: expected %d got %d\n", -EIO, ret);
        return -1;
    }
    if (value != kSentinel) {
        serial_printk("[selftest] msr_safe_known_bad: *value changed from %llx to %llx\n",
                      (unsigned long long)kSentinel, (unsigned long long)value);
        return -1;
    }
    return 0;
}

static int test_msr_safe_fixup_unmarked_rip(void)
{
    /* Fabricate a regs struct with a RIP that is NOT the marked
     * try label.  The fixup must return false. */
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = X86_KERNEL_CS;
    regs.rip = 0xFFFF800000000000ULL;   /* nowhere near arch_rdmsr_safe_try */
    regs.rdi = 0;
    regs.rsi = 0;
    regs.rdx = 0;
    regs.rcx = 0;
    regs.rflags = 0;

    bool handled = arch_msr_gp_fixup(&regs, 0 /* error_code */);
    if (handled) {
        serial_printk("[selftest] msr_safe_fixup_unmarked_rip: helper claimed unmarked RIP\n");
        return -1;
    }
    if (regs.rip != 0xFFFF800000000000ULL) {
        serial_printk("[selftest] msr_safe_fixup_unmarked_rip: helper rewrote regs->rip anyway (%llx)\n",
                      (unsigned long long)regs.rip);
        return -1;
    }
    return 0;
}

static int test_msr_safe_fixup_user_cs(void)
{
    /* Same unmarked RIP but user-mode CS — the helper must still
     * reject (a user RIP can never be the kernel's marked
     * instruction). */
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = X86_USER_CS;
    regs.rip = (uint64_t)(uintptr_t)arch_rdmsr_safe_try;
    regs.rdi = 0;
    regs.rsi = 0;
    regs.rdx = 0;
    regs.rcx = 0;
    regs.rflags = 0;

    bool handled = arch_msr_gp_fixup(&regs, 0 /* error_code */);
    if (handled) {
        serial_printk("[selftest] msr_safe_fixup_user_cs: helper claimed user-mode RIP\n");
        return -1;
    }
    if (regs.rip != (uint64_t)(uintptr_t)arch_rdmsr_safe_try) {
        serial_printk("[selftest] msr_safe_fixup_user_cs: helper rewrote regs->rip from try to %llx\n",
                      (unsigned long long)regs.rip);
        return -1;
    }
    return 0;
}

static int test_msr_safe_fixup_nonzero_error(void)
{
    /* Non-zero error_code (e.g. selector-related GP) must NOT be
     * consumed — the helper must return false even if every other
     * field matches. */
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = X86_KERNEL_CS;
    regs.rip = (uint64_t)(uintptr_t)arch_rdmsr_safe_try;
    regs.rdi = 0;
    regs.rsi = 0;
    regs.rdx = 0;
    regs.rcx = 0;
    regs.rflags = 0;

    bool handled = arch_msr_gp_fixup(&regs, 0x1234ULL /* non-zero */);
    if (handled) {
        serial_printk("[selftest] msr_safe_fixup_nonzero_error: helper claimed GP with err=0x%llx\n",
                      (unsigned long long)0x1234ULL);
        return -1;
    }
    if (regs.rip != (uint64_t)(uintptr_t)arch_rdmsr_safe_try) {
        serial_printk("[selftest] msr_safe_fixup_nonzero_error: helper rewrote regs->rip to %llx\n",
                      (unsigned long long)regs.rip);
        return -1;
    }
    return 0;
}

static int test_msr_safe_marked_rip(void)
{
    /* Positive control: with CS=KERNEL and RIP==try and err=0,
     * helper must rewrite RIP to fail and return true. */
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.cs = X86_KERNEL_CS;
    regs.rip = (uint64_t)(uintptr_t)arch_rdmsr_safe_try;
    regs.rdi = 0;
    regs.rsi = 0;
    regs.rdx = 0;
    regs.rcx = 0;
    regs.rflags = 0;

    bool handled = arch_msr_gp_fixup(&regs, 0 /* error_code */);
    if (!handled) {
        serial_printk("[selftest] msr_safe_marked_rip: helper missed marked RIP\n");
        return -1;
    }
    if (regs.rip != (uint64_t)(uintptr_t)arch_rdmsr_safe_fail) {
        serial_printk("[selftest] msr_safe_marked_rip: helper rewrote regs->rip to %llx, want %llx\n",
                      (unsigned long long)regs.rip,
                      (unsigned long long)(uintptr_t)arch_rdmsr_safe_fail);
        return -1;
    }
    return 0;
}

int test_msr_safe(void)
{
    int ok = 0, fail = 0;
    serial_printk("[selftest] msr_safe... ");
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "known_good",            test_msr_safe_known_good         },
        { "known_bad",             test_msr_safe_known_bad          },
        { "fixup_unmarked_rip",    test_msr_safe_fixup_unmarked_rip },
        { "fixup_user_cs",         test_msr_safe_fixup_user_cs      },
        { "fixup_nonzero_error",   test_msr_safe_fixup_nonzero_error},
        { "marked_rip",            test_msr_safe_marked_rip         },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].fn() == 0) { ok++; }
        else {
            fail++;
            serial_printk("[selftest] msr_safe.%s FAIL\n", cases[i].name);
        }
    }
    serial_printk("[selftest] msr_safe: %d passed, %d failed\n", ok, fail);
    return fail;
}

#endif /* OS01_SELFTEST */

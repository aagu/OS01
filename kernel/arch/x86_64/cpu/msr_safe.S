/*
 * kernel/arch/x86_64/cpu/msr_safe.S
 *
 * Instruction-scoped safe RDMSR primitive (kernel/monitor thermal plan, Task 1).
 *
 * SysV AMD64 ABI:
 *   - arg 1 (uint32_t msr)        in %edi
 *   - arg 2 (uint64_t *value)     in %rsi
 *   - return value (int 0 / -EIO) in %eax
 *   - caller-saved: %rax, %rcx, %rdx, %rsi, %rdi, %r8..%r11
 *
 * The marked instruction is the single `rdmsr` opcode (0x0f 0x32).
 * It is the ONLY thing the C-side fixup matches against, so we
 * emit the bytes literally instead of using the `rdmsr` mnemonic
 * — the two-byte encoding is portable across toolchains and
 * assemblers that disagree on mnemonic recognition.
 *
 * The "fail" continuation loads %eax with -EIO (= -(5)) and
 * returns.  It MUST NOT write to [%rsi]; the C fixup depends on
 * the caller's `*value` being preserved across the #GP.
 *
 * No stack frame is needed: this function saves/restores only the
 * caller-saved registers it actually touches, and SysV explicitly
 * permits it to clobber %rcx/%rdx (the rdmsr implicit outputs) and
 * %eax (the return).  The function is allowed to clobber %rdx and
 * %rcx.
 */

#include <arch/x86_64/linkage.h>

.text

/*
 * arch_rdmsr_safe(uint32_t msr, uint64_t *value) -> int (0 / -EIO)
 *
 * %edi  = msr argument
 * %rsi  = &value
 *
 * We MUST put the MSR into %ecx BEFORE the marked `rdmsr` runs,
 * because `rdmsr` writes its selected MSR index back into %ecx as
 * an architectural side effect.  We save it once at the entry.
 *
 * On success: EDX:EAX holds the MSR value, which we write to
 * [%rsi], and we return 0 in %eax.
 *
 * On #GP: the C fixup redirects regs->rip to arch_rdmsr_safe_fail
 * below, so [%rsi] is never touched and the caller's `*value` is
 * preserved.
 */
ENTRY(arch_rdmsr_safe)
    movl    %edi, %ecx        /* %ecx <- msr (rdmsr clobbers %ecx) */

/* ── marked instruction begins ────────────────────────── */
.globl arch_rdmsr_safe_try
arch_rdmsr_safe_try:
    .byte   0x0f, 0x32         /* rdmsr  →  %edx:%eax = MSR[%ecx] */
/* ── marked instruction ends ──────────────────────────── */

    /* Success path: store %edx:%eax to [%rsi], return 0 in %eax.
     * This code runs only when rdmsr succeeded (no #GP was raised).
     * The store MUST NOT happen on the fail path below. */
    movl    %eax, (%rsi)
    movl    %edx, 4(%rsi)
    xorl    %eax, %eax         /* return 0 */
    ret

/*
 * arch_rdmsr_safe_fail — the fixup redirection target.
 *
 * arch_msr_gp_fixup() in kernel/arch/x86_64/cpu/msr_safe.c
 * overwrites regs->rip with this label's address when a #GP
 * fires at arch_rdmsr_safe_try; iretq resumes the interrupted
 * task here, so we MUST be a valid tail-call continuation of
 * `arch_rdmsr_safe`.
 *
 * On this path:
 *   %eax <- -EIO (signed 32-bit; errno.h: EIO == 5)
 *   ret
 *
 * Crucially, [%rsi] is NEVER written on this path — the caller's
 * `*value` is preserved across the failed probe.  This is the
 * kernel-side guarantee the plan promises (and the in-kernel
 * selftest verifies under KERNEL_RUN_MSRS_SAFE_SELFTEST=1).
 */
.globl arch_rdmsr_safe_fail
arch_rdmsr_safe_fail:
    movl    $-5, %eax          /* -EIO (errno.h: EIO == 5) */
    ret

/*
 * Object identity for the host fixture's `extern const char[]`
 * declarations.  Without .type @object, the linker emits these
 * labels as functions and the C-side address comparisons in
 * hosttests/cases/test_thermal_fixup.c would mismatch.
 */
.type arch_rdmsr_safe_try, @object
.type arch_rdmsr_safe_fail, @object
.size arch_rdmsr_safe_try, 2    /* .byte 0x0f, 0x32 */
.size arch_rdmsr_safe_fail, 6   /* movl imm32 (5 bytes) + ret (1 byte) */

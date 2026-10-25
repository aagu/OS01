#ifndef _KERNEL_ARCH_X86_64_INTR_DISPATCH_H
#define _KERNEL_ARCH_X86_64_INTR_DISPATCH_H

// ─────────────────────────────────────────────────────────
//  arch/x86_64/intr_dispatch.h — private x86_64 dispatch
//  interface (design spec §5.2 / §6.1)
//
//  This header is the private interface between the x86_64
//  routing backend (kernel/arch/x86_64/intr/dispatch.c) and
//  the original business logic it routes to
//  (kernel/arch/x86_64/intr/trap.c).  It is NOT public ABI:
//  the common dispatcher (kernel/intr/dispatch.c) must not
//  include it, and it must not include any private header of
//  the other architecture.
//
//  The x86_handle_* functions are the ORIGINAL do_*
//  bodies, moved verbatim out of the public C landing points.
//  They are cross-TU (hence not static) but stay private to
//  the x86_64 exception path; nothing outside this pair of
//  files may call them, and the backend never calls back into
//  the public do_* entries (no recursion).
//
//  x86_handle_page_fault receives the CR2 snapshot taken ONCE
//  at the very front of the thin public entry; it must never
//  re-read CR2 itself (spec §6.1).
// ─────────────────────────────────────────────────────────

#include <stdint.h>

#include <arch/regs.h>

void x86_handle_debug(pt_regs_t *regs, uint64_t error_code);
void x86_handle_nmi(pt_regs_t *regs, uint64_t error_code);
void x86_handle_int3(pt_regs_t *regs, uint64_t error_code);
void x86_handle_overflow(pt_regs_t *regs, uint64_t error_code);
void x86_handle_bounds(pt_regs_t *regs, uint64_t error_code);
void x86_handle_dev_not_available(pt_regs_t *regs, uint64_t error_code);
void x86_handle_double_fault(pt_regs_t *regs, uint64_t error_code);
void x86_handle_coprocessor_segment_overrun(pt_regs_t *regs, uint64_t error_code);
void x86_handle_invalid_TSS(pt_regs_t *regs, uint64_t error_code);
void x86_handle_segment_not_present(pt_regs_t *regs, uint64_t error_code);
void x86_handle_stack_segment_fault(pt_regs_t *regs, uint64_t error_code);
void x86_handle_x87_FPU_error(pt_regs_t *regs, uint64_t error_code);
void x86_handle_alignment_check(pt_regs_t *regs, uint64_t error_code);
void x86_handle_machine_check(pt_regs_t *regs, uint64_t error_code);
void x86_handle_SIMD_exception(pt_regs_t *regs, uint64_t error_code);
void x86_handle_virtualization_exception(pt_regs_t *regs, uint64_t error_code);

/* #DE (0): user fault → SIGFPE, kernel → original fatal path. */
void x86_handle_divide_error(pt_regs_t *regs, uint64_t error_code);

/* #UD (6): user fault → SIGILL, kernel → original fatal path. */
void x86_handle_undefined_opcode(pt_regs_t *regs, uint64_t error_code);

/* #GP (13): safe-RDMSR fixup first, then user SIGSEGV kill or the
 * original kernel fatal diagnostics. */
void x86_handle_general_protection(pt_regs_t *regs, uint64_t error_code);

/* #PF (14): uaccess fault_jmp recovery, VMA/demand/COW resolution or
 * the original termination paths — all on the caller-supplied CR2
 * snapshot. */
void x86_handle_page_fault(pt_regs_t *regs, uint64_t error_code,
                           uint64_t cr2);

__attribute__((no_stack_protector))
void x86_irq_entry(pt_regs_t *regs, uint64_t vector);
void x86_handle_device_irq(pt_regs_t *regs, uint64_t vector);
__attribute__((no_stack_protector))
void x86_handle_registered_irq(pt_regs_t *regs, uint64_t vector);
void x86_handle_lapic_timer_irq(pt_regs_t *regs, uint64_t vector);
void x86_handle_spurious_irq(pt_regs_t *regs, uint64_t vector);

#endif /* _KERNEL_ARCH_X86_64_INTR_DISPATCH_H */

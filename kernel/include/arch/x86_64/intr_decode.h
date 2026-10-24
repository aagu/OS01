#ifndef _KERNEL_ARCH_X86_64_INTR_DECODE_H
#define _KERNEL_ARCH_X86_64_INTR_DECODE_H

// ─────────────────────────────────────────────────────────
//  arch/x86_64/intr_decode.h — x86_64 event decoder
//
//  Pure function: it turns the raw state of one x86_64 exception
//  into a normalized intr_event (spec §5.1 / §6.1).  It reads
//  no system register (the CR2 snapshot is passed in), executes
//  no handler, allocates nothing, takes no lock and logs
//  nothing.
//
//  The decoder belongs to the early-reachable call chain, so it
//  must not depend on the stack canary.
// ─────────────────────────────────────────────────────────

#include <stdint.h>

#include <intr/dispatch.h>

/* `regs` must not be NULL; `out` receives a fully initialized event
 * (padding is not initialized). */
__attribute__((no_stack_protector))
void x86_intr_decode(struct intr_event *out, const pt_regs_t *regs,
                     uint64_t vector, uint64_t error_code, uint64_t cr2);

__attribute__((no_stack_protector))
void x86_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs,
                         uint64_t vector);

#endif /* _KERNEL_ARCH_X86_64_INTR_DECODE_H */

#ifndef _KERNEL_ARCH_AARCH64_INTR_DECODE_H
#define _KERNEL_ARCH_AARCH64_INTR_DECODE_H

// ─────────────────────────────────────────────────────────
//  arch/aarch64/intr_decode.h — aarch64 event decoder
//
//  Pure functions: they turn the raw state of one aarch64
//  exception (ESR / FAR snapshot) or one interrupt into a
//  normalized intr_event (spec §5.1 / §6.2).  They read no system
//  register, execute no handler, allocate nothing, take no lock
//  and log nothing.
//
//  The decoders belong to the early-reachable call chain, so they
//  must not depend on the stack canary.
// ─────────────────────────────────────────────────────────

#include <stdint.h>

#include <intr/dispatch.h>

/* VBAR slot offsets of the EL1h vectors (16 slots, 128-byte
 * stride): the native vector of an aarch64 event is the slot it
 * was taken through (spec §5.1). */
#define AARCH64_EL1H_SYNC_SLOT 0x200ULL
#define AARCH64_EL1H_IRQ_SLOT  0x280ULL

/* `regs` must not be NULL; `out` receives a fully initialized event
 * (padding is not initialized). */
__attribute__((no_stack_protector))
void aarch64_intr_decode_sync(struct intr_event *out, const pt_regs_t *regs,
                              uint64_t esr, uint64_t far);

__attribute__((no_stack_protector))
void aarch64_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs);

#endif /* _KERNEL_ARCH_AARCH64_INTR_DECODE_H */

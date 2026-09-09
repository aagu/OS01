#ifndef _KERNEL_ARCH_AARCH64_REGS_H
#define _KERNEL_ARCH_AARCH64_REGS_H

// ─────────────────────────────────────────────────────────
//  aarch64 CPU Register Definitions — pt_regs_t layout
//
//  Section 1 (#ifndef __ASSEMBLER__):  C-only  (structs, typedefs)
//  Section 2 (unconditionally):       shared  (#define, usable in .S)
// ─────────────────────────────────────────────────────────

#ifndef __ASSEMBLER__
#include <stdint.h>

// ── pt_regs_t: exception/interrupt stack frame ──────────
// Layout pushed by the aarch64 exception vector handler
// (kernel/arch/aarch64/entry.S). x0-x29 (30 GPRs), plus
// SP_EL0, ELR_EL1, SPSR_EL1. Must match the push order in entry.S.

typedef struct pt_regs
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
} pt_regs_t;

#endif /* !__ASSEMBLER__ */

#endif /* _KERNEL_ARCH_AARCH64_REGS_H */

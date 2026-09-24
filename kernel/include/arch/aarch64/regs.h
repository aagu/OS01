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
// (kernel/arch/aarch64/entry.S). x0-x30 (31 GPRs), plus
// SP_EL0, ELR_EL1, SPSR_EL1. Must match the push order in entry.S.
// 31 GPR × 8 + 3 sysreg × 8 = 272 bytes (34 slots × 8).
//
// x30 (LR) is included so a C handler (or entry.S epilogue)
// can preserve the caller's link register across the IRQ
// trampoline — required for proper AAPCS64 fidelity when
// the IRQ is taken from C code that did a `bl` immediately
// before `wfi`.

typedef struct pt_regs
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
} pt_regs_t;

// Multi-layer defense (R5): if anyone reorders fields, the layout
// drifts and entry.S's STP/LDP offsets silently corrupt the frame.
// Catch it at compile time, not in the wild.
_Static_assert(sizeof(pt_regs_t) == 34 * 8,
               "pt_regs_t layout drifted from entry.S / spec §5.2");

#endif /* !__ASSEMBLER__ */

// ── pt_regs_t 偏移常量（entry.S 与 C 共享；字段序 = 入栈序，spec §5.2）──
// These are usable from .S via `#include <arch/aarch64/regs.h>`
// (kernel/Makefile .S rule走 C 预处理器, kernel/Makefile:222-224).
#define PT_REGS_X0         (0  * 8)
#define PT_REGS_X2         (2  * 8)
#define PT_REGS_X4         (4  * 8)
#define PT_REGS_X6         (6  * 8)
#define PT_REGS_X8         (8  * 8)
#define PT_REGS_X10        (10 * 8)
#define PT_REGS_X12        (12 * 8)
#define PT_REGS_X14        (14 * 8)
#define PT_REGS_X16        (16 * 8)
#define PT_REGS_X18        (18 * 8)
#define PT_REGS_X20        (20 * 8)
#define PT_REGS_X22        (22 * 8)
#define PT_REGS_X24        (24 * 8)
#define PT_REGS_X26        (26 * 8)
#define PT_REGS_X28        (28 * 8)
#define PT_REGS_X30        (30 * 8)
#define PT_REGS_SP_EL0     (31 * 8)
#define PT_REGS_ELR_EL1    (32 * 8)
#define PT_REGS_SPSR_EL1   (33 * 8)
#define PT_REGS_SIZE       (34 * 8)

/* ID_AA64ISAR0_EL1.RNDR 字段（bits[63:60]）：
 *   0b0000 = 不支持 RNDR/RNDRRS
 *   0b0001 = 支持 RNDR + RNDRRS
 *   0b0010 = 支持 RNDR（IMPL_DEF RNDRRS 处理）
 *   ≥0b0011 = 保留
 * 实施者用：((mrs ID_AA64ISAR0_EL1) >> 60) & 0xF
 */
#define ID_AA64ISAR0_EL1_RNDR_SHIFT  60
#define ID_AA64ISAR0_EL1_RNDR_MASK   (0xFUL << ID_AA64ISAR0_EL1_RNDR_SHIFT)
#define ID_AA64ISAR0_EL1_RNDR_RNDRRS (1UL << ID_AA64ISAR0_EL1_RNDR_SHIFT)

#endif /* _KERNEL_ARCH_AARCH64_REGS_H */

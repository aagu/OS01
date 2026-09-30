/* aarch64 phase 1: EL1 vector table (VBAR_EL1 base).
 *
 * The table is 2 KiB-aligned (16 entries * 128 bytes), placed in
 * `.text` (high half).  main.c (not head.S!) installs it via
 * `msr vbar_el1, ...` immediately after PL011 init — see the
 * comment in main.c for why head.S's pre-MMU `boot_vectors`
 * install is not enough.
 *
 * Phase 1 (Task 2.2) wires the EL1h IRQ slot (entry #6) to a
 * dedicated `el1_irq_entry` trampoline which:
 *   - Saves the FULL 31 GPR + sp_el0 + elr_el1 + spsr_el1 frame
 *     onto the IRQ stack (layout matches struct pt_regs).
 *   - Calls `el1_irq(regs)` in trap.c, which delegates to the GIC
 *     driver's generic dispatch (handler table lookup + EOI).
 *   - Restores the frame and `eret`s back to the interrupted PC.
 *
 * The GIC handler table is populated by `arch_tick_start()` (CNTP)
 * and the irq_probe (SGI 2 / SPI 40), all via gic_register_handler().
 *
 * Slot numbering: the architecture orders the 16 entries by
 * exception class then source SP.  Layout (offset within the
 * 2 KiB-aligned base):
 *   +0x000  Sync  SP_EL0
 *   +0x080  IRQ   SP_EL0
 *   +0x100  FIQ   SP_EL0
 *   +0x180  SError SP_EL0
 *   +0x200  Sync  SP_ELx
 *   +0x280  IRQ   SP_ELx   ← the EL1h IRQ we dispatch
 *   +0x300  FIQ   SP_ELx
 *   +0x380  SError SP_ELx
 *   +0x400..+0x780   same 4 classes from EL0 (AArch64)
 *   +0x800..+0x980   AArch32 entries (unused; AArch64 EL0 traps
 *                    these, but we keep the table complete so
 *                    the architecture is satisfied)
 *
 * The vector table itself is fixed-size so the table address is
 * 2 KiB-aligned by construction; VBAR_EL1 accepts a 2 KiB-aligned
 * base (per ARM ARM D17.2.86).
 */

#include <arch/aarch64/regs.h>

.section .text.entry, "ax"

.balign 0x800                           /* align to 2 KiB */
.globl exception_vectors
exception_vectors:

/* ───── Synchronous exception, current SP_EL0 ───── */
.balign 0x80
1:  b   .

/* ───── IRQ, current SP_EL0 ───── */
.balign 0x80
2:  b   .

/* ───── FIQ, current SP_EL0 ───── */
.balign 0x80
3:  b   .

/* ───── SError, current SP_EL0 ───── */
.balign 0x80
4:  b   .

/* ───── Synchronous exception, current SP_ELx ─────
 *
 * Each slot must be ≤0x80 bytes (the architecture enforces 128-byte
 * stride between entries).  This slot therefore only branches; the
 * full save + diagnostic call happens in `el1_sync_entry` declared
 * below the table (mirrors the IRQ-slot pattern). */
.balign 0x80
5:
    b   el1_sync_entry

/* ───── IRQ, current SP_ELx (this is what kernel uses at EL1h) ─────
 *
 * Each slot must be ≤0x80 bytes (the architecture enforces 128-byte
 * stride between entries).  This slot therefore only branches; the
 * full save/restore happens in `el1_irq_entry` declared below the
 * table. */
.balign 0x80
6:
    b   el1_irq_entry

/* ───── FIQ, current SP_ELx ───── */
.balign 0x80
7:  b   .

/* ───── SError, current SP_ELx ───── */
.balign 0x80
8:  b   .

/* ───── Synchronous exception, EL0 (AArch64) ───── */
.balign 0x80
9:  b   .

/* ───── IRQ, EL0 ───── */
.balign 0x80
10: b   .

/* ───── FIQ, EL0 ───── */
.balign 0x80
11: b   .

/* ───── SError, EL0 ───── */
.balign 0x80
12: b   .

/* ───── AArch32 entries (unused; EL0 in AArch64 mode traps them) ───── */
.balign 0x80
13: b   .

.balign 0x80
14: b   .

.balign 0x80
15: b   .

.balign 0x80
16: b   .

/* ──────────────────────────────────────────────────────────────
 * el1_irq_entry — EL1h IRQ 通用入口（spec §4.2/§5）
 *
 * 入栈序 = pt_regs_t 字段序 = regs.h PT_REGS_* 偏移。
 * 全量保存 31 GPR + sp_el0 + elr + spsr，布局 = 34 槽 × 8 = 272 字节。
 * bl 到 el1_irq(struct pt_regs *)，x0 = sp（指向刚压好的帧）。
 * eret 前完整恢复。
 * ────────────────────────────────────────────────────────────── */
.balign 0x80
.globl el1_irq_entry
el1_irq_entry:
    sub     sp, sp, #PT_REGS_SIZE
    stp     x0, x1,   [sp, #PT_REGS_X0]
    stp     x2, x3,   [sp, #PT_REGS_X2]
    stp     x4, x5,   [sp, #PT_REGS_X4]
    stp     x6, x7,   [sp, #PT_REGS_X6]
    stp     x8, x9,   [sp, #PT_REGS_X8]
    stp     x10, x11, [sp, #PT_REGS_X10]
    stp     x12, x13, [sp, #PT_REGS_X12]
    stp     x14, x15, [sp, #PT_REGS_X14]
    stp     x16, x17, [sp, #PT_REGS_X16]
    stp     x18, x19, [sp, #PT_REGS_X18]
    stp     x20, x21, [sp, #PT_REGS_X20]
    stp     x22, x23, [sp, #PT_REGS_X22]
    stp     x24, x25, [sp, #PT_REGS_X24]
    stp     x26, x27, [sp, #PT_REGS_X26]
    stp     x28, x29, [sp, #PT_REGS_X28]
    str     x30,      [sp, #PT_REGS_X30]
    mrs     x1, spsr_el1
    str     x1,       [sp, #PT_REGS_SPSR_EL1]
    mrs     x1, elr_el1
    str     x1,       [sp, #PT_REGS_ELR_EL1]
    mrs     x1, sp_el0
    str     x1,       [sp, #PT_REGS_SP_EL0]
    mov     x0, sp                       /* x0 = &regs for el1_irq */
    bl      el1_irq
    ldr     x1,       [sp, #PT_REGS_SP_EL0]
    msr     sp_el0, x1
    ldr     x1,       [sp, #PT_REGS_ELR_EL1]
    msr     elr_el1, x1
    ldr     x1,       [sp, #PT_REGS_SPSR_EL1]
    msr     spsr_el1, x1
    ldr     x30,      [sp, #PT_REGS_X30]
    ldp     x28, x29, [sp, #PT_REGS_X28]
    ldp     x26, x27, [sp, #PT_REGS_X26]
    ldp     x24, x25, [sp, #PT_REGS_X24]
    ldp     x22, x23, [sp, #PT_REGS_X22]
    ldp     x20, x21, [sp, #PT_REGS_X20]
    ldp     x18, x19, [sp, #PT_REGS_X18]
    ldp     x16, x17, [sp, #PT_REGS_X16]
    ldp     x14, x15, [sp, #PT_REGS_X14]
    ldp     x12, x13, [sp, #PT_REGS_X12]
    ldp     x10, x11, [sp, #PT_REGS_X10]
    ldp     x8, x9,   [sp, #PT_REGS_X8]
    ldp     x6, x7,   [sp, #PT_REGS_X6]
    ldp     x4, x5,   [sp, #PT_REGS_X4]
    ldp     x2, x3,   [sp, #PT_REGS_X2]
    ldp     x0, x1,   [sp, #PT_REGS_X0]
    add     sp, sp, #PT_REGS_SIZE
    eret

/* ──────────────────────────────────────────────────────────────
 * el1_sync_entry — EL1h sync 异常诊断入口（spec §3, Task 1）
 *
 * 现场保存序与 el1_irq_entry 一致：pt_regs_t 字段序 = 入栈序 =
 * regs.h PT_REGS_* 偏移。全量保存 31 GPR 后才快照 SPSR_EL1/ELR_EL1/
 * SP_EL0/ESR_EL1/FAR_EL1，避免后续 MRS 覆盖原始 x0–x30。
 *
 * AAPCS64 参数：(x0=sp → &regs, x1=esr, x2=far)。ESR_EL1/FAR_EL1 在
 * 调用 C 前用 caller-saved 通用寄存器暂存，再分别送入 x1/x2。
 *
 * aarch64_el1_sync_fatal 是 noreturn；汇编后置不可返回的 WFI 兜底，
 * 防止编译器/ABI 失误意外 `eret`。
 * ────────────────────────────────────────────────────────────── */
.balign 0x80
.globl el1_sync_entry
el1_sync_entry:
    sub     sp, sp, #PT_REGS_SIZE
    stp     x0, x1,   [sp, #PT_REGS_X0]
    stp     x2, x3,   [sp, #PT_REGS_X2]
    stp     x4, x5,   [sp, #PT_REGS_X4]
    stp     x6, x7,   [sp, #PT_REGS_X6]
    stp     x8, x9,   [sp, #PT_REGS_X8]
    stp     x10, x11, [sp, #PT_REGS_X10]
    stp     x12, x13, [sp, #PT_REGS_X12]
    stp     x14, x15, [sp, #PT_REGS_X14]
    stp     x16, x17, [sp, #PT_REGS_X16]
    stp     x18, x19, [sp, #PT_REGS_X18]
    stp     x20, x21, [sp, #PT_REGS_X20]
    stp     x22, x23, [sp, #PT_REGS_X22]
    stp     x24, x25, [sp, #PT_REGS_X24]
    stp     x26, x27, [sp, #PT_REGS_X26]
    stp     x28, x29, [sp, #PT_REGS_X28]
    str     x30,      [sp, #PT_REGS_X30]
    /* Snapshot 系统寄存器到 PT_REGS_* 槽位。x3 是 caller-saved；此
     * 时 x0-x30 已压栈，临时覆盖 x3 不影响异常现场。 */
    mrs     x3, spsr_el1
    str     x3,       [sp, #PT_REGS_SPSR_EL1]
    mrs     x3, elr_el1
    str     x3,       [sp, #PT_REGS_ELR_EL1]
    mrs     x3, sp_el0
    str     x3,       [sp, #PT_REGS_SP_EL0]
    /* 再次读取 ESR/FAR 到 caller-saved x1/x2 直接做参数；
     * 已有 PT_REGS 现场保留 SPSR/ELR/SP_EL0 三个槽供 C 解码。 */
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mov     x0, sp                       /* x0 = &regs */
    bl      aarch64_el1_sync_fatal
    /* noreturn fallback：禁用中断 + 永久 WFI，杜绝 eret。 */
0:  msr     daifset, #0xf
    wfi
    b       0b

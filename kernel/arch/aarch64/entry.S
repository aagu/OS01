/* aarch64 phase 1: EL1 vector table (VBAR_EL1 base).
 *
 * The table is 2 KiB-aligned (16 entries * 128 bytes), placed in
 * `.text` (high half).  main.c (not head.S!) installs it via
 * `msr vbar_el1, ...` immediately after PL011 init — see the
 * comment in main.c for why head.S's pre-MMU `boot_vectors`
 * install is not enough.
 *
 * Phase 1 (Task 3) wires up the EL1h IRQ slot (entry #5) to the
 * C-side dispatch (el1_irq_dispatch) which reads GICC_IAR, re-arms
 * CNTP_TVAL_EL0, and EOI's the GIC.  Every other slot still spins
 * — we don't expect to see any of them fire in phase 1.
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

/* ───── Synchronous exception, current SP_ELx ───── */
.balign 0x80
5:  b   .

/* ───── IRQ, current SP_ELx (this is what kernel uses at EL1h) ─────
 *
 * The kernel runs at EL1 with SP_EL1; on IRQ entry the CPU jumps
 * here with the same SP_EL1 (the architecture saves PSTATE+ELR
 * into the per-bank save area).  We don't need to save any
 * additional state in phase 1 (no syscall ABI, no userspace).
 *
 * Dispatch:
 *   1. Branch to the C handler (bl el1_irq_dispatch).
 *   2. The C handler reads GICC_IAR, re-arms CNTP, EOI's the GIC,
 *      and prints a tick counter.
 *   3. eret returns to the wfi in aarch64_main.
 *
 * The bl clobbers LR; that's fine because eret reads ELR_EL1,
 * which the architecture keeps across the IRQ entry. */
.balign 0x80
6:
    bl  el1_irq_dispatch
    eret

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

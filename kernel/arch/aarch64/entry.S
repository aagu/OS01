/* aarch64 phase 1: EL1 vector table (VBAR_EL1 base).
 *
 * The table is 2 KiB-aligned (16 entries * 128 bytes), placed in
 * `.text` (high half).  head.S installs it via `msr vbar_el1, ...`
 * immediately AFTER MMU is enabled and we have jumped high.
 *
 * Phase 1 only needs EL1h IRQ routing (Task 3 will fill GIC dispatch);
 * everything else is a sync spinloop.  Sync/FIQ/SError from EL1 all
 * spin: spec says "b ."; they should never fire before Task 3.
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

/* ───── IRQ, current SP_ELx (this is what kernel uses at EL1h) ───── */
.balign 0x80
6:  /* Phase 1 stub.  Once GIC is wired (Task 3) we read ICCIAR_EL1,
      * dispatch to the GIC handler, and acknowledge via ICCEOIR_EL1.
      * For now: spin.  IRQ remains masked at this point in boot. */
    b   6b

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

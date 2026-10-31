#include <arch/aarch64/context.h>

#ifdef OS01_SELFTEST

.text
.align 2
.global aarch64_sched_reg_probe
.type aarch64_sched_reg_probe, %function
/* x0 is a per-thread pattern. Return bit 0 for callee-saved register
 * corruption, bit 1 for local canary corruption, bit 2 for SP drift. */
aarch64_sched_reg_probe:
    sub sp, sp, #144
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x0, [sp, #96]
    mov x9, sp
    str x9, [sp, #104]
    mov x9, x0
    mov x10, #0x5a5a
    eor x9, x9, x10
    str x9, [sp, #112]
    mvn x9, x9
    str x9, [sp, #120]
    mov x9, #16
    str x9, [sp, #128]

    ldr x19, [sp, #96]
    add x20, x19, #1
    add x21, x19, #2
    add x22, x19, #3
    add x23, x19, #4
    add x24, x19, #5
    add x25, x19, #6
    add x26, x19, #7
    add x27, x19, #8
    add x28, x19, #9
    add x29, x19, #10
.Lregs_yield:
    bl kthread_yield

    mov w0, wzr
    mov x9, sp
    ldr x10, [sp, #104]
    cmp x9, x10
    cset w13, ne
    orr w0, w0, w13, lsl #2
    ldr x9, [sp, #112]
    ldr x10, [sp, #96]
    mov x11, #0x5a5a
    eor x10, x10, x11
    cmp x9, x10
    cset w12, ne
    orr w0, w0, w12, lsl #1
    ldr x9, [sp, #120]
    mvn x10, x10
    cmp x9, x10
    cset w12, ne
    orr w0, w0, w12, lsl #1

    ldr x10, [sp, #96]
    cmp x19, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x20, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x21, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x22, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x23, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x24, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x25, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x26, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x27, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x28, x10
    b.ne .Lregs_bad
    add x10, x10, #1
    cmp x29, x10
    b.ne .Lregs_bad
    cbnz w0, .Lregs_restore
    ldr x9, [sp, #128]
    subs x9, x9, #1
    str x9, [sp, #128]
    b.ne .Lregs_yield
    b .Lregs_restore
.Lregs_bad:
    orr w0, w0, #1
.Lregs_restore:
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #144
    ret
.size aarch64_sched_reg_probe, .-aarch64_sched_reg_probe

.align 2
.global aarch64_sched_timer_irq_probe
.type aarch64_sched_timer_irq_probe, %function
/* x0=target scheduler-test CNTP count, x1=CNTP deadline in cntvct ticks.
 * Caller-saved sentinels remain live across a real timer IRQ; this checks
 * the actual entry.S save/restore path, not just the C handler's frame. */
aarch64_sched_timer_irq_probe:
    sub sp, sp, #128
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x0, [sp, #96]
    str x1, [sp, #104]
    mov x9, sp
    str x9, [sp, #112]
    mov x0, #0x1111
    mov x1, #0x2222
    mov x2, #0x3333
    mov x3, #0x4444
    mov x4, #0x5555
    mov x5, #0x6666
    mov x18, #0x7777
.Ltimer_wait:
    adrp x9, aarch64_sched_timer_tick_count
    add x9, x9, :lo12:aarch64_sched_timer_tick_count
    ldar x10, [x9]
    ldr x11, [sp, #96]
    cmp x10, x11
    b.hs .Ltimer_check
    mrs x10, cntvct_el0
    ldr x11, [sp, #104]
    cmp x10, x11
    b.hs .Ltimer_timeout
    wfi
    b .Ltimer_wait
.Ltimer_check:
    mov w12, wzr
    mov x6, #0x1111
    cmp x0, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x2222
    cmp x1, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x3333
    cmp x2, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x4444
    cmp x3, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x5555
    cmp x4, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x6666
    cmp x5, x6
    b.ne .Ltimer_regs_bad
    mov x6, #0x7777
    cmp x18, x6
    b.ne .Ltimer_regs_bad
    mov x9, sp
    ldr x10, [sp, #112]
    cmp x9, x10
    b.eq .Ltimer_restore
    mov w12, #2
    b .Ltimer_restore
.Ltimer_regs_bad:
    mov w12, #1
    b .Ltimer_restore
.Ltimer_timeout:
    mov w12, #4
.Ltimer_restore:
    mov w0, w12
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #128
    ret
.size aarch64_sched_timer_irq_probe, .-aarch64_sched_timer_irq_probe
#endif

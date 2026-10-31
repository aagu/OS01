#include <arch/aarch64/context.h>

.text
.align 2
.global aarch64_switch_context
.type aarch64_switch_context, %function
/* x0=actual_prev, x1=prev thread_t, x2=next thread_t. */
aarch64_switch_context:
    stp x19, x20, [x1, #(0 * 8)]
    stp x21, x22, [x1, #(2 * 8)]
    stp x23, x24, [x1, #(4 * 8)]
    stp x25, x26, [x1, #(6 * 8)]
    stp x27, x28, [x1, #(8 * 8)]
    str x29, [x1, #(10 * 8)]
    str x30, [x1, #AARCH64_THREAD_X30_OFF]
    mov x3, sp
    str x3, [x1, #AARCH64_THREAD_SP_OFF]

    ldp x19, x20, [x2, #(0 * 8)]
    ldp x21, x22, [x2, #(2 * 8)]
    ldp x23, x24, [x2, #(4 * 8)]
    ldp x25, x26, [x2, #(6 * 8)]
    ldp x27, x28, [x2, #(8 * 8)]
    ldr x29, [x2, #(10 * 8)]
    ldr x30, [x2, #AARCH64_THREAD_X30_OFF]
    ldr x4, [x2, #AARCH64_THREAD_SP_OFF]
    mov sp, x4

    /* Validate the installed stack before the first C instruction on it. */
    mov x9, sp
    and x5, x9, #-(AARCH64_STACK_SIZE)
    ldr x6, [x2, #AARCH64_THREAD_STACK_BASE_OFF]
    cmp x5, x6
    b.ne .Lbad_next
    tst x9, #15
    b.ne .Lbad_next
    ldr x7, [x2, #AARCH64_THREAD_USABLE_LOW_OFF]
    cmp sp, x7
    b.lo .Lbad_next
    sub x8, sp, #16
    cmp x8, x7
    b.lo .Lbad_next
    ldr x7, [x2, #AARCH64_THREAD_STACK_TOP_OFF]
    cmp sp, x7
    b.hs .Lbad_next

    /* Run the common tail on next's stack, preserving its continuation. */
    sub sp, sp, #16
    str x30, [sp]
    bl task_switch_complete
    ldr x30, [sp]
    add sp, sp, #16
    ret

.Lbad_next:
    mov x9, sp
    ldr x10, [x2, #AARCH64_THREAD_STACK_BASE_OFF]
    ldr x11, [x2, #AARCH64_THREAD_STACK_TOP_OFF]
    mov x0, #1
    mov x1, x9
    mov x2, x10
    mov x3, x11
    b aarch64_context_fatal_asm
.size aarch64_switch_context, .-aarch64_switch_context

#include <arch/aarch64/context.h>

.text
.align 2
.global arch_aarch64_kthread_entry
.type arch_aarch64_kthread_entry, %function
/* Initial context: x19=fn, x20=arg, x21=thread_t, SP=top-16. */
arch_aarch64_kthread_entry:
    mov x8, sp
    and x3, x8, #-(AARCH64_STACK_SIZE)
    ldr x4, [x21, #AARCH64_THREAD_STACK_BASE_OFF]
    cmp x3, x4
    b.ne .Lentry_bad
    tst x8, #15
    b.ne .Lentry_bad
    ldr x5, [x21, #AARCH64_THREAD_USABLE_LOW_OFF]
    cmp sp, x5
    b.lo .Lentry_bad
    ldr x6, [x21, #AARCH64_THREAD_STACK_TOP_OFF]
    cmp sp, x6
    b.hs .Lentry_bad
    sub x7, sp, #16
    cmp x7, x5
    b.lo .Lentry_bad

    bl arch_aarch64_entry_prepare
    mov x0, x20
    msr daifclr, #2
    blr x19
    bl kthread_exit
    brk #0
.Lentry_bad:
    mov x9, sp
    ldr x10, [x21, #AARCH64_THREAD_STACK_BASE_OFF]
    ldr x11, [x21, #AARCH64_THREAD_STACK_TOP_OFF]
    mov x0, #2
    mov x1, x9
    mov x2, x10
    mov x3, x11
    b aarch64_context_fatal_asm
.size arch_aarch64_kthread_entry, .-arch_aarch64_kthread_entry

.align 2
.global aarch64_bsp_stack_enter
.type aarch64_bsp_stack_enter, %function
/* x0 is the precomputed idle top-16. */
aarch64_bsp_stack_enter:
    mov sp, x0
    mov x8, sp
    and x3, x8, #-(AARCH64_STACK_SIZE)
    adrp x4, init_thread
    add x4, x4, :lo12:init_thread
    ldr x5, [x4, #AARCH64_THREAD_STACK_BASE_OFF]
    cmp x3, x5
    b.ne .Lbsp_bad
    tst x8, #15
    b.ne .Lbsp_bad
    ldr x6, [x4, #AARCH64_THREAD_USABLE_LOW_OFF]
    cmp sp, x6
    b.lo .Lbsp_bad
    ldr x7, [x4, #AARCH64_THREAD_STACK_TOP_OFF]
    cmp sp, x7
    b.hs .Lbsp_bad
    sub x8, sp, #16
    cmp x8, x6
    b.lo .Lbsp_bad

    mov x29, xzr
    b arch_aarch64_bsp_handoff
.Lbsp_bad:
    mov x9, sp
    adrp x10, init_thread
    add x10, x10, :lo12:init_thread
    ldr x2, [x10, #AARCH64_THREAD_STACK_BASE_OFF]
    ldr x3, [x10, #AARCH64_THREAD_STACK_TOP_OFF]
    mov x0, #3
    mov x1, x9
    b aarch64_context_fatal_asm
.size aarch64_bsp_stack_enter, .-aarch64_bsp_stack_enter

/* Invalid pre-C stack paths switch to a dedicated high-half emergency
 * stack before entering the logger.  No current accessor is involved. */
.align 2
.global aarch64_context_fatal_asm
.type aarch64_context_fatal_asm, %function
aarch64_context_fatal_asm:
    msr daifset, #2
    adrp x9, aarch64_context_fatal_stack_top
    add x9, x9, :lo12:aarch64_context_fatal_stack_top
    mov sp, x9
    bl aarch64_context_fatal
    brk #0
.size aarch64_context_fatal_asm, .-aarch64_context_fatal_asm

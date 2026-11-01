#include <arch/aarch64/context.h>
#ifdef OS01_SELFTEST
#include <selftest/aarch64_scheduler.h>
#include <arch/aarch64/boot_offsets.h>
#endif

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

#ifdef OS01_SELFTEST
    /* Preserve the real pre-C stack observation from the first-entry
     * assembly boundary in THIS CPU's record slot.  The CPU index is
     * read through TPIDR_EL1 + PERCPU_CPU_ID_OFFSET — never through
     * `current`, which is not valid until arch_aarch64_entry_prepare.
     * Only scratch registers (x9-x13) are used: x19/x20/x21 carry the
     * first-entry parameters and must survive untouched. */
    mrs x11, tpidr_el1
    ldr w12, [x11, #PERCPU_CPU_ID_OFFSET]
    adrp x9, aarch64_sched_entry_records
    add x9, x9, :lo12:aarch64_sched_entry_records
    mov x13, #AARCH64_SCHED_ENTRY_RECORD_SIZE
    madd x9, x12, x13, x9
    str x8, [x9, #AARCH64_SCHED_ENTRY_RECORD_SP_OFF]
    str x3, [x9, #AARCH64_SCHED_ENTRY_RECORD_BASE_OFF]
    mov x10, #1
    str x10, [x9, #AARCH64_SCHED_ENTRY_RECORD_OK_OFF]
#endif

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

/* ── AP cooperative-scheduler trampoline (P1 spec §3.2) ─────────────
 * x0 = new sp, x1 = thread_t* ctx, x2 = logical cpu.  Every spec stack
 * boundary is validated BEFORE the SP is installed; on any mismatch the
 * AP takes the (no-stack) fatal path instead of switching.  The old AP
 * work stack is abandoned on purpose — it is a boot stack, not owned by
 * the scheduler. */
.align 2
.global aarch64_ap_stack_enter
.type aarch64_ap_stack_enter, %function
aarch64_ap_stack_enter:
    tst x0, #15
    b.ne .Lap_bad
    and x9, x0, #-(AARCH64_STACK_SIZE)
    ldr x10, [x1, #AARCH64_THREAD_STACK_BASE_OFF]
    cmp x9, x10
    b.ne .Lap_bad
    ldr x11, [x1, #AARCH64_THREAD_STACK_TOP_OFF]
    add x12, x9, #AARCH64_STACK_SIZE
    cmp x11, x12
    b.ne .Lap_bad
    ldr x13, [x1, #AARCH64_THREAD_USABLE_LOW_OFF]
    cmp x0, x13
    b.lo .Lap_bad
    sub x9, x0, #16
    cmp x9, x13
    b.lo .Lap_bad
    mov sp, x0
    mov w0, w2
    b aarch64_ap_idle_handoff
.Lap_bad:
    mov x0, #13
    mov x1, sp
    /* x2/x3: the base/top the trampoline derived for the diagnostic */
    and x2, x1, #-(AARCH64_STACK_SIZE)
    add x3, x2, #AARCH64_STACK_SIZE
    b aarch64_context_fatal_asm
.size aarch64_ap_stack_enter, .-aarch64_ap_stack_enter

/* Invalid pre-C stack paths switch to a dedicated high-half emergency
 * stack before entering the logger.  No current accessor is involved.
 * Since P1 (spec §6) the shared emergency stack is installed by ONE
 * CPU only: an ldaxr/stxr arbitration on a BSS word runs BEFORE any
 * stack is installed; losing CPUs mask IRQs and park stackless. */
.align 2
.global aarch64_context_fatal_asm
.type aarch64_context_fatal_asm, %function
aarch64_context_fatal_asm:
    msr daifset, #2
#ifdef AARCH64_AP_SCHED_TEST_FATAL_OWNER
    /* fatal-owner variant: record this CPU's arrival BEFORE the
       arbitration (cpu via TPIDR_EL1 — no current exists here).  The
       arrivals word is separate from the ownership word. */
    adrp x11, aarch64_fatal_arrivals_word
    add x11, x11, :lo12:aarch64_fatal_arrivals_word
    mrs x12, tpidr_el1
    ldr w13, [x12, #PERCPU_CPU_ID_OFFSET]
    mov w10, #1
    lsl w10, w10, w13
.Lfatal_arrive:
    ldaxr w9, [x11]
    orr w9, w9, w10
    stlxr w13, w9, [x11]
    cbnz w13, .Lfatal_arrive
#endif
    adrp x9, aarch64_fatal_owner_word
    add x9, x9, :lo12:aarch64_fatal_owner_word
    mov w10, #1
.Lfatal_claim:
    ldaxr w11, [x9]
    cbnz w11, .Lfatal_lose
    stxr w12, w10, [x9]
    cbnz w12, .Lfatal_claim
    b .Lfatal_owner
.Lfatal_lose:
#ifdef AARCH64_AP_SCHED_TEST_FATAL_OWNER
    adrp x11, aarch64_fatal_losers_word
    add x11, x11, :lo12:aarch64_fatal_losers_word
.Lfatal_lose_count:
    ldxr w12, [x11]
    add w12, w12, #1
    stxr w13, w12, [x11]
    cbnz w13, .Lfatal_lose_count
#endif
    /* Park stackless with IRQs masked; the owner never releases. */
.Lfatal_park:
    wfe
    b .Lfatal_park
.Lfatal_owner:
    adrp x9, aarch64_context_fatal_stack_top
    add x9, x9, :lo12:aarch64_context_fatal_stack_top
    mov sp, x9
#ifdef AARCH64_AP_SCHED_TEST_FATAL_OWNER
    bl aarch64_ap_fatal_owner_enter
    brk #0
#endif
    bl aarch64_context_fatal
    brk #0
.size aarch64_context_fatal_asm, .-aarch64_context_fatal_asm

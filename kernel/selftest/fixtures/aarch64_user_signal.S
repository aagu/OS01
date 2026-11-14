/* kernel/selftest/fixtures/aarch64_user_signal.S
 *
 * M4.14 AArch64 native signal + sigreturn EL0 acceptance fixture.
 *
 * Self-contained single ELF image (/bin/user_signal):
 *   - argc == 2 (post-exec child): sends SIGUSR1 to self; since exec
 *     reset sighand[SIGUSR1] to SIG_DFL, the child is terminated with
 *     exit_code == SIGUSR1 (10).
 *   - argc == 0 (initial parent):
 *     1. Negative checks:
 *        - SYS_signal with kernel-space sa_handler -> -EINVAL (-22)
 *        - SYS_sigreturn with forged EL1h spsr_el1=0x5 -> -EINVAL (-22)
 *        - SYS_sigreturn with unaligned sp_el0 -> -EFAULT (-14)
 *     2. sigprocmask(SIG_BLOCK) -> SYS_kill(self, SIGUSR1) stays pending;
 *        sigprocmask(SIG_UNBLOCK) delivers SIGUSR1 to sigusr1_handler,
 *        which verifies x0==10 and 16B sp alignment, increments g_sig_count,
 *        rewrites saved frame.regs.x19 to 0x13572468, and returns to
 *        sigreturn_restorer (SYS_sigreturn). Main resumes with x0==0,
 *        g_sig_count==1, and x19==0x13572468.
 *     3. SYS_fork() -> child calls SYS_exec("/bin/user_signal", argv, NULL)
 *        with argc==2 -> child killed by SIGUSR1 -> parent waitpid() sees
 *        status == 10.
 *     4. SYS_exit(0x66).
 */

#include <uapi/syscall.h>

#define SIGUSR1_NUM   10
#define SIG_BLOCK_OP  0
#define SIG_UNBLOCK_OP 1

.macro US_EXIT code
    mov     x0, #\code
    mov     x8, #SYS_exit
    svc     #0
.endm

.section .text
.global _start
.type _start, @function
_start:
    cmp     x0, #2
    b.eq    post_exec_child

    /* ── 1. Negative check: kernel-space sa_handler -> -EINVAL (-22) ── */
    sub     sp, sp, #32
    mov     x9, #0xffff800000000000
    adrp    x10, sigreturn_restorer
    add     x10, x10, :lo12:sigreturn_restorer
    stp     x9, xzr, [sp, #0]       /* sa_handler, sa_flags=0 */
    stp     x10, xzr, [sp, #16]     /* sa_restorer, sa_mask=0 */

    mov     x0, #SIGUSR1_NUM
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_signal
    svc     #0
    add     sp, sp, #32
    cmn     x0, #22                 /* -EINVAL */
    b.eq    1f
    US_EXIT 0x01
1:

    /* ── 2. Negative check: forged EL1h spsr_el1=0x5 -> -EINVAL (-22) ── */
    sub     sp, sp, #288            /* >= 280B struct aarch64_sigframe, 16B aligned */
    mov     x9, #0
2:
    str     xzr, [sp, x9]
    add     x9, x9, #8
    cmp     x9, #288
    b.lt    2b

    mov     x9, #0x5                /* EL1h */
    str     x9, [sp, #264]          /* frame.regs.spsr_el1 */
    mov     x8, #SYS_sigreturn
    svc     #0
    add     sp, sp, #288
    cmn     x0, #22                 /* -EINVAL */
    b.eq    3f
    US_EXIT 0x02
3:

    /* ── 3. Negative check: unaligned sp_el0 -> -EFAULT (-14) ── */
    sub     sp, sp, #8
    mov     x8, #SYS_sigreturn
    svc     #0
    add     sp, sp, #8
    cmn     x0, #14                 /* -EFAULT */
    b.eq    4f
    US_EXIT 0x03
4:

    /* ── 4. Install SIGUSR1 handler + restorer ── */
    sub     sp, sp, #32
    adrp    x9, sigusr1_handler
    add     x9, x9, :lo12:sigusr1_handler
    adrp    x10, sigreturn_restorer
    add     x10, x10, :lo12:sigreturn_restorer
    stp     x9, xzr, [sp, #0]
    stp     x10, xzr, [sp, #16]

    mov     x0, #SIGUSR1_NUM
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_signal
    svc     #0
    add     sp, sp, #32
    cmp     x0, #0
    b.eq    5f
    US_EXIT 0x04
5:

    /* ── 5. sigprocmask(SIG_BLOCK, &usr1_mask, NULL) ── */
    sub     sp, sp, #16
    mov     x9, #(1 << (SIGUSR1_NUM - 1))   /* 0x200 */
    str     x9, [sp, #0]
    str     xzr, [sp, #8]

    mov     x0, #SIG_BLOCK_OP
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_sigprocmask
    svc     #0
    cmp     x0, #0
    b.eq    6f
    US_EXIT 0x05
6:

    /* ── 6. SYS_kill(getpid(), SIGUSR1) while blocked ── */
    mov     x8, #SYS_getpid
    svc     #0
    mov     x1, #SIGUSR1_NUM
    mov     x8, #SYS_kill
    svc     #0
    cmp     x0, #0
    b.eq    7f
    US_EXIT 0x06
7:
    adrp    x9, g_sig_count
    add     x9, x9, :lo12:g_sig_count
    ldr     x10, [x9]
    cmp     x10, #0
    b.eq    8f
    US_EXIT 0x07
8:

    /* ── 7. Set x19 = 0xdeadbeef, unblock SIGUSR1 -> handler runs ── */
    movz    x19, #0xbeef
    movk    x19, #0xdead, lsl #16

    mov     x9, #(1 << (SIGUSR1_NUM - 1))
    str     x9, [sp, #0]
    mov     x0, #SIG_UNBLOCK_OP
    mov     x1, sp                  /* &usr1_mask */
    add     x2, sp, #8              /* &old_mask */
    mov     x8, #SYS_sigprocmask
    svc     #0

    /* Returned via SYS_sigreturn from sigusr1_handler! */
    cmp     x0, #0
    b.eq    9f
    US_EXIT 0x08
9:
    ldr     x10, [sp, #8]           /* old_mask must be 0x200 */
    add     sp, sp, #16
    cmp     x10, #(1 << (SIGUSR1_NUM - 1))
    b.eq    10f
    US_EXIT 0x09
10:
    adrp    x9, g_sig_count
    add     x9, x9, :lo12:g_sig_count
    ldr     x10, [x9]
    cmp     x10, #1
    b.eq    11f
    US_EXIT 0x0a
11:
    movz    x11, #0x2468
    movk    x11, #0x1357, lsl #16
    cmp     x19, x11
    b.eq    12f
    US_EXIT 0x0b
12:

    /* ── 8. fork() + exec("/bin/user_signal", argv, NULL) + waitpid ── */
    mov     x8, #SYS_fork
    svc     #0
    cmp     x0, #0
    b.lt    fork_fail
    b.eq    fork_child

    /* Parent: waitpid(child_pid, &status, 0) */
    mov     x20, x0
    sub     sp, sp, #16
    mov     x0, x20
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x20
    b.eq    13f
    US_EXIT 0x0c
13:
    ldr     w1, [sp]
    add     sp, sp, #16
    cmp     w1, #SIGUSR1_NUM
    b.eq    14f
    US_EXIT 0x0d
14:
    US_EXIT 0x66

fork_fail:
    US_EXIT 0x0e

fork_child:
    sub     sp, sp, #32
    adrp    x9, str_self_path
    add     x9, x9, :lo12:str_self_path
    adrp    x10, str_child_arg
    add     x10, x10, :lo12:str_child_arg
    stp     x9, x10, [sp, #0]
    str     xzr, [sp, #16]

    mov     x0, x9
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_exec
    svc     #0
    US_EXIT 0xef

post_exec_child:
    mov     x8, #SYS_getpid
    svc     #0
    mov     x1, #SIGUSR1_NUM
    mov     x8, #SYS_kill
    svc     #0
    /* exec must have reset SIGUSR1 handler to SIG_DFL */
    US_EXIT 0xee

/* ── Signal handler & restorer trampoline ── */
.type sigusr1_handler, @function
sigusr1_handler:
    cmp     x0, #SIGUSR1_NUM
    b.eq    1f
    US_EXIT 0xd1
1:
    mov     x9, sp
    tst     x9, #0xf
    b.eq    2f
    US_EXIT 0xd2
2:
    adrp    x9, g_sig_count
    add     x9, x9, :lo12:g_sig_count
    ldr     x10, [x9]
    add     x10, x10, #1
    str     x10, [x9]

    /* Rewrite saved frame.regs.x19 (offset 19 * 8 = 152) to 0x13572468 */
    movz    x11, #0x2468
    movk    x11, #0x1357, lsl #16
    str     x11, [sp, #152]
    ret

.type sigreturn_restorer, @function
sigreturn_restorer:
    mov     x8, #SYS_sigreturn
    svc     #0
    US_EXIT 0xd3

.section .rodata
str_self_path:
    .asciz "/bin/user_signal"
str_child_arg:
    .asciz "child"

.section .data
.align 3
g_sig_count:
    .quad 0

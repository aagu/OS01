/* kernel/selftest/fixtures/aarch64_user_waitpid.S
 *
 * M4.12 EL0 acceptance payload for SYS_waitpid + ZOMBIE / orphan reaping.
 *
 * Stages:
 *   1. Blocking waitpid + COW parent stack write-back
 *   2. WNOHANG polling + specific PID and pid == -1 reaping
 *   3. Bad status (-EFAULT) preserves ZOMBIE + lazy anonymous page write-back + -ECHILD
 *   4. Unwaited ZOMBIE child + live orphan child (getppid() -> 0) cleanup on exit
 *
 * Exits with 0x66 on full pass, or a non-zero step code (1..39) on failure.
 */

#include <uapi/syscall.h>

#define WP_PROT_READ        1
#define WP_PROT_RW          3
#define WP_MAP_PRIVATE_ANON 0x22
#define WP_WNOHANG          1
#define WP_EFAULT           -14
#define WP_ECHILD           -10
#define WP_SVC_YIELD        0x4d40

.macro WP_LDR64 reg, val
    movz    \reg, #((\val) & 0xffff)
    movk    \reg, #(((\val) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\val) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\val) >> 48) & 0xffff), lsl #48
.endm

.macro WP_SYSCALL nr
    mov     x8, #\nr
    svc     #0
.endm

.macro WP_EXIT code
    mov     x0, #\code
    WP_SYSCALL SYS_exit
.endm

    .text
    .globl _start
_start:
    /* Reserve 16 bytes on stack before any fork() so [sp] stays on a
     * shared COW stack page until waitpid() write-back splits it. */
    sub     sp, sp, #16

    /* ── Stage 1: Blocking waitpid + COW stack write-back ──────── */
    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_1
    b.eq    .Lchild1
    mov     x19, x0                 /* x19 = child1_pid */

    /* Parent does NOT touch stack; pass sp directly to blocking waitpid. */
    mov     x0, x19
    mov     x1, sp
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, x19
    b.ne    .Lfail_2
    ldr     w9, [sp]
    mov     w10, #(0x2a << 8)
    cmp     w9, w10
    b.ne    .Lfail_3
    b       .Lstage2

.Lchild1:
    svc     #WP_SVC_YIELD
    svc     #WP_SVC_YIELD
    svc     #WP_SVC_YIELD
    WP_EXIT 0x2a

    /* ── Stage 2: WNOHANG + specific PID + pid == -1 ───────────── */
.Lstage2:
    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_4
    b.eq    .Lchild2
    mov     x20, x0                 /* x20 = child2_pid */

    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_5
    b.eq    .Lchild3
    mov     x21, x0                 /* x21 = child3_pid */

    /* child2 is still running: WNOHANG must return 0. */
    mov     x0, x20
    mov     x1, sp
    mov     x2, #WP_WNOHANG
    WP_SYSCALL SYS_waitpid
    cmp     x0, #0
    b.ne    .Lfail_6

    /* Poll child3 with WNOHANG until it exits. */
    mov     x22, #4096
.Lpoll_c3:
    svc     #WP_SVC_YIELD
    mov     x0, x21
    mov     x1, sp
    mov     x2, #WP_WNOHANG
    WP_SYSCALL SYS_waitpid
    cmp     x0, x21
    b.eq    .Lc3_reaped
    cmp     x0, #0
    b.ne    .Lfail_7
    subs    x22, x22, #1
    b.gt    .Lpoll_c3
    b       .Lfail_8

.Lc3_reaped:
    ldr     w9, [sp]
    mov     w10, #(0x22 << 8)
    cmp     w9, w10
    b.ne    .Lfail_9

    /* Reap remaining child2 using 32-bit -1 (w0 = 0xffffffff). */
    mov     w0, #-1
    mov     x1, sp
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, x20
    b.ne    .Lfail_10
    ldr     w9, [sp]
    mov     w10, #(0x11 << 8)
    cmp     w9, w10
    b.ne    .Lfail_11
    b       .Lstage3

.Lchild2:
    svc     #WP_SVC_YIELD
    svc     #WP_SVC_YIELD
    WP_EXIT 0x11

.Lchild3:
    svc     #WP_SVC_YIELD
    svc     #WP_SVC_YIELD
    WP_EXIT 0x22

    /* ── Stage 3: -EFAULT preserves ZOMBIE + lazy page + -ECHILD ─ */
.Lstage3:
    /* mmap 2 RW pages at 0x40000000: page 0 stays untouched/lazy,
     * page 1 is mprotected to PROT_READ. */
    WP_LDR64 x0, 0x40000000
    mov     x1, #0x2000
    mov     x2, #WP_PROT_RW
    mov     x3, #WP_MAP_PRIVATE_ANON
    mov     x4, #-1
    mov     x5, #0
    WP_SYSCALL SYS_mmap
    WP_LDR64 x9, 0x40000000
    cmp     x0, x9
    b.ne    .Lfail_12
    mov     x23, x0                 /* x23 = lazy RW page */

    add     x24, x23, #0x1000       /* x24 = RO page */
    mov     x0, x24
    mov     x1, #0x1000
    mov     x2, #WP_PROT_READ
    WP_SYSCALL SYS_mprotect
    cmp     x0, #0
    b.ne    .Lfail_13

    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_14
    b.eq    .Lchild4
    mov     x25, x0                 /* x25 = child4_pid */

    /* Yield so child4 exits into TASK_ZOMBIE (on_cpu == 0). */
    svc     #WP_SVC_YIELD

    /* Bad pointer 0x1 -> -EFAULT, ZOMBIE preserved. */
    mov     x0, x25
    mov     x1, #1
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, #WP_EFAULT
    b.ne    .Lfail_15

    /* Read-only page -> -EFAULT, ZOMBIE preserved. */
    mov     x0, x25
    mov     x1, x24
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, #WP_EFAULT
    b.ne    .Lfail_16

    /* Untouched lazy RW page -> succeeds and writes 0x55 << 8. */
    mov     x0, x25
    mov     x1, x23
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, x25
    b.ne    .Lfail_17
    ldr     w9, [x23]
    mov     w10, #(0x55 << 8)
    cmp     w9, w10
    b.ne    .Lfail_18

    /* Repeat waitpid(child4_pid, NULL, 0) -> -ECHILD. */
    mov     x0, x25
    mov     x1, #0
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, #WP_ECHILD
    b.ne    .Lfail_19

    /* waitpid(-1, NULL, 0) with no children -> -ECHILD. */
    mov     w0, #-1
    mov     x1, #0
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, #WP_ECHILD
    b.ne    .Lfail_20

    /* waitpid(-1, (int *)0x1, 0) with no children -> -EFAULT. */
    mov     w0, #-1
    mov     x1, #1
    mov     x2, #0
    WP_SYSCALL SYS_waitpid
    cmp     x0, #WP_EFAULT
    b.ne    .Lfail_21
    b       .Lstage4

.Lchild4:
    WP_EXIT 0x55

    /* ── Stage 4: Unwaited ZOMBIE + live orphan cleanup on exit ── */
.Lstage4:
    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_22
    b.eq    .Lchild5

    WP_SYSCALL SYS_fork
    cmp     x0, #0
    b.lt    .Lfail_23
    b.eq    .Lchild6

    /* Yield once so child5 becomes TASK_ZOMBIE before parent exits. */
    svc     #WP_SVC_YIELD
    WP_EXIT 0x66

.Lchild5:
    WP_EXIT 0x33

.Lchild6:
    mov     x22, #4096
.Lwait_orphan:
    svc     #WP_SVC_YIELD
    WP_SYSCALL SYS_getppid
    cmp     x0, #0
    b.eq    .Lorphan_ok
    subs    x22, x22, #1
    b.gt    .Lwait_orphan
    WP_EXIT 0x7f
.Lorphan_ok:
    WP_EXIT 0x44

.Lfail_1:  WP_EXIT 1
.Lfail_2:  WP_EXIT 2
.Lfail_3:  WP_EXIT 3
.Lfail_4:  WP_EXIT 4
.Lfail_5:  WP_EXIT 5
.Lfail_6:  WP_EXIT 6
.Lfail_7:  WP_EXIT 7
.Lfail_8:  WP_EXIT 8
.Lfail_9:  WP_EXIT 9
.Lfail_10: WP_EXIT 10
.Lfail_11: WP_EXIT 11
.Lfail_12: WP_EXIT 12
.Lfail_13: WP_EXIT 13
.Lfail_14: WP_EXIT 14
.Lfail_15: WP_EXIT 15
.Lfail_16: WP_EXIT 16
.Lfail_17: WP_EXIT 17
.Lfail_18: WP_EXIT 18
.Lfail_19: WP_EXIT 19
.Lfail_20: WP_EXIT 20
.Lfail_21: WP_EXIT 21
.Lfail_22: WP_EXIT 22
.Lfail_23: WP_EXIT 23

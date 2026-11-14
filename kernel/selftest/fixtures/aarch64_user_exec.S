/* kernel/selftest/fixtures/aarch64_user_exec.S
 *
 * M4.13 AArch64 SYS_exec + user stack argument marshaling fixture.
 *
 * Dual-mode assembly image selected by USER_EXEC_MODE:
 *   0: parent/child launcher (error rollbacks + mmap + fork + exec + waitpid)
 *   1: /bin/exec_child target (verifies sp alignment, [sp], x0/x1/x2, argv/envp)
 */

#include <uapi/syscall.h>

#ifndef USER_EXEC_MODE
#define USER_EXEC_MODE 0
#endif

.macro UE_EXIT code
    mov     x0, #\code
    mov     x8, #SYS_exit
    svc     #0
.endm

.section .text
.global _start
.type _start, @function
_start:

#if USER_EXEC_MODE == 0

    /* 1. SYS_exec(NULL, NULL, NULL) -> -EFAULT (-14) */
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x8, #SYS_exec
    svc     #0
    cmn     x0, #14
    b.eq    1f
    UE_EXIT 0x01
1:

    /* 2. SYS_exec("/bin/no_such_prog", NULL, NULL) -> -ENOENT (-2) */
    adrp    x0, str_bad_path
    add     x0, x0, :lo12:str_bad_path
    mov     x1, #0
    mov     x2, #0
    mov     x8, #SYS_exec
    svc     #0
    cmn     x0, #2
    b.eq    2f
    UE_EXIT 0x02
2:

    /* 3. Allocate one lazy RW anonymous page for -E2BIG & COW arg string */
    mov     x0, #0
    mov     x1, #4096
    mov     x2, #3          /* PROT_READ | PROT_WRITE */
    mov     x3, #0x22       /* MAP_PRIVATE | MAP_ANONYMOUS */
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, #0
    b.gt    3f
    UE_EXIT 0x03
3:
    mov     x19, x0         /* x19 = mmap_page */

    /* Fill 1023 'A's + NUL (1024 bytes) */
    mov     x9, #0
    mov     w10, #'A'
4:
    strb    w10, [x19, x9]
    add     x9, x9, #1
    cmp     x9, #1023
    b.lt    4b
    strb    wzr, [x19, x9]

    /* big_argv = { x19, x19, x19, x19, NULL } -> 4096B > 4096 - 512 */
    sub     sp, sp, #48
    stp     x19, x19, [sp, #0]
    stp     x19, x19, [sp, #16]
    str     xzr, [sp, #32]

    adrp    x0, str_child_path
    add     x0, x0, :lo12:str_child_path
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_exec
    svc     #0
    add     sp, sp, #48
    cmn     x0, #7          /* -E2BIG */
    b.eq    5f
    UE_EXIT 0x04
5:

    /* Write "cow_arg\0" into mmap_page */
    adrp    x9, str_cow_arg
    add     x9, x9, :lo12:str_cow_arg
    ldr     x10, [x9]       /* "cow_arg\0" is exactly 8 bytes */
    str     x10, [x19]

    /* 4. fork() */
    mov     x8, #SYS_fork
    svc     #0
    cmp     x0, #0
    b.lt    fork_fail
    b.eq    child_path

    /* Parent: waitpid(child_pid, &status, 0) */
    mov     x20, x0         /* x20 = child_pid */
    sub     sp, sp, #16
    mov     x0, x20
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x20
    b.eq    6f
    UE_EXIT 0x06
6:
    ldr     w1, [sp]
    add     sp, sp, #16
    mov     w2, #(0x2a << 8)
    cmp     w1, w2
    b.eq    7f
    UE_EXIT 0x07
7:
    UE_EXIT 0x55

fork_fail:
    UE_EXIT 0x05

child_path:
    /* Build envp = { "K=V", NULL } and argv = { "/bin/exec_child", x19, "tail", NULL } */
    sub     sp, sp, #48
    adrp    x9, str_kv
    add     x9, x9, :lo12:str_kv
    str     x9, [sp, #0]
    str     xzr, [sp, #8]   /* envp at sp + 0 */

    adrp    x0, str_child_path
    add     x0, x0, :lo12:str_child_path
    adrp    x10, str_tail
    add     x10, x10, :lo12:str_tail
    str     x0, [sp, #16]
    str     x19, [sp, #24]
    str     x10, [sp, #32]
    str     xzr, [sp, #40]  /* argv at sp + 16 */

    add     x1, sp, #16
    mov     x2, sp
    mov     x8, #SYS_exec
    svc     #0
    UE_EXIT 0xee

.section .rodata
str_bad_path:
    .asciz "/bin/no_such_prog"
str_child_path:
    .asciz "/bin/exec_child"
    .balign 8
str_cow_arg:
    .asciz "cow_arg"
str_tail:
    .asciz "tail"
str_kv:
    .asciz "K=V"

#else /* USER_EXEC_MODE == 1 (/bin/exec_child) */

    /* 1. (sp & 0xf) == 0 */
    mov     x9, sp
    tst     x9, #0xf
    b.eq    1f
    UE_EXIT 0x11
1:
    /* 2. x0 == 3 */
    cmp     x0, #3
    b.eq    2f
    UE_EXIT 0x12
2:
    /* 3. [sp] == 3 */
    ldr     x9, [sp]
    cmp     x9, #3
    b.eq    3f
    UE_EXIT 0x13
3:
    /* 4. x1 == sp + 8 */
    add     x9, sp, #8
    cmp     x1, x9
    b.eq    4f
    UE_EXIT 0x14
4:
    /* 5. x2 == sp + 40 */
    add     x9, sp, #40
    cmp     x2, x9
    b.eq    5f
    UE_EXIT 0x15
5:
    /* 6. argv[0][0] == '/' */
    ldr     x9, [x1, #0]
    ldrb    w10, [x9]
    cmp     w10, #'/'
    b.eq    6f
    UE_EXIT 0x16
6:
    /* 7. argv[1] == "cow_arg" */
    ldr     x9, [x1, #8]
    adrp    x10, exp_cow_arg
    add     x10, x10, :lo12:exp_cow_arg
    bl      streq_check
    cbz     x0, 7f
    UE_EXIT 0x17
7:
    /* 8. argv[2] == "tail" */
    ldr     x9, [x1, #16]
    adrp    x10, exp_tail
    add     x10, x10, :lo12:exp_tail
    bl      streq_check
    cbz     x0, 8f
    UE_EXIT 0x18
8:
    /* 9. argv[3] == NULL */
    ldr     x9, [x1, #24]
    cbz     x9, 9f
    UE_EXIT 0x19
9:
    /* 10. envp[0] == "K=V" */
    ldr     x9, [x2, #0]
    adrp    x10, exp_kv
    add     x10, x10, :lo12:exp_kv
    bl      streq_check
    cbz     x0, 10f
    UE_EXIT 0x1a
10:
    /* 11. envp[1] == NULL */
    ldr     x9, [x2, #8]
    cbz     x9, 11f
    UE_EXIT 0x1b
11:
    UE_EXIT 0x2a

/* Compare NUL-terminated strings x9 and x10. Returns x0 = 0 if equal, 1 otherwise. */
streq_check:
    ldrb    w11, [x9], #1
    ldrb    w12, [x10], #1
    cmp     w11, w12
    b.ne    1f
    cbnz    w11, streq_check
    mov     x0, #0
    ret
1:
    mov     x0, #1
    ret

.section .rodata
exp_cow_arg:
    .asciz "cow_arg"
exp_tail:
    .asciz "tail"
exp_kv:
    .asciz "K=V"

#endif

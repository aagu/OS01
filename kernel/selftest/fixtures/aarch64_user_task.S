/* kernel/selftest/fixtures/aarch64_user_task.S
 *
 * The payload of the M4 user-task acceptance case.
 *
 * One source, three images: the fixture id, the data value and the
 * payload mode are compile-time constants, so A, B and the replacement
 * image share the SAME VA layout (the same text/data pages) and differ
 * only in their data.  That is what makes the two tasks' private roots
 * comparable: the same VA, different physical pages, different data.
 *
 * The payload talks to the kernel through the M4 internal SVCs:
 *   #0x4d40  yield          (the kernel answers with x0 == 0)
 *   #0x4d41  replace image  (x0 is the catalog index)
 *   #0x4d42  exit           (x0 is the exit status)
 *
 * The persistent state lives in the callee-saved registers and in the
 * image's own data page, so a yield round trip can be checked.
 */

#ifndef USER_FIXTURE_ID
#define USER_FIXTURE_ID 0
#endif
#ifndef USER_FIXTURE_VALUE
#define USER_FIXTURE_VALUE 0x1111
#endif
#ifndef USER_FIXTURE_MODE
#define USER_FIXTURE_MODE 0
#endif

/* Payload modes. */
#define USER_MODE_EXEC   0   /* A: yield, then the replacement sequence */
#define USER_MODE_GUARD  1   /* B: yield, then touch the guard page */
#define USER_MODE_LEAVE  2   /* the replacement image: leave at once */

#define SVC_YIELD 0x4d40
#define SVC_EXEC  0x4d41
#define SVC_EXIT  0x4d42

/* The errno values the kernel reports for the rejected replacements. */
#define ERR_ENOEXEC 8
#define ERR_ENOMEM  12

.text
    .rept 4
    brk #0                       /* the entry is not the segment base */
    .endr
.global _start
_start:
    /* Publish the entry PC so the test can prove the continuation. */
    adr     x21, user_entry
    adr     x22, _start
    str     x22, [x21]

    /* The expected data value lives in a callee-saved register. */
    movz    x19, #USER_FIXTURE_VALUE

    /* The data page holds the fixture's own value; the kernel mapped
     * it writable, so the value can be read back and rewritten. */
    adr     x21, user_value
    ldr     x20, [x21]
    cmp     x20, x19
    b.ne    .Lfail
    mov     x20, #USER_FIXTURE_ID
    str     x20, [x21, #8]

    /* A private stack value: the user stack belongs to this task
     * alone. */
    mov     x20, #0x5a
    str     x20, [sp, #-16]!

#if USER_FIXTURE_MODE == USER_MODE_LEAVE
    /* The replacement image leaves immediately with its own status. */
    mov     x0, #0x33
    svc     #SVC_EXIT
#endif

    /* A bounded yield loop.  The kernel answers every yield with
     * x0 == 0; the data and the stack value must survive the round
     * trip. */
    mov     x23, #64
.Lyield:
    svc     #SVC_YIELD
    cbz     x0, .Lyield_ok
    b       .Lfail
.Lyield_ok:
    adr     x21, user_value
    ldr     x20, [x21]
    cmp     x20, x19
    b.ne    .Lfail
    ldr     x20, [sp]
    cmp     x20, #0x5a
    b.ne    .Lfail
    subs    x23, x23, #1
    b.ne    .Lyield

#if USER_FIXTURE_MODE == USER_MODE_GUARD
    /* Touch the guard page: the page below the user stack is never
     * mapped, so this must raise a Data Abort with a translation
     * error and the faulting address in FAR. */
    movz    x20, #0xf000
    movk    x20, #0x03ff, lsl #16
    movk    x20, #0x0002, lsl #32     /* x20 = 0x0000000203fff000 */
    ldr     x21, [x20]
    b       .Lfail
#else
    /* Replacement attempt 1: an invalid catalog entry.  The kernel
     * must reject it with -ENOEXEC and leave the running image
     * untouched. */
    mov     x0, #2
    svc     #SVC_EXEC
    cmn     x0, #ERR_ENOEXEC
    b.eq    .Lexec_second
    b       .Lfail

    /* Replacement attempt 2: a valid image whose construction fails
     * once with -ENOMEM.  The running image must survive it. */
.Lexec_second:
    mov     x0, #3
    svc     #SVC_EXEC
    cmn     x0, #ERR_ENOMEM
    b.eq    .Lexec_third
    b       .Lfail

    /* Replacement attempt 3: the real replacement.  The task continues
     * in the NEW image, which owns the exit. */
.Lexec_third:
    mov     x0, #4
    svc     #SVC_EXEC
    cbz     x0, .Lexec_ok
    b       .Lfail
.Lexec_ok:
    b       .Lexit
#endif

.Lfail:
    mov     x0, #0xf0
    svc     #SVC_EXIT
.Lexit:
    mov     x0, #0x11
    svc     #SVC_EXIT

.data
.balign 8
user_value:
    .quad USER_FIXTURE_VALUE
    .quad USER_FIXTURE_ID
user_entry:
    .quad 0

.bss
.balign 8
user_bss:
    .skip 8

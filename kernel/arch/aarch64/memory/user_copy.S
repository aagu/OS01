#if AARCH64_EL0_TEST
#include <errno.h>

.text
.balign 4
/* x0 = dst, x1 = src, x2 = len. The user operand register still
 * holds the fault address when an LDTRB/STTRB aborts. */
.globl aarch64_copy_from_user_raw
.type aarch64_copy_from_user_raw, %function
aarch64_copy_from_user_raw:
    cbz     x2, 2f
.globl aarch64_uaccess_load
aarch64_uaccess_load:
    ldtrb   w3, [x1]
    strb    w3, [x0], #1
    add     x1, x1, #1
    subs    x2, x2, #1
    b.ne    aarch64_uaccess_load
2:  mov     w0, #0
    ret
.size aarch64_copy_from_user_raw, . - aarch64_copy_from_user_raw

.globl aarch64_copy_to_user_raw
.type aarch64_copy_to_user_raw, %function
aarch64_copy_to_user_raw:
    cbz     x2, 2f
1:  ldrb    w3, [x1], #1
.globl aarch64_uaccess_store
aarch64_uaccess_store:
    sttrb   w3, [x0]
    add     x0, x0, #1
    subs    x2, x2, #1
    b.ne    1b
2:  mov     w0, #0
    ret
.size aarch64_copy_to_user_raw, . - aarch64_copy_to_user_raw

.globl aarch64_uaccess_fault
aarch64_uaccess_fault:
    mov     w0, #-EFAULT
    ret
#endif

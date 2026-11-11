/* The existing M4 probe payload, now linked as a user ELF. */
#include <errno.h>

.text
    .rept 4
    brk #0                              /* entry is not the segment base */
    .endr
.global _start
_start:
    adr     x11, probe_value
    ldr     x10, [x11, #8]               /* .data from the file */
    mov     x13, #0x1357
    cmp     x10, x13
    b.ne    2f
    adr     x13, probe_bss
    ldr     x10, [x13]                   /* .bss outside p_filesz */
    cbnz    x10, 2f
    mov     x12, x0
    cmp x12, #2
    b.eq 4f
    adrp x9, _start
    ldr x0, [x11]
    str x0, [sp, #-16]!
    mov x30, #0x1234
    svc #0
    ldr x10, [sp], #16
    add x10, x10, #1
    cmp x0, x10
    b.ne 2f
    mov x10, #0x1234
    cmp x30, x10
    b.ne 2f
    cbz x12, 1f
    cmp x12, #1
    b.ne 3f
    str x0, [x9]
    b 2f
3:  ldr x0, [x12]
    b 2f
1:  svc #1
2:  svc #2
    b 2b
4:  adr x0, probe_value
    add x0, x0, #1, lsl #12
    sub x9, x0, #1
    mov w10, #0x12
    strb w10, [x9]
    mov w10, #0x34
    strb w10, [x9, #1]
    mov x0, x9
    mov x1, #2
    svc #3
    cbnz x0, 2b
    ldrb w10, [x9]
    cmp w10, #0x34
    b.ne 2b
    ldrb w10, [x9, #1]
    cmp w10, #0x12
    b.ne 2b
    sub x0, sp, #1
    mov x1, #2
    svc #3
    cmn x0, #EFAULT
    b.ne 2b
    mov x0, xzr
    b 1b

.data
.balign 8
probe_value:
    .quad 0, 0x1357

.bss
.balign 8
probe_bss:
    .skip 8

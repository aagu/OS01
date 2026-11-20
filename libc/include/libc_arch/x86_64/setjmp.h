#ifndef _LIBC_ARCH_X86_64_SETJMP_H
#define _LIBC_ARCH_X86_64_SETJMP_H

/* System V AMD64 ABI callee-saved state:
 *   [0] rbx, [1] rbp, [2] r12, [3] r13, [4] r14, [5] r15, [6] rip, [7] rsp
 * Kept at 16 unsigned longs (128 bytes) to preserve existing x86_64 ABI. */
typedef unsigned long jmp_buf[16];

#endif /* _LIBC_ARCH_X86_64_SETJMP_H */

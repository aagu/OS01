/* libc/arch/x86_64/setjmp.S — System V AMD64 setjmp / longjmp override.
 *
 * Layout (matches libc/include/arch/x86_64/setjmp.h):
 *   0(%rdi)  rbx
 *   8(%rdi)  rbp
 *  16(%rdi)  r12
 *  24(%rdi)  r13
 *  32(%rdi)  r14
 *  40(%rdi)  r15
 *  48(%rdi)  return address (rip)
 *  56(%rdi)  caller rsp (rsp + 8 on entry)
 */

    .text
    .global setjmp
    .type setjmp, @function
setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    movq    (%rsp), %rdx        /* return address pushed by call */
    movq    %rdx, 48(%rdi)
    leaq    8(%rsp), %rdx       /* caller's stack pointer */
    movq    %rdx, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    56(%rdi), %rsp
    movq    48(%rdi), %rdx
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:
    jmp     *%rdx
    .size longjmp, . - longjmp

/* Standalone ET_EXEC compatibility fixture for ASLR-B systest (x86_64 only).
 * Linked with libc/arch/x86_64/user.ld at fixed base 0x400000.
 * Verifies runtime &_start == 0x400000 and exits 73 on match, 74 otherwise. */

.section .text.start, "ax", @progbits
.globl _start
.type _start, @function
_start:
    leaq _start(%rip), %rax
    movabsq $0x400000, %rcx
    cmpq %rcx, %rax
    jne .Lmismatch
    movq $2, %rax
    movq $73, %rdi
    int $0x80
    hlt

.Lmismatch:
    movq $2, %rax
    movq $74, %rdi
    int $0x80
    hlt
.size _start, . - _start

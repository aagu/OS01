.section .text.start,"ax"
.global _start
.type _start, @function
_start:
    # Clear frame pointer for backtrace termination
    xorq %rbp, %rbp

    # Zero-initialize BSS
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx
    jz 1f
    xorb %al, %al
    rep stosb
1:
    # Call main(argc, argv) — no arguments for now
    xorl %edi, %edi      # argc = 0
    xorl %esi, %esi      # argv = NULL
    call main

    # exit(return_value)
    movl %eax, %edi
    movl $2, %eax        # SYS_exit = 2
    int $0x80

    # Should never reach here
    hlt


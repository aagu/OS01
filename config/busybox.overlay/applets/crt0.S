.section .text.start,"ax"
.global _start
.type _start, @function
_start:
    # Clear frame pointer for backtrace termination
    xorq %rbp, %rbp

    # ── Save argc, argv, envp BEFORE clobbering registers ──
    # Kernel passes: rdi=argc, rsi=argv, rdx=envp
    # Save to callee-saved registers that survive across function calls.
    movq %rdi, %r12          # r12 = argc
    movq %rsi, %r13          # r13 = argv
    movq %rdx, %r14          # r14 = envp

    # Zero-initialize BSS
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx
    jz 1f
    xorb %al, %al
    rep stosb
1:
    # Restore argc, argv, envp and call main
    movq %r12, %rdi          # argc
    movq %r13, %rsi          # argv
    movq %r14, %rdx          # envp
    call main

    # exit(return_value)
    movl %eax, %edi
    movl $2, %eax        # SYS_exit = 2
    int $0x80

    # Should never reach here
    hlt

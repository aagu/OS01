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
    # Save entry rsp AFTER BSS clear — xorb %al above clobbers rax, and rsp
    # is unchanged by rep stosb, so saving here is equivalent (spec §4.2).
    movq %rsp, %rax

    # __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
    # rdi=main rsi=argc rdx=argv rcx=init(0) r8=fini(0) r9=rtld_fini(0)
    # 7th arg (stack_end) passed on the stack.
    movq (%rsp), %rsi          # argc  ← from the stack (not kernel registers)
    leaq 8(%rsp), %rdx         # argv
    leaq main(%rip), %rdi      # main
    xorl %ecx, %ecx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    subq $16, %rsp             # keep rsp 16-aligned; [rsp] = arg slot
    movq %rax, (%rsp)          # stack_end = entry rsp
    call __libc_start_main
    addq $16, %rsp

    # exit(main's return value)
    movl %eax, %edi
    movl $2, %eax              # SYS_exit = 2
    int $0x80

    # Should never reach here
    hlt
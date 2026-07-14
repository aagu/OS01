# kernel/arch/x86_64/thread_entry.S
# Thread entry trampoline for kernel threads (x86_64).
# Called by kernel_thread() — pops saved register state from
# the fake pt_regs frame, loads data segments, and calls fn(arg).
# On return, calls do_exit().

.globl kernel_thread_func
kernel_thread_func:
    sti                         # re-enable IRQs after first context switch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbx                   # fn pointer
    popq %rcx
    popq %rdx                   # arg
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rax                   # skip DS slot
    popq %rax                   # skip ES slot
    popq %rax                   # original RAX
    pushq %rax                  # save RAX temporarily
    movq $0x10, %rax
    movq %rax, %ds
    movq %rax, %es
    movq %rax, %fs
    # GS is NOT reloaded — its base is per-CPU, set via
    # IA32_GS_BASE MSR. Loading a selector would clobber
    # the per-CPU base with the GDT flat descriptor value.
    popq %rax                   # restore RAX
    addq $0x38, %rsp
    movq %rdx, %rdi             # arg
    callq *%rbx                 # call fn(arg)
    movq %rax, %rdi
    callq do_exit

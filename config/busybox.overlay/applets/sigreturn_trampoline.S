# user/sigreturn_trampoline.S — signal handler return trampoline
# Placed on the user stack as the handler's return address.
# When the handler executes 'ret', it lands here.
# Executes SYS_sigreturn (43) to restore the saved register frame.

    .section .text.start,"ax"
    .global sigreturn_trampoline
    .type sigreturn_trampoline, @function
sigreturn_trampoline:
    movq $43, %rax       # SYS_sigreturn
    int   $0x80
    # unreachable — iretq from SYS_sigreturn goes back to
    # the original execution context
    hlt

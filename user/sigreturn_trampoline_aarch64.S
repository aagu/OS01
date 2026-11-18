# user/sigreturn_trampoline_aarch64.S — signal handler return trampoline
# Placed on the user stack as the handler's return address.
# When the handler executes 'ret', it lands here.
# Executes SYS_sigreturn (43) via the aarch64 native SVC ABI (x8 = number)
# to restore the saved register frame.

    .section .text.start,"ax"
    .global sigreturn_trampoline
    .type sigreturn_trampoline, @function
sigreturn_trampoline:
    mov x8, #43               // SYS_sigreturn
    svc #0
    // unreachable — the sigreturn path goes back to the original
    // execution context
    brk #0
    b .
    .size sigreturn_trampoline, . - sigreturn_trampoline

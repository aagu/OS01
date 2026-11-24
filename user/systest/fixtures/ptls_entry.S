/*
 * ptls_entry.S — entry stub for the PT_TLS acceptance probes.
 *
 * The SysV initial stack pointer (argc/argv/envp/auxv) is passed to
 * the freestanding C probe; the probe never returns (it exits through
 * the raw exit syscall), but a defensive halt closes the path.
 */
    .text
    .globl _start
    .type _start, @function
_start:
    movq %rsp, %rdi
    andq $-16, %rsp
    call ptls_probe_main
    movq $2, %rax              /* SYS_exit */
    movq $90, %rdi
    int $0x80
1:
    hlt
    jmp 1b
    .size _start, . - _start

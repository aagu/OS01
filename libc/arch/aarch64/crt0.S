# libc/arch/aarch64/crt0.S — AArch64 user program entry
# Same contract as libc/arch/x86_64/crt0.S (x86): zero BSS, then
# __libc_start_main(main, argc, argv, init=0, fini=0, rtld_fini=0, stack_end)
# with the AAPCS64 register window (x0..x6); the kernel's initial stack
# frame places argc at [sp] and argv at [sp+8] (aarch64_user_image_setup_stack).

    .section .text.start,"ax"
    .global _start
    .type _start, @function
_start:
    // Clear frame pointer for backtrace termination
    mov x29, #0

    // Zero-initialize BSS (4-byte stores; both symbols are 16-aligned)
    adrp x0, __bss_start
    add x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
    cmp x0, x1
    b.eq 1f
    mov w2, #0
2:  str w2, [x0], #4
    cmp x0, x1
    b.lo 2b
1:

    // __libc_start_main(main, argc, argv, 0, 0, 0, stack_end)
    mov x6, sp                // stack_end = entry sp (saved before any push)
    ldr x1, [sp]              // argc  ← from the stack (not kernel registers)
    add x2, sp, #8            // argv
    adrp x0, main
    add x0, x0, :lo12:main
    mov x3, #0                // init
    mov x4, #0                // fini
    mov x5, #0                // rtld_fini
    bl __libc_start_main

    // __libc_start_main does not return (it ends in exit(3)); the tail is
    // a defensive SYS_exit so a buggy return still terminates cleanly.
    mov w8, #2                // SYS_exit = 2
    svc #0

    // Should never reach here
    brk #0
    b .
    .size _start, . - _start

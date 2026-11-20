#ifndef _LIBC_ARCH_X86_64_DIV_H
#define _LIBC_ARCH_X86_64_DIV_H

/* x86_64: divq — single instruction, quotient in rax, remainder in rdx. */
#define do_div(n, base) ({ \
    int __res; \
    __asm__("divq %%rcx" : "=a" (n), "=d" (__res) : "0" (n), "1" (0), "c" (base)); \
    __res; })

#endif /* _LIBC_ARCH_X86_64_DIV_H */

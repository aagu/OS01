#include <setjmp.h>
#include <stdint.h>

int setjmp(jmp_buf env) {
    __asm__ volatile(
        "movq %%rbx, 0*8(%0)\n"
        "movq %%rbp, 1*8(%0)\n"
        "movq %%r12, 2*8(%0)\n"
        "movq %%r13, 3*8(%0)\n"
        "movq %%r14, 4*8(%0)\n"
        "movq %%r15, 5*8(%0)\n"
        "movq 8(%%rbp), %%rcx\n"
        "movq %%rcx, 6*8(%0)\n"
        "leaq 16(%%rbp), %%rcx\n"
        "movq %%rcx, 7*8(%0)\n"
        :: "r"(env)
        : "memory", "rcx"
    );
    return 0;
}

void longjmp(jmp_buf env, int val) {
    register uint64_t *e asm("r11") = (uint64_t *)env;
    register uint64_t v asm("r10") = (uint64_t)val;

    __asm__ volatile(
        "movq 0(%1), %%rbx\n"
        "movq 8(%1), %%rbp\n"
        "movq 16(%1), %%r12\n"
        "movq 24(%1), %%r13\n"
        "movq 32(%1), %%r14\n"
        "movq 40(%1), %%r15\n"
        "movq 48(%1), %%rcx\n"
        "movq 56(%1), %%rdx\n"
        "movq %%rcx, (%%rdx)\n"
        "movq %%rdx, %%rsp\n"
        "movq %0, %%rax\n"
        "testl %%eax, %%eax\n"
        "jnz 1f\n"
        "incl %%eax\n"
        "1:\n"
        "ret\n"
        :: "r"(v), "r"(e)
        : "rbx", "rbp", "r12", "r13", "r14", "r15", "rcx", "rdx", "memory"
    );
}

// kernel/arch/x86_64/irq.c -- x86-specific IRQ gate installation
//
// Build_IRQ expansions and arch_irq_install() — one per IRQ line.

#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/linkage.h>

extern void ret_from_intr(void);
extern void do_IRQ(pt_regs_t *regs, uint64_t nr);

#define Build_IRQ(nr)                                                    \
    extern void IRQ##nr##_interrupt(void);                                \
    __asm__(                                                             \
        ".globl " SYMBOL_NAME_STR(IRQ) #nr "_interrupt\n\t"              \
        SYMBOL_NAME_STR(IRQ) #nr "_interrupt:\n\t"                       \
        "pushq  $0\n\t"                   /* dummy error code */         \
        "cld;\n\t"                                                        \
        "pushq  %rax;\n\t"                                               \
        "pushq  %rax;\n\t"                                               \
        "movq   %es,    %rax;\n\t"                                       \
        "pushq  %rax;\n\t"                                               \
        "movq   %ds,    %rax;\n\t"                                       \
        "pushq  %rax;\n\t"                                               \
        "xorq   %rax,   %rax;\n\t"                                       \
        "pushq  %rbp;\n\t"                                               \
        "pushq  %rdi;\n\t"                                               \
        "pushq  %rsi;\n\t"                                               \
        "pushq  %rdx;\n\t"                                               \
        "pushq  %rcx;\n\t"                                               \
        "pushq  %rbx;\n\t"                                               \
        "pushq  %r8;\n\t"                                                \
        "pushq  %r9;\n\t"                                                \
        "pushq  %r10;\n\t"                                               \
        "pushq  %r11;\n\t"                                               \
        "pushq  %r12;\n\t"                                               \
        "pushq  %r13;\n\t"                                               \
        "pushq  %r14;\n\t"                                               \
        "pushq  %r15;\n\t"                                               \
        "movq   $0x10,  %rdx;\n\t"                                       \
        "movq   %rdx,   %ds;\n\t"                                        \
        "movq   %rdx,   %es;\n\t"                                        \
        "movq   %rsp,   %rdi;\n\t"       /* pt_regs* (arg 1) */          \
        "movq   $" #nr ", %rsi;\n\t"     /* IRQ number (0-15, arg 2) */     \
        "leaq   ret_from_intr(%rip), %rax;\n\t"                          \
        "pushq  %rax;\n\t"               /* return via ret_from_intr */  \
        "jmp    do_IRQ\n\t"                                              \
    );                                                                   \
    static inline void __attribute__((always_inline))                    \
    _irq_install_##nr(void) {                                            \
        set_intr_gate_raw(0x20 + (nr), 0,                                \
                          (void *)(uintptr_t)IRQ##nr##_interrupt);       \
    }

Build_IRQ(0);  Build_IRQ(1);  Build_IRQ(2);  Build_IRQ(3);
Build_IRQ(4);  Build_IRQ(5);  Build_IRQ(6);  Build_IRQ(7);
Build_IRQ(8);  Build_IRQ(9);  Build_IRQ(10); Build_IRQ(11);
Build_IRQ(12); Build_IRQ(13); Build_IRQ(14); Build_IRQ(15);

#undef Build_IRQ

void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist) {
    set_intr_gate_raw(vector, ist, stub);
}

void arch_irq_install(void) {
    _irq_install_0();  _irq_install_1();  _irq_install_2();  _irq_install_3();
    _irq_install_4();  _irq_install_5();  _irq_install_6();  _irq_install_7();
    _irq_install_8();  _irq_install_9();  _irq_install_10(); _irq_install_11();
    _irq_install_12(); _irq_install_13(); _irq_install_14(); _irq_install_15();
}

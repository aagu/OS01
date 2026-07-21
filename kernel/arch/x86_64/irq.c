// kernel/arch/x86_64/irq.c -- x86-specific IRQ gate installation
//
// Build_IRQ expansions and arch_irq_install() — one per IRQ line.

#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/linkage.h>

extern void ret_from_intr(void);
extern void do_IRQ(pt_regs_t *regs, uint64_t nr);

#define Build_IRQ(nr, vector)                                              \
    extern void IRQ##nr##_interrupt(void);                                  \
    __asm__(                                                                \
        ".globl " SYMBOL_NAME_STR(IRQ) #nr "_interrupt\n\t"                \
        SYMBOL_NAME_STR(IRQ) #nr "_interrupt:\n\t"                         \
        "pushq  $0\n\t"                   /* dummy error code */           \
        "cld;\n\t"                                                          \
        "pushq  %rax;\n\t"                                                 \
        "pushq  %rax;\n\t"                                                 \
        "movq   %es,    %rax;\n\t"                                         \
        "pushq  %rax;\n\t"                                                 \
        "movq   %ds,    %rax;\n\t"                                         \
        "pushq  %rax;\n\t"                                                 \
        "xorq   %rax,   %rax;\n\t"                                         \
        "pushq  %rbp;\n\t"                                                 \
        "pushq  %rdi;\n\t"                                                 \
        "pushq  %rsi;\n\t"                                                 \
        "pushq  %rdx;\n\t"                                                 \
        "pushq  %rcx;\n\t"                                                 \
        "pushq  %rbx;\n\t"                                                 \
        "pushq  %r8;\n\t"                                                  \
        "pushq  %r9;\n\t"                                                  \
        "pushq  %r10;\n\t"                                                 \
        "pushq  %r11;\n\t"                                                 \
        "pushq  %r12;\n\t"                                                 \
        "pushq  %r13;\n\t"                                                 \
        "pushq  %r14;\n\t"                                                 \
        "pushq  %r15;\n\t"                                                 \
        "movq   $0x10,  %rdx;\n\t"                                         \
        "movq   %rdx,   %ds;\n\t"                                          \
        "movq   %rdx,   %es;\n\t"                                          \
        "movq   %rsp,   %rdi;\n\t"       /* pt_regs* (arg 1) */            \
        "movq   $" #vector ", %rsi;\n\t" /* IRQ vector (arg 2) */          \
        "leaq   ret_from_intr(%rip), %rax;\n\t"                            \
        "pushq  %rax;\n\t"               /* return via ret_from_intr */    \
        "jmp    do_IRQ\n\t"                                              \
    );                                                                     \
    static inline void __attribute__((always_inline))                      \
    _irq_install_##nr(void) {                                              \
        set_intr_gate_raw(vector, 0,                                        \
                          (void *)(uintptr_t)IRQ##nr##_interrupt);         \
    }

Build_IRQ(0, 0x20);  Build_IRQ(1, 0x21);  Build_IRQ(2, 0x22);  Build_IRQ(3, 0x23);
Build_IRQ(4, 0x24);  Build_IRQ(5, 0x25);  Build_IRQ(6, 0x26);  Build_IRQ(7, 0x27);
Build_IRQ(8, 0x28);  Build_IRQ(9, 0x29);  Build_IRQ(10, 0x2a); Build_IRQ(11, 0x2b);
Build_IRQ(12, 0x2c); Build_IRQ(13, 0x2d); Build_IRQ(14, 0x2e); Build_IRQ(15, 0x2f);
// PCI GSIs 16-23 on Q35/ICH9 (PIRQ[A-D] → GSI[16-19], plus 4 extra for chipsets
// that expose more lines, e.g. ICH10 with PIRQ[E-H] → GSI[20-23]).
Build_IRQ(16, 0x30); Build_IRQ(17, 0x31); Build_IRQ(18, 0x32); Build_IRQ(19, 0x33);
Build_IRQ(20, 0x34); Build_IRQ(21, 0x35); Build_IRQ(22, 0x36); Build_IRQ(23, 0x37);

#undef Build_IRQ

void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist) {
    set_intr_gate_raw(vector, ist, stub);
}

void arch_irq_install(void) {
    _irq_install_0();  _irq_install_1();  _irq_install_2();  _irq_install_3();
    _irq_install_4();  _irq_install_5();  _irq_install_6();  _irq_install_7();
    _irq_install_8();  _irq_install_9();  _irq_install_10(); _irq_install_11();
    _irq_install_12(); _irq_install_13(); _irq_install_14(); _irq_install_15();
    _irq_install_16(); _irq_install_17(); _irq_install_18(); _irq_install_19();
    _irq_install_20(); _irq_install_21(); _irq_install_22(); _irq_install_23();
}

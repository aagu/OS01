// kernel/arch/x86_64/irq.c -- x86-specific IRQ gate installation
//
// Build_IRQ expansions and arch_irq_install() — one per IRQ line.

#include <arch/x86_64/gate.h>
#include <arch/x86_64/linkage.h>
#include <arch/irq.h>      // arch_irq_dispatch (arch-neutral hook)

extern void ret_from_intr(void);
// do_IRQ used to live in kernel/intr/pic/8259A.c. It now lives as
// `arch_irq_dispatch` in kernel/arch/x86_64/irq_hooks.c (alongside
// the rest of the x86_64 IRQ-hook strong overrides).

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
        "jmp    arch_irq_dispatch\n\t"                                   \
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

// Fixed MSI-X slot (vector 0x39, nr = 25)
Build_IRQ(25, 0x39);

// Extended device IRQ pool: 0x42..0x7f (62 vectors, nr = vector - 32)
Build_IRQ(34, 0x42); Build_IRQ(35, 0x43); Build_IRQ(36, 0x44); Build_IRQ(37, 0x45);
Build_IRQ(38, 0x46); Build_IRQ(39, 0x47); Build_IRQ(40, 0x48); Build_IRQ(41, 0x49);
Build_IRQ(42, 0x4a); Build_IRQ(43, 0x4b); Build_IRQ(44, 0x4c); Build_IRQ(45, 0x4d);
Build_IRQ(46, 0x4e); Build_IRQ(47, 0x4f); Build_IRQ(48, 0x50); Build_IRQ(49, 0x51);
Build_IRQ(50, 0x52); Build_IRQ(51, 0x53); Build_IRQ(52, 0x54); Build_IRQ(53, 0x55);
Build_IRQ(54, 0x56); Build_IRQ(55, 0x57); Build_IRQ(56, 0x58); Build_IRQ(57, 0x59);
Build_IRQ(58, 0x5a); Build_IRQ(59, 0x5b); Build_IRQ(60, 0x5c); Build_IRQ(61, 0x5d);
Build_IRQ(62, 0x5e); Build_IRQ(63, 0x5f); Build_IRQ(64, 0x60); Build_IRQ(65, 0x61);
Build_IRQ(66, 0x62); Build_IRQ(67, 0x63); Build_IRQ(68, 0x64); Build_IRQ(69, 0x65);
Build_IRQ(70, 0x66); Build_IRQ(71, 0x67); Build_IRQ(72, 0x68); Build_IRQ(73, 0x69);
Build_IRQ(74, 0x6a); Build_IRQ(75, 0x6b); Build_IRQ(76, 0x6c); Build_IRQ(77, 0x6d);
Build_IRQ(78, 0x6e); Build_IRQ(79, 0x6f); Build_IRQ(80, 0x70); Build_IRQ(81, 0x71);
Build_IRQ(82, 0x72); Build_IRQ(83, 0x73); Build_IRQ(84, 0x74); Build_IRQ(85, 0x75);
Build_IRQ(86, 0x76); Build_IRQ(87, 0x77); Build_IRQ(88, 0x78); Build_IRQ(89, 0x79);
Build_IRQ(90, 0x7a); Build_IRQ(91, 0x7b); Build_IRQ(92, 0x7c); Build_IRQ(93, 0x7d);
Build_IRQ(94, 0x7e); Build_IRQ(95, 0x7f);

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

    _irq_install_25();

    _irq_install_34(); _irq_install_35(); _irq_install_36(); _irq_install_37();
    _irq_install_38(); _irq_install_39(); _irq_install_40(); _irq_install_41();
    _irq_install_42(); _irq_install_43(); _irq_install_44(); _irq_install_45();
    _irq_install_46(); _irq_install_47(); _irq_install_48(); _irq_install_49();
    _irq_install_50(); _irq_install_51(); _irq_install_52(); _irq_install_53();
    _irq_install_54(); _irq_install_55(); _irq_install_56(); _irq_install_57();
    _irq_install_58(); _irq_install_59(); _irq_install_60(); _irq_install_61();
    _irq_install_62(); _irq_install_63(); _irq_install_64(); _irq_install_65();
    _irq_install_66(); _irq_install_67(); _irq_install_68(); _irq_install_69();
    _irq_install_70(); _irq_install_71(); _irq_install_72(); _irq_install_73();
    _irq_install_74(); _irq_install_75(); _irq_install_76(); _irq_install_77();
    _irq_install_78(); _irq_install_79(); _irq_install_80(); _irq_install_81();
    _irq_install_82(); _irq_install_83(); _irq_install_84(); _irq_install_85();
    _irq_install_86(); _irq_install_87(); _irq_install_88(); _irq_install_89();
    _irq_install_90(); _irq_install_91(); _irq_install_92(); _irq_install_93();
    _irq_install_94(); _irq_install_95();
}

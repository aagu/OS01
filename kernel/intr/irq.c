#include <kernel/interrupt.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/printk.h>
#include <stddef.h>
#include <kernel/softirq.h>
#include <string.h>

#define SAVE_ALL				\
	"cld;			\n\t"		\
	"pushq	%rax;		\n\t"		\
	"pushq	%rax;		\n\t"		\
	"movq	%es,	%rax;	\n\t"		\
	"pushq	%rax;		\n\t"		\
	"movq	%ds,	%rax;	\n\t"		\
	"pushq	%rax;		\n\t"		\
	"xorq	%rax,	%rax;	\n\t"		\
	"pushq	%rbp;		\n\t"		\
	"pushq	%rdi;		\n\t"		\
	"pushq	%rsi;		\n\t"		\
	"pushq	%rdx;		\n\t"		\
	"pushq	%rcx;		\n\t"		\
	"pushq	%rbx;		\n\t"		\
	"pushq	%r8;		\n\t"		\
	"pushq	%r9;		\n\t"		\
	"pushq	%r10;		\n\t"		\
	"pushq	%r11;		\n\t"		\
	"pushq	%r12;		\n\t"		\
	"pushq	%r13;		\n\t"		\
	"pushq	%r14;		\n\t"		\
	"pushq	%r15;		\n\t"		\
	"movq	$0x10,	%rdx;	\n\t"		\
	"movq	%rdx,	%ds;	\n\t"		\
	"movq	%rdx,	%es;	\n\t"

/*
*/

#define IRQ_NAME2(nr) nr##_interrupt(void)
#define IRQ_NAME(nr) IRQ_NAME2(IRQ##nr)

/*
*/

#define Build_IRQ(nr)							\
void IRQ_NAME(nr);								\
__asm__ (	SYMBOL_NAME_STR(IRQ)#nr"_interrupt:		\n\t"	\
			"pushq	$0x00				\n\t"	\
			SAVE_ALL					\
			"movq	%rsp,	%rdi			\n\t"	\
			"leaq	ret_from_intr(%rip),	%rax	\n\t"	\
			"pushq	%rax				\n\t"	\
			"movq	$"#nr",	%rsi			\n\t"	\
			"jmp	do_IRQ	\n\t");


/*
*/

Build_IRQ(0x20)
Build_IRQ(0x21)
Build_IRQ(0x22)
Build_IRQ(0x23)
Build_IRQ(0x24)
Build_IRQ(0x25)
Build_IRQ(0x26)
Build_IRQ(0x27)
Build_IRQ(0x28)
Build_IRQ(0x29)
Build_IRQ(0x2a)
Build_IRQ(0x2b)
Build_IRQ(0x2c)
Build_IRQ(0x2d)
Build_IRQ(0x2e)
Build_IRQ(0x2f)
Build_IRQ(0x30)
Build_IRQ(0x31)
Build_IRQ(0x32)
Build_IRQ(0x33)
Build_IRQ(0x34)
Build_IRQ(0x35)
Build_IRQ(0x36)
Build_IRQ(0x37)

void (* interrupt[24])(void)=
{
	IRQ0x20_interrupt,
	IRQ0x21_interrupt,
	IRQ0x22_interrupt,
	IRQ0x23_interrupt,
	IRQ0x24_interrupt,
	IRQ0x25_interrupt,
	IRQ0x26_interrupt,
	IRQ0x27_interrupt,
	IRQ0x28_interrupt,
	IRQ0x29_interrupt,
	IRQ0x2a_interrupt,
	IRQ0x2b_interrupt,
	IRQ0x2c_interrupt,
	IRQ0x2d_interrupt,
	IRQ0x2e_interrupt,
	IRQ0x2f_interrupt,
	IRQ0x30_interrupt,
	IRQ0x31_interrupt,
	IRQ0x32_interrupt,
	IRQ0x33_interrupt,
	IRQ0x34_interrupt,
	IRQ0x35_interrupt,
	IRQ0x36_interrupt,
	IRQ0x37_interrupt,
};

int32_t register_irq(uint64_t nr, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
		uint64_t parameter,
        hw_int_controller_t * controller,
        const char * irq_name)
{
	irq_desc_t * p = &irq_table[nr - 32];

	p->controller = controller;
	strcpy(p->irq_name, irq_name);
	p->parameter = parameter;
	p->flags = 0;
	p->handler = handler;

	if (p->controller != NULL)
	{
		p->controller->install(nr, arg);
		p->controller->enable(nr);
	}

	return 1;
}

uint32_t unregister_irq(uint64_t nr)
{
	irq_desc_t* p = &irq_table[nr - 32];

	if (p->controller != NULL)
	{
		p->controller->disable(nr);
		p->controller->uninstall(nr);
	}
	p->controller = NULL;
	p->irq_name = NULL;
	p->parameter = 0;
	p->flags = 0;
	p->handler = NULL;

	return 1;
}

void irq_install()
{
    for (int i = 32; i < 56; i++)
    {
        set_intr_gate(i, 2, interrupt[i - 32]);
    }

	softirq_init();
}
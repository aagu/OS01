#include <kernel/interrupt.h>
#include <kernel/arch/irq.h>
#include <kernel/debug.h>
#include <stddef.h>
#include <kernel/softirq.h>
#include <string.h>

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
	p->irq_name[0] = '\0';
	p->parameter = 0;
	p->flags = 0;
	p->handler = NULL;

	return 1;
}

void irq_install()
{
	arch_irq_install();
	softirq_init();
}

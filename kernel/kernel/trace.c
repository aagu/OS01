#include <kernel/trace.h>
#include <kernel/printk.h>

void backtrace(pt_regs_t* regs)
{
    unsigned long *rbp = (unsigned long *)regs->rbp;
	unsigned long ret_address = regs->rip;
	int i = 0;

	color_printk(RED,BLACK,"====================== Kernel Stack Backtrace ======================\n");

	for(i = 0;i<KERNEL_TRACE_DEPTH;i++)
	{
		if(lookup_kallsyms(ret_address,i))
			break;
		if((unsigned long)rbp < (unsigned long)regs->rsp)
			break;

		ret_address = *(rbp+1);
		rbp = (unsigned long *)*rbp;
	}
}

int32_t lookup_kallsyms(uint64_t address, int32_t level)
{
    int index = 0;
	int level_index = 0;
	char * string =(char *) &kallsyms_names;
	for(index = 0;index<kallsyms_syms_num;index++)
		if(address > kallsyms_addresses[index] && address <= kallsyms_addresses[index+1])
			break;
	if(index < kallsyms_syms_num)
	{
		for(level_index = 0;level_index < level;level_index++)
			color_printk(RED,BLACK,"  ");
		color_printk(RED,BLACK,"+---> ");

		color_printk(RED,BLACK,"address:%#018lx \t(+) %04d function:%s\n",address,address - kallsyms_addresses[index],&string[kallsyms_index[index]]);
		return 0;
	}
	else
		return 1;
}
#include <core/trace.h>
#include <log/log.h>
#include <stddef.h>

void backtrace(pt_regs_t* regs)
{
    unsigned long *rbp = (unsigned long *)regs->rbp;
	unsigned long ret_address = regs->rip;
	int i = 0;

	log_err("====================== Kernel Stack Backtrace ======================\n");

	for(i = 0;i<KERNEL_TRACE_DEPTH;i++)
	{
		if(lookup_kallsyms(ret_address,i)) {
			log_err("%*s+---> address:%#018lx \t(no symbol)\n", i * 2, "", ret_address);
		}
		if((unsigned long)rbp < (unsigned long)regs->rsp)
			break;

		ret_address = *(rbp+1);
		rbp = (unsigned long *)*rbp;
	}
}

int32_t lookup_kallsyms(uint64_t address, int32_t level)
{
	const char *name = NULL;
	uint64_t offset = 0;

	if (trace_symbol_lookup((uintptr_t)address, &name, &offset))
	{
		log_err("%*s+---> address:%#018lx \t(+) %04d function:%s\n",
		        level * 2, "", address,
		        (int)offset,
		        name);
		return 0;
	}
	return 1;
}

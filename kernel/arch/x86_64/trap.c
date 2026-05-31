#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/printk.h>
#include <kernel/trace.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/task.h>
#include <driver/serial.h>

// Redirect a user-mode fault to do_exit instead of returning to ring 3
static void kill_current_user_task(pt_regs_t *regs)
{
    serial_printk("Killing task %d (user fault at RIP=%p)\n",
                  current->pid, regs->rip);
    current->state = TASK_ZOMBIE;

    // Overwrite iretq target: return to ring-0 do_exit instead of user code
    regs->rip = (uint64_t)do_exit;
    regs->cs  = KERNEL_CS;
    regs->ss  = KERNEL_DS;    // ring-0 stack segment
    regs->ds  = KERNEL_DS;
    regs->es  = KERNEL_DS;
    regs->rflags = (1 << 9);
}

void do_divide_error(pt_regs_t * regs, uint64_t error_code)
{
    color_printk(RED, BLACK, "do_divide_error(0),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_divide_error(0),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_debug(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_debug(1),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_debug(1),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_nmi(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_nmi(2),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_nmi(2),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_int3(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_int3(3),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_int3(3),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_overflow(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_overflow(4),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_overflow(4),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_bounds(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_bounds(5),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_bounds(5),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_undefined_opcode(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_undefined_opcode(6),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_undefined_opcode(6),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_dev_not_available(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_dev_not_available(7),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_dev_not_available(7),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_double_fault(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_double_fault(8),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_double_fault(8),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_coprocessor_segment_overrun(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_coprocessor_segment_overrun(9),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_coprocessor_segment_overrun(9),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_invalid_TSS(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_invalid_TSS(10),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_invalid_TSS(10),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		color_printk(RED,BLACK,"The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
		serial_printk("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		color_printk(RED,BLACK,"Refers to a gate descriptor in the IDT;\n");
		serial_printk("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		color_printk(RED,BLACK,"Refers to a descriptor in the GDT or the current LDT;\n");
		serial_printk("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			color_printk(RED,BLACK,"Refers to a segment or gate descriptor in the LDT;\n");
			serial_printk("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			color_printk(RED,BLACK,"Refers to a descriptor in the current GDT;\n");
			serial_printk("Refers to a descriptor in the current GDT;\n");
		}
	}
	color_printk(RED,BLACK,"Segment Selector Index:%#010x\n",error_code & 0xfff8);
	serial_printk("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_segment_not_present(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_segment_not_present(11),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_segment_not_present(11),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		color_printk(RED,BLACK,"The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
		serial_printk("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		color_printk(RED,BLACK,"Refers to a gate descriptor in the IDT;\n");
		serial_printk("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		color_printk(RED,BLACK,"Refers to a descriptor in the GDT or the current LDT;\n");
		serial_printk("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			color_printk(RED,BLACK,"Refers to a segment or gate descriptor in the LDT;\n");
			serial_printk("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			color_printk(RED,BLACK,"Refers to a descriptor in the current GDT;\n");
			serial_printk("Refers to a descriptor in the current GDT;\n");
		}
	}
	color_printk(RED,BLACK,"Segment Selector Index:%#010x\n",error_code & 0xfff8);
	serial_printk("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_stack_segment_fault(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_stack_segment_fault(12),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_stack_segment_fault(12),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		color_printk(RED,BLACK,"The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
		serial_printk("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		color_printk(RED,BLACK,"Refers to a gate descriptor in the IDT;\n");
		serial_printk("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		color_printk(RED,BLACK,"Refers to a descriptor in the GDT or the current LDT;\n");
		serial_printk("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			color_printk(RED,BLACK,"Refers to a segment or gate descriptor in the LDT;\n");
			serial_printk("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			color_printk(RED,BLACK,"Refers to a descriptor in the current GDT;\n");
			serial_printk("Refers to a descriptor in the current GDT;\n");
		}
	}
	color_printk(RED,BLACK,"Segment Selector Index:%#010x\n",error_code & 0xfff8);
	serial_printk("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_general_protection(pt_regs_t * regs, uint64_t error_code)
{
	// User-mode fault → kill the task, don't halt the kernel
	if (regs->cs & 3) {
		serial_printk("do_general_protection(13) from user, killing task %d\n", current->pid);
		kill_current_user_task(regs);
		return;
	}
	color_printk(RED,BLACK,"do_general_protection(13),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_general_protection(13),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		color_printk(RED,BLACK,"The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
		serial_printk("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		color_printk(RED,BLACK,"Refers to a gate descriptor in the IDT;\n");
		serial_printk("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		color_printk(RED,BLACK,"Refers to a descriptor in the GDT or the current LDT;\n");
		serial_printk("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			color_printk(RED,BLACK,"Refers to a segment or gate descriptor in the LDT;\n");
			serial_printk("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			color_printk(RED,BLACK,"Refers to a descriptor in the current GDT;\n");
			serial_printk("Refers to a descriptor in the current GDT;\n");
		}
	}
	color_printk(RED,BLACK,"Segment Selector Index:%#010x\n",error_code & 0xfff8);
	serial_printk("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_page_fault(pt_regs_t * regs, uint64_t error_code)
{
	uint64_t cr2 = 0;
	__asm__	__volatile__("movq	%%cr2,	%0":"=r"(cr2)::"memory");

	// User-mode fault → kill the task, don't halt the kernel
	if (regs->cs & 3) {
		serial_printk("do_page_fault(14) user err=%p rip=%p cr2=%p\n",
		              error_code, regs->rip, cr2);
		kill_current_user_task(regs);
		return;
	}

	color_printk(RED,BLACK,"do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(!(error_code & 0x01))
	{
		color_printk(RED,BLACK,"Page Not-Present,\t");
		serial_printk("Page Not-Present,\t");
	}

	if(error_code & 0x02)
	{
		color_printk(RED,BLACK,"Write Cause Fault,\t");
		serial_printk("Write Cause Fault,\t");
	}
	else
	{
		color_printk(RED,BLACK,"Read Cause Fault,\t");
		serial_printk("Read Cause Fault,\t");
	}

	if(error_code & 0x04)
	{
		color_printk(RED,BLACK,"Fault in user(3)\t");
		serial_printk("Fault in user(3)\t");
	}
	else
	{
		color_printk(RED,BLACK,"Fault in supervisor(0,1,2)\t");
		serial_printk("Fault in supervisor(0,1,2)\t");
	}

	if(error_code & 0x08)
	{
		color_printk(RED,BLACK,",Reserved Bit Cause Fault\t");
		serial_printk(",Reserved Bit Cause Fault\t");
	}

	if(error_code & 0x10)
	{
		color_printk(RED,BLACK,",Instruction fetch Cause Fault");
		serial_printk(",Instruction fetch Cause Fault");
	}

	color_printk(RED,BLACK,"\n");
	serial_printk("\n");

	color_printk(RED,BLACK,"CR2:%#018lx\n",cr2);
	serial_printk("CR2:%#018lx\n",cr2);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_x87_FPU_error(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_x87_FPU_error(16),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_x87_FPU_error(16),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_alignment_check(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_alignment_check(17),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_alignment_check(17),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_machine_check(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_machine_check(18),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_machine_check(18),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_SIMD_exception(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_SIMD_exception(19),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_SIMD_exception(19),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_virtualization_exception(pt_regs_t * regs, uint64_t error_code)
{
	color_printk(RED,BLACK,"do_virtualization_exception(20),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_virtualization_exception(20),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_system_call(pt_regs_t *regs, uint64_t error_code __attribute__((unused)))
{
    serial_printk("hello from user (syscall %d from pid %d)\n",
                  regs->rax, current->pid);
}

void sys_vector_install()
{
    set_trap_gate(0,1,divide_error);
    set_trap_gate(1,1,debug);
	set_intr_gate(2,1,nmi);
	set_system_gate(3,1,int3);
	set_system_gate(4,1,overflow);
	set_system_gate(5,1,bounds);
	set_trap_gate(6,1,undefined_opcode);
	set_trap_gate(7,1,dev_not_available);
	set_trap_gate(8,3,double_fault);  // IST 3 = dedicated double fault stack
	set_trap_gate(9,1,coprocessor_segment_overrun);
	set_trap_gate(10,1,invalid_TSS);
	set_trap_gate(11,1,segment_not_present);
	set_trap_gate(12,1,stack_segment_fault);
	set_trap_gate(13,1,general_protection);
	set_trap_gate(14,1,page_fault);
	//15 Intel reserved. Do not use.
	set_trap_gate(16,1,x87_FPU_error);
	set_trap_gate(17,1,alignment_check);
	set_trap_gate(18,1,machine_check);
	set_trap_gate(19,1,SIMD_exception);
	set_trap_gate(20,1,virtualization_exception);

	// int 0x80 syscall gate — DPL=3 so user code can call it
	set_system_gate(0x80, 0, system_call);
}
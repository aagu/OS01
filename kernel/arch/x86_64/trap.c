#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/printk.h>
#include <kernel/trace.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/slab.h>
#include <driver/serial.h>
#include <errno.h>
#include <uapi/syscall.h>
#include <string.h>
#include <stdlib.h>
#include <fs/vfs.h>

// ── Helper: find the current task from TSS.rsp0 ──────────────
// Safe to call from IST exception stacks where get_current_task()
// (RSP masking) returns garbage.
static inline task_t *task_from_tss(void)
{
    percpu_t *cpu = this_cpu();
    if (!cpu || !cpu->tss)
        return NULL;
    uint64_t rsp0 = cpu->tss->rsp0;
    task_t *task = (task_t *)((rsp0 - 1) & ~(STACK_SIZE - 1));
    if ((uint64_t)task < 0xffff800000000000UL)
        return NULL;
    return task;
}

// Kill the user task that was running when a user-mode fault occurred.
// We are on the IST exception stack — get_current_task() is broken.
// Use TSS.rsp0 to locate the correct task, then overwrite the iretq
// frame to redirect execution to do_exit() on the task's kernel stack
// (where get_current_task() will work correctly).
static void kill_current_user_task(pt_regs_t *regs)
{
    task_t *task = task_from_tss();

    if (!task || (task->flags & PF_KTHREAD)) {
        serial_printk("User fault with no user task\n");
        return;
    }

    serial_printk("Killing task %d (user fault at RIP=%p)\n",
                  task->pid, regs->rip);

    // Mark the task ZOMBIE now, so the reaper can clean it up later.
    // The actual resource cleanup (vmm_free_user_map, kfree) happens
    // in do_exit() which we redirect to via iretq.
    task->state = TASK_ZOMBIE;

    // Overwrite the iretq target so the task "returns" to do_exit
    // running in ring 0 on its own kernel stack — where
    // get_current_task() works correctly.
    regs->rip  = (uint64_t)do_exit;
    regs->cs   = KERNEL_CS;
    regs->ss   = KERNEL_DS;
    regs->rsp  = (uint64_t)task + STACK_SIZE;  // task's kernel stack (NOT user RSP!)
    regs->ds   = KERNEL_DS;
    regs->es   = KERNEL_DS;
    regs->rdi  = 0;              // exit code for do_exit
    regs->rflags = (1 << 9);     // IF=1
}

// Check for user-mode fault and kill the task if so.  Returns 1 if
// the fault was user-mode (caller should return immediately), 0 if
// kernel-mode (caller should continue with kernel fault handling).
static inline int handle_user_fault(pt_regs_t *regs, const char *name)
{
    if (regs->cs & 3) {
        serial_printk("%s from user, killing task\n", name);
        kill_current_user_task(regs);
        return 1;
    }
    return 0;
}

void do_divide_error(pt_regs_t * regs, uint64_t error_code)
{
        if (handle_user_fault(regs, "do_divide_error")) return;
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
    if (handle_user_fault(regs, "do_overflow")) return;
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
    if (handle_user_fault(regs, "do_bounds")) return;
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
    if (handle_user_fault(regs, "do_undefined_opcode")) return;
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
    if (handle_user_fault(regs, "do_dev_not_available")) return;
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
    if (handle_user_fault(regs, "do_coprocessor_segment_overrun")) return;
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
    if (handle_user_fault(regs, "do_invalid_TSS")) return;
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
    if (handle_user_fault(regs, "do_segment_not_present")) return;
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
    if (handle_user_fault(regs, "do_stack_segment_fault")) return;
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
	// NOTE: on IST stack — do NOT use current (get_current_task).
	if (regs->cs & 3) {
		task_t *t = task_from_tss();
		serial_printk("do_general_protection(13) from user, killing task %d\n",
		              t ? t->pid : -1);
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
	// NOTE: on IST stack — do NOT use current (get_current_task).
	if (regs->cs & 3) {
		task_t *t = task_from_tss();
		serial_printk("do_page_fault(14) user err=%p rip=%p cr2=%p pid=%d\n",
		              error_code, regs->rip, cr2,
		              t ? t->pid : -1);
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
    if (handle_user_fault(regs, "do_x87_FPU_error")) return;
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
    if (handle_user_fault(regs, "do_alignment_check")) return;
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
    if (handle_user_fault(regs, "do_machine_check")) return;
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
    if (handle_user_fault(regs, "do_SIMD_exception")) return;
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
    if (handle_user_fault(regs, "do_virtualization_exception")) return;
	color_printk(RED,BLACK,"do_virtualization_exception(20),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	serial_printk("do_virtualization_exception(20),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

#define USER_CODE_ADDR 0x400000UL
#define USER_PAGE_SIZE 0x200000UL  // 2MB

void do_system_call(pt_regs_t *regs, uint64_t error_code __attribute__((unused)))
{
    switch (regs->rax) {
    case SYS_putchar: {
        // putchar(int c) — write one char to framebuffer
        char c = (char)regs->rdi;
        color_printk(WHITE, BLACK, "%c", c);
        regs->rax = (uint64_t)(unsigned char)c;
        break;
    }
    case SYS_write: {
        // write(const char *str, size_t len) — write string to framebuffer
        const char *str = (const char *)regs->rdi;
        // Bounds check: string must be in user-accessible memory
        if ((uint64_t)str >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        color_printk(WHITE, BLACK, "%s", str);
        regs->rax = 0;
        break;
    }
    case SYS_exit: {
        // exit(int code) — terminate current process
        do_exit(regs->rdi);
        // unreachable — do_exit calls schedule() which never returns
    }
    case SYS_brk: {
        // brk(void *addr) — set program break, return new break
        uint64_t addr = regs->rdi;
        mm_t *mm = current->mm;
        if (mm == NULL || mm->start_brk == 0) {
            regs->rax = -ENOMEM;
            break;
        }
        if (addr == 0) {
            // Query current break
            regs->rax = mm->end_brk;
            break;
        }
        if (addr < mm->start_brk) {
            regs->rax = -EINVAL;
            break;
        }
        // Safety: keep heap below the stack area within the user page
        if (addr > (USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000)) {
            regs->rax = -ENOMEM;
            break;
        }
        mm->end_brk = addr;
        regs->rax = addr;
        break;
    }
    case SYS_getpid: {
        regs->rax = current->pid;
        break;
    }
    case SYS_exec: {
        // exec(const char *path) — replace current process with ELF from file
        const char *path = (const char *)regs->rdi;
        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        // Copy path to kernel heap to avoid TOCTOU with user memory
        char *path_copy = strdup(path);
        if (!path_copy) {
            regs->rax = -ENOMEM;
            break;
        }

        int64_t ret = sys_exec(path_copy, regs);
        kfree(path_copy);
        regs->rax = ret;
        break;
    }
    case SYS_read: {
        // read(const char *path, void *buffer, size_t size)
        const char *path = (const char *)regs->rdi;
        void *buffer = (void *)regs->rsi;
        uint64_t size = regs->rdx;

        // Bounds check: both pointers must be in user-accessible memory
        if ((uint64_t)path >= current->addr_limit ||
            (uint64_t)buffer >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        // Copy path to kernel heap to avoid TOCTOU
        char *path_copy = strdup(path);
        if (!path_copy) {
            regs->rax = -ENOMEM;
            break;
        }

        vfs_node_t *node = vfs_lookup(path_copy);
        if (!node) {
            kfree(path_copy);
            regs->rax = -ENOENT;
            break;
        }

        // Read into kernel buffer first, then copy to user
        uint8_t kbuf[256];
        uint64_t to_read = size > 256 ? 256 : size;
        int64_t n = vfs_read(node, 0, to_read, kbuf);

        if (n > 0)
            memcpy(buffer, kbuf, (uint64_t)n);

        vfs_node_put(node);
        kfree(path_copy);
        regs->rax = n;
        break;
    }
    default:
        serial_printk("syscall: unknown nr=%d from pid=%d\n",
                      regs->rax, current->pid);
        regs->rax = -EINVAL;
        break;
    }
}

void sys_vector_install()
{
    set_trap_gate(0,1,divide_error);
    set_trap_gate(1,1,debug);
	set_intr_gate_raw(2, 1, nmi);
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
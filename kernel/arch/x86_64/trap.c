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
#include <kernel/file.h>

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
        // write(int fd, const void *buf, size_t len) — fd-based
        int fd = (int)regs->rdi;
        const void *buf = (const void *)regs->rsi;
        uint64_t size = regs->rdx;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }
        if ((uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];
        regs->rax = fd_write(f, buf, size);
        break;
    }
    case SYS_exit: {
        // exit(int code) — terminate current process
        current->exit_code = regs->rdi;
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
        // read(int fd, void *buf, size_t len) — fd-based
        int fd = (int)regs->rdi;
        void *buf = (void *)regs->rsi;
        uint64_t size = regs->rdx;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }
        if ((uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];
        regs->rax = fd_read(f, buf, size);
        break;
    }
    case SYS_open: {
        // open(const char *path, int flags) → fd
        const char *path = (const char *)regs->rdi;
        int flags = (int)regs->rsi;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        if (!current->files) {
            regs->rax = -ENFILE;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) {
            regs->rax = -ENOMEM;
            break;
        }

        vfs_node_t *node = vfs_lookup_from(path_copy, current->files->cwd);
        kfree(path_copy);

        // O_CREAT: create file if it doesn't exist
        if (!node && (flags & O_CREAT)) {
            // Find parent directory — parse the path to extract parent
            char parent_path[VFS_NAME_MAX];
            const char *name = NULL;

            // Copy path and find last '/'
            char pbuf[VFS_NAME_MAX];
            size_t plen = strlen(path);
            if (plen >= VFS_NAME_MAX) { regs->rax = -ENAMETOOLONG; break; }
            memcpy(pbuf, path, plen + 1);

            char *last_slash = NULL;
            for (char *s = pbuf; *s; s++)
                if (*s == '/') last_slash = s;

            if (last_slash && last_slash != pbuf) {
                // e.g., "/dir/file" — parent is "/dir", name is "file"
                *last_slash = '\0';
                name = last_slash + 1;
                strcpy(parent_path, pbuf);
            } else if (last_slash == pbuf && plen > 1) {
                // e.g., "/file" — parent is "/", name is "file"
                parent_path[0] = '/'; parent_path[1] = '\0';
                name = pbuf + 1;
            } else {
                // No slash — relative path, parent is cwd
                name = pbuf;
                // Use cwd as parent path
                size_t cwd_len = strlen(current->files->cwd);
                if (cwd_len >= VFS_NAME_MAX) { regs->rax = -ENAMETOOLONG; break; }
                memcpy(parent_path, current->files->cwd, cwd_len + 1);
            }

            if (!name || *name == '\0') { regs->rax = -EINVAL; break; }

            vfs_node_t *parent = vfs_lookup_from(parent_path, current->files->cwd);
            if (!parent) { regs->rax = -ENOENT; break; }
            if (parent->type != VFS_DIR) { vfs_node_put(parent); regs->rax = -ENOTDIR; break; }
            if (!parent->ops || !parent->ops->create) {
                vfs_node_put(parent);
                regs->rax = -EROFS;
                break;
            }

            node = parent->ops->create(parent, name);
            vfs_node_put(parent);
            if (!node) { regs->rax = -EEXIST; break; }
        }

        if (!node) {
            regs->rax = -ENOENT;
            break;
        }

        // O_TRUNC: truncate regular files to size 0.
        // The filesystem's write op will reallocate clusters as needed.
        if ((flags & O_TRUNC) && node->type == VFS_FILE) {
            node->size = 0;
            // Clear the first-cluster pointer so fat_write allocates fresh
            node->fs_data = NULL;
        }

        file_t *f = file_alloc();
        if (!f) {
            vfs_node_put(node);
            regs->rax = -ENOMEM;
            break;
        }

        f->type = FD_VFS;
        f->node = node;
        f->flags = flags;
        f->offset = 0;

        int newfd = fd_alloc(current->files, f);
        if (newfd < 0) {
            file_free(f);
            regs->rax = -ENFILE;
            break;
        }
        regs->rax = newfd;
        break;
    }
    case SYS_close: {
        // close(int fd) → 0 / -EBADF
        int fd = (int)regs->rdi;

        if (fd < 0 || fd >= NOFILE || !current->files || !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }
        fd_close(current->files, fd);
        regs->rax = 0;
        break;
    }
    case SYS_dup: {
        // dup(int oldfd) → newfd / -errno
        int oldfd = (int)regs->rdi;

        if (oldfd < 0 || oldfd >= NOFILE || !current->files ||
            !current->files->fd[oldfd]) {
            regs->rax = -EBADF;
            break;
        }

        // Find lowest free fd
        int newfd = -1;
        for (int i = 0; i < NOFILE; i++) {
            if (current->files->fd[i] == NULL) {
                newfd = i;
                break;
            }
        }
        if (newfd < 0) {
            regs->rax = -ENFILE;
            break;
        }

        file_t *f = current->files->fd[oldfd];
        f->refcount++;
        current->files->fd[newfd] = f;
        regs->rax = newfd;
        break;
    }
    case SYS_dup2: {
        // dup2(int oldfd, int newfd) → newfd / -errno
        int oldfd = (int)regs->rdi;
        int newfd = (int)regs->rsi;

        if (oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE ||
            !current->files || !current->files->fd[oldfd]) {
            regs->rax = -EBADF;
            break;
        }

        if (oldfd == newfd) {
            regs->rax = newfd;
            break;
        }

        // Close newfd first if open
        if (current->files->fd[newfd])
            fd_close(current->files, newfd);

        file_t *f = current->files->fd[oldfd];
        f->refcount++;
        current->files->fd[newfd] = f;
        regs->rax = newfd;
        break;
    }
    case SYS_fork: {
        // fork() → child PID in parent, 0 in child
        int64_t pid = do_fork(regs, 0, 0, 0);
        // Parent path: regs->rax = child PID
        // Child path: do_fork already set child's pt_regs->rax = 0
        regs->rax = pid;
        serial_printk("fork: pid=%d returned %d\n", (int)current->pid, (int)pid);
        break;
    }
    case SYS_waitpid: {
        // waitpid(pid, *status, options) → child PID or error
        int64_t pid = (int64_t)(int)regs->rdi;
        int *status = (int *)regs->rsi;
        int options = (int)regs->rdx;

        // Validate status pointer
        if (status && (uint64_t)status >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        regs->rax = do_waitpid(pid, status, options);
        break;
    }
    case SYS_pipe: {
        // pipe(int fds[2]) → 0 / -errno
        int *fds = (int *)regs->rdi;
        regs->rax = do_pipe(fds);
        break;
    }
    case SYS_chdir: {
        // chdir(const char *path) → 0 / -errno
        const char *path = (const char *)regs->rdi;
        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        if (!current->files) {
            regs->rax = -ENOENT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        vfs_node_t *node = vfs_lookup_from(path_copy, current->files->cwd);
        kfree(path_copy);
        if (!node) { regs->rax = -ENOENT; break; }
        if (node->type != VFS_DIR) { vfs_node_put(node); regs->rax = -ENOTDIR; break; }
        vfs_node_put(node);

        // Build the new absolute cwd
        char new_cwd[256];
        if (path[0] == '/') {
            // absolute
            size_t len = strlen(path);
            if (len >= 255) { regs->rax = -EINVAL; break; }
            memcpy(new_cwd, path, len + 1);
        } else {
            // relative: cwd + "/" + path
            int cwd_len = (int)strlen(current->files->cwd);
            int path_len = (int)strlen(path);
            if (cwd_len + 1 + path_len >= 256) { regs->rax = -EINVAL; break; }
            memcpy(new_cwd, current->files->cwd, cwd_len);
            new_cwd[cwd_len] = '/';
            memcpy(new_cwd + cwd_len + 1, path, path_len + 1);
        }
        // Collapse "//" and trailing "/"
        // For now, simple store
        memcpy(current->files->cwd, new_cwd, sizeof(current->files->cwd));

        serial_printk("chdir: pid=%d → '%s'\n", (int)current->pid, current->files->cwd);
        regs->rax = 0;
        break;
    }
    case SYS_getcwd: {
        // getcwd(char *buf, size_t size) → buf / NULL(-errno)
        char *buf = (char *)regs->rdi;
        uint64_t size = regs->rsi;
        if ((uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        if (!current->files) {
            regs->rax = -ENOENT;
            break;
        }
        uint64_t len = strlen(current->files->cwd) + 1;
        if (len > size) { regs->rax = -ERANGE; break; }
        memcpy((void *)buf, current->files->cwd, len);
        regs->rax = (int64_t)(uint64_t)buf;  // success: return buf
        break;
    }
    default:
        serial_printk("syscall: unknown nr=%d from pid=%d\n",
                      (int)regs->rax, (int)current->pid);
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
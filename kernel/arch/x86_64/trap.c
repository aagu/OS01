#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/irq.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/hw.h>
#include <kernel/arch/segment.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/printk.h>
#include <kernel/log.h>
#include <kernel/trace.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/task.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/percpu.h>
#include <kernel/apic.h>
#include <kernel/slab.h>
#include <driver/serial.h>
#include <errno.h>
#include <uapi/syscall.h>
#include <uapi/stat.h>
#include <string.h>
typedef int pid_t;
#include <termios.h>
#include <kernel/tty.h>
#include <stdlib.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <kernel/debug.h>
#include <kernel/file.h>
#include <kernel/poll.h>     // struct pollfd, do_poll()
#include <kernel/select.h>   // sigset_t, do_select(), do_pselect6()
#include <device/timer.h>
#include <kernel/clocksource.h>  // clocksource_read_ns()
#include <uapi/time.h>
#include <kernel.h>
#include <kernel/vma.h>
#include <sys/random.h>   // GRND_NONBLOCK, GRND_RANDOM (for SYS_getrandom)
#include <kernel/random.h>  // get_random_bytes(), RANDOM_MAX_LEN
#include <uapi/futex.h>
#include <kernel/futex.h>
#include <uapi/sockaddr.h>  // struct sockaddr_in (shared with userspace)
#include <net/socket.h>     // do_socket, do_connect, etc.
// ── Local signal constants (kernel has its own signal.h) ──
#ifndef SIG_BLOCK
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2
#endif

// ── User address translation ─────────────────────────────────
// Walk the user page table to resolve a user-space virtual
// address to its physical address.  Returns 0 on failure.
// The caller passes Phy_To_Virt(result) to get a kernel pointer.
uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va)
{
    size_t l4 = (va >> PAGE_GDT_SHIFT) & 0x1ff;
    size_t l3 = (va >> PAGE_1G_SHIFT) & 0x1ff;
    size_t l2 = (va >> PAGE_2M_SHIFT) & 0x1ff;
    if (!(pml4[l4] & PAGE_Present)) return 0;
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);
    if (!(pml3[l3] & PAGE_Present)) return 0;
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);
    if (!(pml2[l2] & PAGE_Present)) return 0;
    return (pml2[l2] & PAGE_2M_MASK & ~PAGE_XD) | (va & 0x1FFFFF);
}

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
        log_err("User fault with no user task\n");
        return;
    }

    log_err("Killing task %d (user fault at RIP=%p)\n",
            task->pid, regs->rip);

    // Mark the task ZOMBIE now, so a waiter (do_waitpid) can reap it
    // later. The actual resource cleanup (vmm_free_user_map, kfree)
    // happens in do_exit() which we call directly.
    task->state = TASK_ZOMBIE;

    // Switch to the task's own kernel stack and call do_exit.
    // We CANNOT use iretq for a ring-0 return here.  When iretq
    // keeps the same privilege level (ring 0 → ring 0) it does
    // NOT pop RSP/SS from the frame, so we would remain on the
    // IST exception stack.  get_current_task() (RSP masking)
    // would return garbage, causing memory corruption or a crash.
    //
    // Instead, switch RSP to the task's kernel stack directly and
    // call do_exit.  RSP is set to (task + STACK_SIZE - 8) so that
    // get_current_task() returns the correct task struct (RSP
    // masking rounds down to the STACK_SIZE-aligned base).
    // The -8 also mimics the stack state after a normal function
    // call (return address below the top) so do_exit's prologue
    // and sub-functions work correctly.  Since do_exit() calls
    // schedule() (which context-switches away and never returns),
    // the compiler is told this path is unreachable.
    __asm__ __volatile__(
        "movq %[stack_top], %%rsp\n\t"
        "xorl %%edi, %%edi\n\t"         // rdi = 0 (exit code)
        "call do_exit\n\t"
        :
        : [stack_top] "r"((uint64_t)task + STACK_SIZE - 8)
        : "edi", "memory"
    );
    __builtin_unreachable();
}

// Check for user-mode fault and kill the task if so.  Returns 1 if
// the fault was user-mode (caller should return immediately), 0 if
// kernel-mode (caller should continue with kernel fault handling).
static inline int handle_user_fault(pt_regs_t *regs, const char *name)
{
    if (regs->cs & 3) {
        log_err("%s from user, killing task\n", name);
        kill_current_user_task(regs);
        return 1;
    }
    return 0;
}

void do_divide_error(pt_regs_t * regs, uint64_t error_code)
{
        if (handle_user_fault(regs, "do_divide_error")) return;
    log_err("do_divide_error(0),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_debug(pt_regs_t * regs, uint64_t error_code)
{
	log_err("do_debug(1),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_nmi(pt_regs_t * regs, uint64_t error_code)
{
	log_err("do_nmi(2),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_int3(pt_regs_t * regs, uint64_t error_code)
{
	log_err("do_int3(3),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_overflow(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_overflow")) return;
	log_err("do_overflow(4),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_bounds(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_bounds")) return;
	log_err("do_bounds(5),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_undefined_opcode(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_undefined_opcode")) return;
	log_err("do_undefined_opcode(6),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_dev_not_available(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_dev_not_available")) return;
	log_err("do_dev_not_available(7),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_double_fault(pt_regs_t * regs, uint64_t error_code)
{
	log_err("do_double_fault(8),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_coprocessor_segment_overrun(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_coprocessor_segment_overrun")) return;
	log_err("do_coprocessor_segment_overrun(9),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_invalid_TSS(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_invalid_TSS")) return;
	log_err("do_invalid_TSS(10),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		log_info("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		log_info("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		log_info("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			log_info("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			log_info("Refers to a descriptor in the current GDT;\n");
		}
	}
	log_info("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_segment_not_present(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_segment_not_present")) return;
	log_err("do_segment_not_present(11),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		log_info("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		log_info("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		log_info("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			log_info("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			log_info("Refers to a descriptor in the current GDT;\n");
		}
	}
	log_info("Segment Selector Index:%#010x\n",error_code & 0xfff8);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_stack_segment_fault(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_stack_segment_fault")) return;
	log_err("do_stack_segment_fault(12),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(error_code & 0x01)
	{
		log_info("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		log_info("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		log_info("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			log_info("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			log_info("Refers to a descriptor in the current GDT;\n");
		}
	}
	log_info("Segment Selector Index:%#010x\n",error_code & 0xfff8);
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
		log_err("do_general_protection(13) from user, killing task %d\n",
		        t ? t->pid : -1);
		kill_current_user_task(regs);
		return;
	}
	log_err("do_general_protection(13),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	log_err(" GPR: RAX=%#018lx RBX=%#018lx RCX=%#018lx RDX=%#018lx\n",
	        regs->rax, regs->rbx, regs->rcx, regs->rdx);
	log_err(" GPR: RSI=%#018lx RDI=%#018lx RBP=%#018lx R8=%#018lx\n",
	        regs->rsi, regs->rdi, regs->rbp, regs->r8);
	log_err(" GPR: R9=%#018lx R10=%#018lx R11=%#018lx R12=%#018lx\n",
	        regs->r9, regs->r10, regs->r11, regs->r12);
	log_err(" GPR: R13=%#018lx R14=%#018lx R15=%#018lx\n",
	        regs->r13, regs->r14, regs->r15);
	log_err(" CS=%#04lx SS=%#04lx EFLAGS=%#018lx CR2=%#018lx\n",
	        regs->cs, regs->ss, regs->rflags, ({ uint64_t v; __asm__ __volatile__("movq %%cr2, %0" : "=r"(v) :: "memory"); v; }));


	if(error_code & 0x01)
	{
		log_info("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");
	}

	if(error_code & 0x02)
	{
		log_info("Refers to a gate descriptor in the IDT;\n");
	}
	else
	{
		log_info("Refers to a descriptor in the GDT or the current LDT;\n");
	}

	if((error_code & 0x02) == 0)
	{
		if(error_code & 0x04)
		{
			log_info("Refers to a segment or gate descriptor in the LDT;\n");
		}
		else
		{
			log_info("Refers to a descriptor in the current GDT;\n");
		}
	}
	log_info("Segment Selector Index:%#010x\n",error_code & 0xfff8);
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

	// Detect kernel-mode PF (IST 0 = task stack)
	if (!(regs->cs & 3)) {
		task_t *t = task_from_tss();
		serial_printk("PF-KRN: err=%lx rip=%lx rsp=%lx rbp=%lx cr2=%lx "
			"pid=%d cpu=%d\n",
			error_code, regs->rip, regs->rsp, regs->rbp, cr2,
			t?t->pid:-1, cpu_id());

		// TASKLIST dump below covers every live task (pid/state/
		// on_rq/on_cpu/cpu/stack) — the UAF victim is identifiable
		// even when the crashing task_t is already reclaimed.

		// TASKLIST FIRST: full task inventory before touching t (which
		// may itself be a freed/garbage pointer).  Prints every live
		// task's pid/state/stack so the UAF victim is identifiable even
		// when the crashing task_t is already reclaimed.
		{
			uint64_t head = (uint64_t)&init_task_union.task.list;
			uint64_t pos = *(uint64_t *)head;  // list.next
			int n = 0;
			serial_printk("PF-KRN: tasklist head=%lx\n", head);
			// Circular list: stop when we return to head.  Do NOT use
			// address ordering (head is lower than heap nodes).
			while (pos != head && n < 32) {
				if (pos < 0xffff800000000000ULL ||
				    pos > 0xffff800020000000ULL) {
					serial_printk("PF-KRN:   tasklist corrupt pos=%lx\n", pos);
					break;
				}
				task_t *tl = container_of((list_t *)pos, task_t, list);
				uint64_t stk = (uint64_t)tl->stack_alloc_base;
				serial_printk("PF-KRN:   task pid=%ld st=%d fl=%lx cpu=%u on_rq=%u on_cpu=%u thd=%p stk=%lx\n",
					(long)tl->pid, (int)tl->state, (unsigned long)tl->flags,
					(unsigned)tl->cpu, (unsigned)tl->on_rq, (unsigned)tl->on_cpu,
					(void *)tl->thread, stk);
				pos = *(uint64_t *)pos;  // next
				n++;
			}
		}
		serial_printk("PF-KRN: r8=%lx r9=%lx r10=%lx r11=%lx\n",
			regs->r8, regs->r9, regs->r10, regs->r11);
		serial_printk("PF-KRN: r12=%lx r13=%lx r14=%lx r15=%lx\n",
			regs->r12, regs->r13, regs->r14, regs->r15);
		serial_printk("PF-KRN: rax=%lx rbx=%lx rcx=%lx rdx=%lx\n",
			regs->rax, regs->rbx, regs->rcx, regs->rdx);
		serial_printk("PF-KRN: rdi=%lx rsi=%lx cs=%lx ss=%lx\n",
			regs->rdi, regs->rsi, regs->cs, regs->ss);
		if (t) {
			serial_printk("PF-KRN: task sig=%lx blk=%lx st=%d fl=%lx\n",
				t->signal, t->blocked, t->state, t->flags);
			serial_printk("PF-KRN: thd rsp=%lx rsp0=%lx rip=%lx\n",
				t->thread->rsp, t->thread->rsp0, t->thread->rip);
			// Dump 8 bytes at saved RSP to see what schedule() saved
			uint64_t saved_rsp = t->thread->rsp;
			if (saved_rsp >= 0xffff800000000000ULL)
				serial_printk("PF-KRN: *thd_rsp=%lx %lx\n",
					*(uint64_t *)saved_rsp,
					*((uint64_t *)saved_rsp + 1));
			// Dump 16 qwords around the saved rsp to reconstruct the
			// return chain that led to the bad RIP.
			if (saved_rsp >= 0xffff800000000000ULL) {
				uint64_t *p = (uint64_t *)(saved_rsp & ~0x7ULL);
				serial_printk("PF-KRN: thd_rsp-8..+120:\n");
				for (int i = -1; i < 15; i++) {
					uint64_t addr = (uint64_t)p + i * 8;
					if (addr < 0xffff800000000000ULL ||
					    addr > 0xffff800020000000ULL)
						break;
					serial_printk("PF-KRN:   [%+d] %p: %lx\n",
						(i * 8), (void *)addr,
						*(uint64_t *)addr);
				}
			}
			// Also dump 8 qwords at the crash rsp.
			{
				uint64_t cr = regs->rsp;
				if (cr >= 0xffff800000000000ULL &&
				    cr <= 0xffff800020000000ULL) {
					serial_printk("PF-KRN: crsp dump:\n");
					for (int i = 0; i < 8; i++) {
						uint64_t addr = cr + i * 8;
						if (addr > 0xffff800020000000ULL) break;
						serial_printk("PF-KRN:   crsp[%d] %p: %lx\n",
							i, (void *)addr, *(uint64_t *)addr);
					}
				}
			}
		}
		// Check stack boundaries
		uint64_t sb = regs->rsp & ~(STACK_SIZE - 1);
		serial_printk("PF-KRN: stack %lu/%lu used\n",
			((sb + STACK_SIZE) - regs->rsp), (uint64_t)STACK_SIZE);

		// Raw rbp-chain backtrace: return addresses + frame pointers.
		// Walk up to 12 frames; stop on unmapped/loop pointers.
		{
			uint64_t fp = regs->rbp;
			serial_printk("PF-KRN: bt rbp=%lx rip=%lx\n", fp, regs->rip);
			for (int i = 0; i < 12; i++) {
				if (fp < 0xffff800000000000ULL ||
				    fp > 0xffff800020000000ULL)
					break;
				uint64_t *fr = (uint64_t *)fp;
				uint64_t ret = fr[1];
				uint64_t next_fp = fr[0];
				serial_printk("PF-KRN:   #%d fp=%lx ret=%lx\n",
					i, fp, ret);
				if (next_fp <= fp) break;  // loop or forward
				fp = next_fp;
			}
		}
	}

	// User-mode PF - VMA-based demand paging
	// NOTE: on IST stack - do NOT use current (get_current_task).
	if (regs->cs & 3) {
		task_t *t = task_from_tss();
		if (!t || !t->mm) {
			kill_current_user_task(regs);
			return;
		}

		vma_t *vma = vma_find(t->mm, cr2);
		if (!vma) {
			log_debug("PF: pid=%d cr2=%p no vma\n", t->pid, cr2);
			kill_current_user_task(regs);
			return;
		}

		// -- Permission check --
		// error_code: bit 0=P, bit 1=W/R, bit 4=I/D

		// PROT_NONE VMA -> any access is SIGSEGV
		if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))) {
			log_debug("PF: pid=%d cr2=%p PROTNONE\n", t->pid, cr2);
			kill_current_user_task(regs);
			return;
		}

		// Write protection violation (P=1, W=1)
		if ((error_code & 0x03) == 0x03 && !(vma->vm_flags & VM_WRITE)) {
			log_debug("PF: pid=%d cr2=%p write to RO page\n",
			          t->pid, cr2);
			kill_current_user_task(regs);
			return;
		}

		// Instruction fetch (I=1)
		if ((error_code & 0x10) && !(vma->vm_flags & VM_EXEC)) {
			kill_current_user_task(regs);
			return;
		}
		// -- COW resolution (P=1, W=1, VM_WRITE is set) --
		if ((error_code & 0x03) == 0x03) {
			uint64_t *user_pml4 =
			    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pml4);
			uint64_t *pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
			if (pte && (*pte & PAGE_COW)) {
				uint64_t old_phys = *pte & PAGE_4K_MASK;
				if (page_cow_refs(old_phys) > 1) {
					uint64_t new_phys = alloc_4k_page();
					if (!new_phys) {
						kill_current_user_task(regs);
						return;
					}
					memcpy((void *)Phy_To_Virt(new_phys),
					       (void *)Phy_To_Virt(old_phys),
					       PAGE_4K_SIZE);
					*pte = new_phys | vma->vm_page_prot;
					page_cow_put(old_phys);
				} else {
					(void)page_cow_put(old_phys);
					*pte = old_phys | vma->vm_page_prot;
				}
				flush_tlb();
				return;
			}
		}

		// -- Page not present (P=0) - demand allocation --
		if (!(error_code & 0x01)) {
			uint64_t *user_pml4 =
			    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pml4);

			if (vma->vm_flags & VM_ANON) {
				uint64_t phys = alloc_4k_page();
				if (!phys) {
					log_debug("PF: pid=%d OOM\n", t->pid);
					kill_current_user_task(regs);
					return;
				}
				int rc = vmm_map_4k_page(user_pml4, phys,
							     PAGE_4K_ALIGN(cr2), vma->vm_page_prot);
				if (rc != 0) {
					free_4k_page(phys);
					kill_current_user_task(regs);
					return;
				}
				return;
			}

			if (vma->vm_file && !(vma->vm_flags & VM_IO)) {
				uint64_t phys = alloc_4k_page();
				if (!phys) {
					kill_current_user_task(regs);
					return;
				}
				uint64_t file_off =
				    (cr2 - vma->vm_start)
				    + (vma->vm_pgoff << PAGE_4K_SHIFT);
				int n = vfs_read(vma->vm_file, file_off,
						  PAGE_4K_SIZE,
						  (void *)Phy_To_Virt(phys));
				if (n < 0) {
					free_4k_page(phys);
					kill_current_user_task(regs);
					return;
				}
				// Zero-fill tail to avoid leaking kernel data
				if ((size_t)n < PAGE_4K_SIZE)
				    memset((char *)Phy_To_Virt(phys) + n, 0,
				           PAGE_4K_SIZE - (size_t)n);
				int rc = vmm_map_4k_page(user_pml4, phys,
							     PAGE_4K_ALIGN(cr2), vma->vm_page_prot);
				if (rc != 0) {
					free_4k_page(phys);
					kill_current_user_task(regs);
					return;
				}
				return;
			}
		}

		// Unhandled -> SIGSEGV
		kill_current_user_task(regs);
		return;
	}
	log_err("do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);

	if(!(error_code & 0x01))
	{
		log_info("Page Not-Present,\t");
	}

	if(error_code & 0x02)
	{
		log_info("Write Cause Fault,\t");
	}
	else
	{
		log_info("Read Cause Fault,\t");
	}

	if(error_code & 0x04)
	{
		log_info("Fault in user(3)\t");
	}
	else
	{
		log_info("Fault in supervisor(0,1,2)\t");
	}

	if(error_code & 0x08)
	{
		log_info(",Reserved Bit Cause Fault\t");
	}

	if(error_code & 0x10)
	{
		log_info(",Instruction fetch Cause Fault");
	}

	log_info("\n");

	log_info("CR2:%#018lx\n",cr2);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_x87_FPU_error(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_x87_FPU_error")) return;
	log_err("do_x87_FPU_error(16),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_alignment_check(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_alignment_check")) return;
	log_err("do_alignment_check(17),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_machine_check(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_machine_check")) return;
	log_err("do_machine_check(18),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_SIMD_exception(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_SIMD_exception")) return;
	log_err("do_SIMD_exception(19),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

void do_virtualization_exception(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_virtualization_exception")) return;
	log_err("do_virtualization_exception(20),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->rsp, regs->rip);
	backtrace(regs);
	while(1)
    {
        hlt();
    }
}

#define USER_CODE_ADDR 0x400000UL
#define USER_PAGE_SIZE 0x200000UL  // 2MB

// ── Signal delivery ──────────────────────────────────────────
// Dispatch pending signals for current.  Called from:
//   - do_system_call (after every syscall returning to ring 3)
//   - ret_from_intr  (after every interrupt returning to ring 3)
//
// Processes signals in order (1..NSIG-1).  SIG_DFL behaviour:
//   ignore  → SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGTSTP/TTIN/TTOU
//   kill    → everything else (do_exit, never returns)
//
// Registered handlers clear the pending bit but don't yet
// deliver to user space (future work).

int arch_do_signal_delivery(pt_regs_t *regs)
{
    uint64_t pending = current->signal;
    if (!pending)
        return 0;

    // ── NULL regs path (tty.c inline signal clear) ─────────
    // tty.c calls arch_do_signal_delivery(NULL) when a direct
    // switch bypassed ret_from_intr.  Only non-fatal signals
    // can be pending here.  Handle SIG_IGN + non-fatal SIG_DFL;
    // registered handlers are left pending.
    if (!regs) {
        for (int sig = 1; sig < NSIG; sig++) {
            if (!(pending & (1ULL << sig)))
                continue;
            void (*handler)(int) = current->sighand[sig].sa_handler;
            if (handler == SIG_IGN) {
                current->signal &= ~(1ULL << sig);
                continue;
            }
            if (handler == SIG_DFL) {
                switch (sig) {
                case SIGCHLD: case SIGURG: case SIGWINCH:
                case SIGCONT: case SIGTSTP: case SIGTTIN: case SIGTTOU:
                    current->signal &= ~(1ULL << sig);
                    break;
                default:
                    // PID 1 is special: ignore fatal signals
                    if (current->pid == 1) {
                        current->signal &= ~(1ULL << sig);
                        break;
                    }
                    current->signal &= ~(1ULL << sig);
                    do_exit((uint64_t)sig);
                    return 1;  // unreachable
                }
            }
        }
        return 0;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1ULL << sig)))
            continue;

        // Skip blocked signals (sigset uses BSD numbering: bit N-1 = signal N)
        if (sig != SIGKILL && sig != SIGSTOP &&
            current->blocked & (1ULL << (sig - 1)))
            continue;

        void (*handler)(int) = current->sighand[sig].sa_handler;

        if (handler == SIG_IGN) {
            current->signal &= ~(1ULL << sig);
            continue;
        }

        if (handler == SIG_DFL) {
            current->signal &= ~(1ULL << sig);
            switch (sig) {
            case SIGCHLD: case SIGURG: case SIGWINCH:
            case SIGCONT:
                break;   // ignore by default
            case SIGTSTP: case SIGTTIN: case SIGTTOU:
                break;   // stop — not implemented
            default:
                // PID 1 is special: ignore signals that would
                // otherwise kill, matching Linux behaviour.
                // Init must explicitly register handlers for
                // signals it wants to receive (SIGUSR1, SIGUSR2, SIGTERM).
                if (current->pid == 1)
                    break;   // ignore for init
                log_err("task %d killed by signal %d (default)\n",
                        (int)current->pid, sig);
                do_exit((uint64_t)sig);
                return 1;  // unreachable — do_exit switches away
            }
            continue;
        }

        // ── Registered handler ─────────────────────────────
        uint64_t restorer = (uint64_t)current->sighand[sig].sa_restorer;

        // CPL guard: only deliver to ring-3 frames
        if (!(regs->cs & 3)) {
            continue;  // leave pending, retry on next return-to-userspace
        }

        // sa_restorer NULL guard
        if (!restorer) {
            log_err("task %d: signal %d handler has no restorer, "
                    "killing\n", (int)current->pid, sig);
            current->signal &= ~(1ULL << sig);
            do_exit((uint64_t)sig);
            return 1;  // unreachable
        }

        // 1. Build sigframe on kernel stack
        struct sigframe frame;
        memset(&frame, 0, sizeof(frame));
        frame.r15=regs->r15; frame.r14=regs->r14; frame.r13=regs->r13;
        frame.r12=regs->r12; frame.r11=regs->r11; frame.r10=regs->r10;
        frame.r9=regs->r9;   frame.r8=regs->r8;
        frame.rbx=regs->rbx; frame.rcx=regs->rcx; frame.rdx=regs->rdx;
        frame.rsi=regs->rsi; frame.rdi=regs->rdi; frame.rbp=regs->rbp;
        frame.ds=regs->ds;   frame.es=regs->es;   frame.rax=regs->rax;
        frame.rip=regs->rip; frame.cs=regs->cs;   frame.rflags=regs->rflags;
        frame.rsp=regs->rsp; frame.ss=regs->ss;
        frame.blocked = current->blocked;

        // 2. Compute aligned user RSP (SysV ABI: RSP%16==8 after iretq)
        size_t total = sizeof(frame) + 8;  // 200 + 8 = 208
        uint64_t new_rsp = ((regs->rsp - total - 8) & ~15UL) + 8;

        // 3. Translate user stack VA → kernel pointer
        uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
        uint64_t frame_phys = user_va_to_phys(user_pml4, new_rsp + 8);
        if (!frame_phys) {
            continue;  // leave pending, retry
        }
        void *kstack = (void *)Phy_To_Virt(frame_phys);

        // 4. Write sigframe + trampoline return address to user stack
        memcpy(kstack, &frame, sizeof(frame));           // sigframe at new_rsp+8
        uint64_t tramp = restorer;
        memcpy(kstack - 8, &tramp, 8);                   // trampoline at new_rsp

        // 5. Rewrite pt_regs → RESTORE_ALL → iretq → handler
        regs->rdi = sig;
        regs->rip = (uint64_t)handler;
        regs->rsp = new_rsp;
        regs->cs  = ARCH_USER_CS;  // 0x2b: ring 3 code (GDT index 5 | RPL 3)
        regs->ss  = ARCH_USER_DS;  // 0x33: ring 3 data (GDT index 6 | RPL 3)
        regs->ds  = ARCH_USER_DS;
        regs->es  = ARCH_USER_DS;

        // 6. Block signal during handler execution
        current->blocked |= (1ULL << (sig - 1));
        current->blocked |= current->sighand[sig].sa_mask;

        current->signal &= ~(1ULL << sig);
        return 1;  // handler delivered
    }
    return 0;  // nothing deliverable
}

int arch_signal_pending_fatal(void)
{
    uint64_t pending = current->signal;
    if (!pending)
        return false;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1ULL << sig)))
            continue;
        if (current->sighand[sig].sa_handler != SIG_DFL)
            continue;
        switch (sig) {
        case SIGCHLD: case SIGURG: case SIGWINCH:
        case SIGCONT: case SIGTSTP: case SIGTTIN: case SIGTTOU:
            continue;
        default:
            return true;
        }
    }
    return false;
}

static inline bool syscall_user_range_ok(uint64_t addr, uint64_t len)
{
    return addr != 0 && addr < current->addr_limit &&
           len <= current->addr_limit - addr;
}

// ── nanosleep blocker condition ────────────────────────────
// Condition callback for blocker_wait(): true once the sleep deadline
// (current->wakeup_ns) has been reached.  sched_unblock_blocked()
// runs this from every schedule() (i.e. every tick) and wakes the
// sleeping task when it returns true.
static bool nanosleep_should_unblock(struct task_struct *waiter)
{
    return clocksource_read_ns() >= waiter->wakeup_ns;
}

void do_system_call(pt_regs_t *regs, uint64_t error_code __attribute__((unused)))
{
#ifndef NDEBUG
    // Stack overflow guard: warn when RSP is within 2KB of stack bottom.
    // Keep this for at least one release cycle to catch regressions.
    task_t *cur = get_current_task();
    if (cur) {
        uint64_t stack_bottom = ((uint64_t)cur) & ~(STACK_SIZE - 1);
        if ((uint64_t)__builtin_frame_address(0) - stack_bottom < 2048)
            log_err("WARNING: RSP within 2KB of stack bottom! pid=%d\n",
                    (int)cur->pid);
    }
#endif

    // Linux x86_64 ABI translation (for busybox etc.)
    if ((current->flags & PF_LINUX_ABI) && regs->rax < 320) {
        static const int8_t linux_to_os01[320] = {
            [0] = 6,   // read -> SYS_read
            [1] = 1,   // write -> SYS_write (same)
            [2] = 7,   // open -> SYS_open
            [3] = 8,   // close -> SYS_close
            [4] = 16,  // stat -> SYS_stat
            [5] = 17,  // fstat -> SYS_fstat
            [6] = -1,  // lstat -> unsupported
            [8] = 18,  // lseek -> SYS_lseek
            [9]  = 44,  // mmap
            [10] = 45,  // mprotect
            [11] = 46,  // munmap
            [12] = 3,  // brk -> SYS_brk
            [13] = 39, // rt_sigaction -> SYS_signal
            [14] = 42, // sigprocmask -> SYS_sigprocmask
            [16] = 20, // ioctl -> SYS_ioctl
            [21] = 22, // access -> SYS_access
            [25] = -1, // mremap -> unsupported
            [32] = 9,  // dup -> SYS_dup
            [33] = 10, // dup2 -> SYS_dup2
            [35] = 31, // nanosleep -> SYS_nanosleep
            [39] = 4,  // getpid -> SYS_getpid
            [56] = 11, // clone -> SYS_fork
            [57] = 11, // fork -> SYS_fork
            [59] = 5,  // execve -> SYS_exec
            [60] = 2,  // _exit/exit_group -> SYS_exit
            [61] = 12, // wait4 -> SYS_waitpid
            [62] = 38, // kill -> SYS_kill
            [63] = 35, // uname -> SYS_uname
            [79] = 15, // getcwd -> SYS_getcwd
            [80] = 14, // chdir -> SYS_chdir
            [83] = 23, // unlink -> SYS_unlink (Linux: 87)
            [84] = 24, // mkdir -> SYS_mkdir (Linux: 83)
            [85] = 23, // unlink -> SYS_unlink (Linux 85 = rmdir on some)
            [86] = 25, // rmdir -> SYS_rmdir
            [87] = 23, // unlink -> SYS_unlink
            [89] = 26, // readlink -> SYS_? (Linux 89)
            [102] = 36,// getppid -> SYS_getppid (Linux: 110? no, 102)
            [110] = 36,// getppid -> SYS_getppid
            [162] = 31,// nanosleep -> SYS_nanosleep
            [201] = 34,// times -> SYS_times
            [217] = 21,// getdents64 -> SYS_getdents64
            [231] = 2, // exit_group -> SYS_exit

		// Socket syscalls (Phase 10 networking)
		[41] = 52,	// socket	→ SYS_socket
		[42] = 54,	// connect	→ SYS_connect
		[43] = 56,	// accept	→ SYS_accept
		[44] = 57,	// sendto	→ SYS_sendto
		[45] = 58,	// recvfrom	→ SYS_recvfrom
		[49] = 53,	// bind	→ SYS_bind
		[50] = 55,	// listen	→ SYS_listen
		[51] = 61,	// getsockname	→ SYS_getsockname
		[54] = 59,	// setsockopt	→ SYS_setsockopt
		[55] = 60,	// getsockopt	→ SYS_getsockopt
		[48] = 64,	// shutdown	→ SYS_shutdown
		[164] = 63,	// getifaddr	→ SYS_getifaddr
		[228] = 65,	// clock_gettime	→ SYS_clock_gettime
		[318] = 66,	// getrandom	→ SYS_getrandom
        };
        int8_t os = linux_to_os01[regs->rax];
        // `> 0` not `>= 0`: no Linux syscall in the table maps to OS01
        // putchar (0), so os == 0 always means "zero-filled unmapped entry"
        // and must fall through untranslated -> switch default -> -EINVAL
        // (the kernel's default for any unknown syscall; Linux's -ENOSYS
        // convention for the ABI path is a separate pre-existing gap);
        // os == -1 is the explicit unsupported sentinel (also falls through).
        if (os > 0)
            regs->rax = os;
    }
    switch (regs->rax) {
    // ── Syscall name table (for strace) ─────────────────────
    static const char *syscall_names[71] = {
        [0]  = "putchar",
        [1]  = "write",
        [2]  = "exit",
        [3]  = "brk",
        [4]  = "getpid",
        [5]  = "exec",
        [6]  = "read",
        [7]  = "open",
        [8]  = "close",
        [9]  = "dup",
        [10] = "dup2",
        [11] = "fork",
        [12] = "waitpid",
        [13] = "signal",
        [14] = "chdir",
        [15] = "getcwd",
        [16] = "stat",
        [17] = "fstat",
        [18] = "lseek",
        [19] = "mkdir",
        [20] = "ioctl",
        [21] = "getdents64",
        [22] = "access",
        [23] = "unlink",
        [24] = "mkdir",
        [25] = "rmdir",
        [26] = "readlink",
        [27] = "rename",
        [31] = "nanosleep",
        [34] = "times",
        [35] = "uname",
        [36] = "getppid",
        [38] = "kill",
        [39] = "rt_sigaction",
        [42] = "sigprocmask",
        [43] = "sigreturn",
        [45] = "poweroff",
        [47] = "futex",
        [48] = "poll",
        [49] = "ppoll",
        [50] = "select",
        [51] = "pselect6",
        [52] = "socket",
        [53] = "bind",
        [54] = "connect",
        [55] = "listen",
        [56] = "accept",
        [57] = "sendto",
        [58] = "recvfrom",
        [59] = "setsockopt",
        [60] = "getsockopt",
        [61] = "getsockname",
        [62] = "getpeername",
        [63] = "getifaddr",
        [64] = "shutdown",
        [65] = "clock_gettime",
        [66] = "getrandom",
        [67] = "setpgid",
        [68] = "getpgid",
        [69] = "setsid",
        [70] = "getsid",
    };
    const char *sname = (regs->rax < 71 && syscall_names[regs->rax])
                        ? syscall_names[regs->rax] : "?";
    (void)sname;
    debug_syscall("[strace] pid=%d syscall(%s, arg1=%#lx, arg2=%#lx, arg3=%#lx)\n",
                  (int)current->pid, sname,
                  (unsigned long)regs->rdi,
                  (unsigned long)regs->rsi,
                  (unsigned long)regs->rdx);
    case SYS_putchar: {
        // putchar(int c) — write one char to framebuffer AND serial
        char c = (char)regs->rdi;
        color_printk(WHITE, BLACK, "%c", c);
        {
            uint64_t sf = spin_lock_irqsave(&serial_lock);
            write_serial(c);  // also echo to serial for interactive shell
            spin_unlock_irqrestore(&serial_lock, sf);
        }
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
        // exit(int code) — terminate current process.
        // Encode as Linux does: exit code in the high byte (code<<8),
        // so waitpid() status can distinguish a normal exit (WIFEXITED)
        // from a signal death (low byte = signal → WIFSIGNALED).
        uint64_t code = regs->rdi & 0xFF;
        current->exit_code = code << 8;
        do_exit(code << 8);
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
        // exec(const char *path, char *const argv[], char *const envp[])
        // If argv == NULL: old behavior (no args)
        // If argv != NULL: copy argv/envp strings to new user stack
        const char *path = (const char *)regs->rdi;
        const char *const *argv = (const char *const *)regs->rsi;
        const char *const *envp = (const char *const *)regs->rdx;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        // Validate argv pointer if non-NULL
        if (argv != NULL && (uint64_t)argv >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        // Validate envp pointer if non-NULL
        if (envp != NULL && (uint64_t)envp >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        // Copy path to kernel heap to avoid TOCTOU with user memory
        char *path_copy = strdup(path);
        if (!path_copy) {
            regs->rax = -ENOMEM;
            break;
        }

        int64_t ret = sys_exec(path_copy, regs, argv, envp);
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
            if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->create) {
                vfs_node_put(parent);
                regs->rax = -EROFS;
                break;
            }
            if ((uint64_t)parent->ops->create < 0xffff800000000000ULL) {
                vfs_node_put(parent);
                regs->rax = -1;
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

        // O_TRUNC: truncate regular files to size 0 via filesystem op
        if ((flags & O_TRUNC) && node->type == VFS_FILE) {
            if (node->ops && (uint64_t)node->ops >= 0xffff800000000000ULL && node->ops->truncate &&
                (uint64_t)node->ops->truncate >= 0xffff800000000000ULL)
                node->ops->truncate(node, 0);
            else {
                // Fallback: reset size only (fs_data is FS-specific)
                node->size = 0;
            }
        }

        file_t *f = NULL;
        int rc = devfs_open_node(node, path, flags, &f);
        if (rc == -ENOSYS) {
            // Not a devfs device node → fall through to default FD_VFS
            f = file_alloc();
            if (!f) { vfs_node_put(node); regs->rax = -ENOMEM; break; }
            f->type = FD_VFS;
            f->node = node;  // takes ownership of lookup ref
            f->flags = flags;
        } else {
            vfs_node_put(node);  // devfs path owns its own ref
            if (rc < 0) { regs->rax = rc; break; }
            if (!f) { regs->rax = -ENOMEM; break; }
        }

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
        regs->rax = fd_dup(current->files, oldfd, 0);
        break;
    }
    case SYS_dup2: {
        // dup2(int oldfd, int newfd) → newfd / -errno
        int oldfd = (int)regs->rdi;
        int newfd = (int)regs->rsi;
        regs->rax = fd_dup2(current->files, oldfd, newfd);
        break;
    }
    case SYS_fork: {
        // fork() → child PID in parent, 0 in child
        int64_t pid = do_fork(regs, 0, 0, 0);
        // Parent path: regs->rax = child PID
        // Child path: do_fork already set child's pt_regs->rax = 0
        regs->rax = pid;
        log_info("fork: pid=%d returned %d\n", (int)current->pid, (int)pid);
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
        kfree(current->files->cwd);
        current->files->cwd = strdup(new_cwd);
        if (!current->files->cwd) { regs->rax = -ENOMEM; break; }

        log_info("chdir: pid=%d -> '%s'\n", (int)current->pid, current->files->cwd);
        regs->rax = 0;
        break;
    }
    case SYS_getcwd: {
        // getcwd(char *buf, size_t size) → buf / NULL(-errno)
        char *buf = (char *)regs->rdi;
        uint64_t size = regs->rsi;
        if (!current->files) {
            regs->rax = -ENOENT;
            break;
        }
        uint64_t len = strlen(current->files->cwd) + 1;
        // NULL buf with size=0 means "allocate" — we allocate on
        // the user heap via brk.  For simplicity, just require a
        // buffer of at least 256 bytes when size=0.
        if (buf == NULL) {
            regs->rax = -EINVAL;
            break;
        }
        if ((uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        if (len > size) { regs->rax = -ERANGE; break; }
        memcpy((void *)buf, current->files->cwd, len);
        regs->rax = (int64_t)(uint64_t)buf;
        break;
    }
    case SYS_stat: {
        // stat(const char *path, struct stat *buf) → 0 / -errno
        const char *path = (const char *)regs->rdi;
        struct stat *buf = (struct stat *)regs->rsi;

        if ((uint64_t)path >= current->addr_limit ||
            (uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        vfs_node_t *node = vfs_lookup_from(path_copy, cwd);
        kfree(path_copy);

        if (!node) { regs->rax = -ENOENT; break; }

        if (vfs_stat(node, buf) != 0) {
            vfs_node_put(node);
            regs->rax = -EIO;
            break;
        }
        vfs_node_put(node);
        regs->rax = 0;
        break;
    }
    case SYS_fstat: {
        // fstat(int fd, struct stat *buf) → 0 / -errno
        int fd = (int)regs->rdi;
        struct stat *buf = (struct stat *)regs->rsi;

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

        // PTY and pipe fds have no vfs_node — fill a synthetic stat
        if (!f->node) {
            memset(buf, 0, sizeof(struct stat));
            if (f->type == FD_PTY_MASTER || f->type == FD_PTY_SLAVE)
                buf->st_mode = S_IFCHR | 0600;
            else if (f->type == FD_PIPE)
                buf->st_mode = S_IFIFO | 0600;
            else { regs->rax = -ENOENT; break; }
            regs->rax = 0;
            break;
        }

        if (vfs_stat(f->node, buf) != 0) {
            regs->rax = -EIO;
            break;
        }
        regs->rax = 0;
        break;
    }
    case SYS_lseek: {
        // lseek(int fd, int64_t offset, int whence) → new offset / -errno
        int fd = (int)regs->rdi;
        int64_t offset = (int64_t)regs->rsi;
        int whence = (int)regs->rdx;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];
        if (!f->node) { regs->rax = -ESPIPE; break; }

        int64_t new_offset;
        switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (int64_t)f->offset + offset;
            break;
        case SEEK_END:
            new_offset = (int64_t)f->node->size + offset;
            break;
        default:
            new_offset = -1;
            break;
        }
        if (new_offset < 0) { regs->rax = -EINVAL; break; }
        f->offset = (uint64_t)new_offset;
        regs->rax = new_offset;
        break;
    }
    case SYS_fcntl: {
        // fcntl(int fd, int cmd, uint64_t arg) → result / -errno
        int fd = (int)regs->rdi;
        int cmd = (int)regs->rsi;
        uint64_t arg = regs->rdx;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];

        switch (cmd) {
        case F_DUPFD: {
            // dup to >= arg
            int start = (int)arg;
            if (start < 0) start = 0;
            regs->rax = fd_dup(current->files, fd, start);
            break;
        }
        case F_DUPFD_CLOEXEC: {
            // Same as F_DUPFD for now (close-on-exec not implemented)
            int start = (int)arg;
            if (start < 0) start = 0;
            regs->rax = fd_dup(current->files, fd, start);
            break;
        }
        case F_GETFD:
            // close-on-exec flag (always 0 for now)
            regs->rax = 0;
            break;
        case F_SETFD:
            // ignore close-on-exec for now
            regs->rax = 0;
            break;
        case F_GETFL:
            regs->rax = (int64_t)f->flags;
            break;
        case F_SETFL:
            // Only O_APPEND modifiable
            f->flags = (f->flags & ~O_APPEND) | (int)(arg & O_APPEND);
            regs->rax = 0;
            break;
        default:
            regs->rax = -EINVAL;
            break;
        }
        break;
    }
			case SYS_ioctl: {
			    // ioctl(int fd, unsigned long request, void *arg) -> 0 / -errno
			    int fd = (int)regs->rdi;
			    int request = (int)regs->rsi;
			    void *arg = (void *)regs->rdx;

			    if (fd < 0 || fd >= NOFILE || !current->files ||
			        !current->files->fd[fd]) {
			        regs->rax = -EBADF;
			        break;
			    }
			    file_t *f = current->files->fd[fd];

			    // User pointer validation
			    if ((uint64_t)arg >= current->addr_limit) {
			        regs->rax = -EFAULT;
			        break;
			    }

			    regs->rax = fd_ioctl(f, request, arg);
			    break;
			}
    case SYS_getdents64: {
        // getdents64(int fd, struct linux_dirent64 *buf, unsigned int count)
        // → bytes read / -errno
        int fd = (int)regs->rdi;
        struct linux_dirent64 *buf = (struct linux_dirent64 *)regs->rsi;
        unsigned int count = (unsigned int)regs->rdx;

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
        if (!f->node || f->node->type != VFS_DIR) {
            regs->rax = -ENOTDIR;
            break;
        }

        int ret = vfs_getdents(f->node, buf, count, &f->offset);
        if (ret < 0) {
            regs->rax = -EIO;
            break;
        }
        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_access: {
        // access(const char *path, int mode) → 0 / -errno
        const char *path = (const char *)regs->rdi;
        int mode = (int)regs->rsi;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        vfs_node_t *node = vfs_lookup_from(path_copy, cwd);
        kfree(path_copy);

        if (!node) { regs->rax = -ENOENT; break; }

        // Check access mode
        int ok = 1;
        if (mode & R_OK) {
            // For now, all files are readable
        }
        if (mode & W_OK) {
            // Check if filesystem is writable (not devfs)
            if (node->type == VFS_CHRDEV || node->type == VFS_BLKDEV) {
                // Devices: check if write op exists
                if (!node->ops || (uint64_t)node->ops < 0xffff800000000000ULL ||
                    !node->ops->write)
                    ok = 0;
            }
        }
        if (mode & X_OK) {
            // For now, no execute permission checks
        }

        vfs_node_put(node);

        if (!ok) { regs->rax = -EACCES; break; }
        regs->rax = 0;
        break;
    }
    case SYS_unlink: {
        // unlink(const char *path) → 0 / -errno
        const char *path = (const char *)regs->rdi;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_unlink(path_copy, cwd);
        kfree(path_copy);

        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_mkdir: {
        // mkdir(const char *path, int mode) → 0 / -errno
        // (mode is ignored for now — always 0755)
        const char *path = (const char *)regs->rdi;
        // int mode = (int)regs->rsi;  // ignored for now

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_mkdir(path_copy, cwd);
        kfree(path_copy);

        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_rmdir: {
        // rmdir(const char *path) → 0 / -errno
        const char *path = (const char *)regs->rdi;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_rmdir(path_copy, cwd);
        kfree(path_copy);

        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_rename: {
        // rename(const char *oldpath, const char *newpath) → 0 / -errno
        const char *oldpath = (const char *)regs->rdi;
        const char *newpath = (const char *)regs->rsi;

        if ((uint64_t)oldpath >= current->addr_limit ||
            (uint64_t)newpath >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *old_copy = strdup(oldpath);
        char *new_copy = strdup(newpath);
        if (!old_copy || !new_copy) {
            if (old_copy) kfree(old_copy);
            if (new_copy) kfree(new_copy);
            regs->rax = -ENOMEM;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_rename(old_copy, new_copy, cwd);
        kfree(old_copy);
        kfree(new_copy);

        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_truncate: {
        // truncate(const char *path, off_t length) → 0 / -errno
        const char *path = (const char *)regs->rdi;
        int64_t length = (int64_t)regs->rsi;

        if ((uint64_t)path >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        char *path_copy = strdup(path);
        if (!path_copy) { regs->rax = -ENOMEM; break; }

        const char *cwd = current->files ? current->files->cwd : "/";
        vfs_node_t *node = vfs_lookup_from(path_copy, cwd);
        kfree(path_copy);

        if (!node) { regs->rax = -ENOENT; break; }

        int ret = vfs_truncate(node, (uint64_t)length);
        vfs_node_put(node);
        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_ftruncate: {
        // ftruncate(int fd, off_t length) → 0 / -errno
        int fd = (int)regs->rdi;
        int64_t length = (int64_t)regs->rsi;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];
        if (!f->node) { regs->rax = -ENOENT; break; }

        int ret = vfs_truncate(f->node, (uint64_t)length);
        regs->rax = (int64_t)ret;
        break;
    }
    case SYS_time: {
        // time(time_t *tloc) → 0 (Jan 1 1970 for MVP)
        uint64_t *tloc = (uint64_t *)regs->rdi;
        if (tloc && (uint64_t)tloc < current->addr_limit) {
            *tloc = 0;
        }
        regs->rax = 0;
        break;
    }
    case SYS_gettimeofday: {
        // gettimeofday(struct timeval *tv, struct timezone *tz) → 0
        struct timeval *tv = (struct timeval *)regs->rdi;
        struct timezone *tz = (struct timezone *)regs->rsi;
        if (tv && (uint64_t)tv < current->addr_limit) {
            tv->tv_sec = 0;
            tv->tv_usec = 0;
        }
        if (tz && (uint64_t)tz < current->addr_limit) {
            tz->tz_minuteswest = 0;
            tz->tz_dsttime = 0;
        }
        regs->rax = 0;
        break;
    }
    case SYS_clock_gettime: {
        // clock_gettime(clockid_t clk_id, struct timespec *tp)
        // OS01 has no real RTC wall clock yet (gettimeofday returns 0),
        // so both CLOCK_REALTIME and CLOCK_MONOTONIC report the same
        // monotonic clocksource time (clocksource_read_ns, ns).
        uint64_t clk_id = regs->rdi;
        struct timespec *tp = (struct timespec *)regs->rsi;
        if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
            regs->rax = -EINVAL;
            break;
        }
        if (!tp || (uint64_t)tp >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        uint64_t ns = clocksource_read_ns();
        tp->tv_sec  = ns / 1000000000ULL;
        tp->tv_nsec = ns % 1000000000ULL;
        regs->rax = 0;
        break;
    }
    case SYS_getrandom: {
        // getrandom(void *buf, size_t len, unsigned int flags)
        uint64_t addr  = regs->rdi;
        uint64_t len   = regs->rsi;
        uint64_t flags = regs->rdx;

        if (len == 0) { regs->rax = 0; break; }              // buf may be NULL
        if (flags & ~(GRND_NONBLOCK | GRND_RANDOM)) {        // pool never blocks
            regs->rax = -EINVAL; break;
        }
        if (len > RANDOM_MAX_LEN) len = RANDOM_MAX_LEN;      // truncate, not error

        int rc = user_write_range_begin(addr, len);          // mm->lock + per-page PTE
        if (rc < 0) { regs->rax = rc; break; }               // -EFAULT (lock released)
        get_random_bytes((void *)addr, len);                 // chunked pool fill; mm->lock held
        user_write_range_end();
        regs->rax = len;                                     // actual bytes filled
        break;
    }
case SYS_setpgid: {
    int pid = (int)(int64_t)regs->rdi;
    int pgid = (int)(int64_t)regs->rsi;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    if (pid < 0 || pgid < 0 || pid == 1) {
        regs->rax = -EINVAL; break;
    }
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    task_t *target = NULL;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid && !(t->flags & PF_KTHREAD)) {
            target = t; break;
        }
    }
    if (!target) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -ESRCH; break;
    }
    if (current->pid != target->pid && current->session != target->session) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    // v4: pgid == pid OR pgid exists in caller's session
    int pgid_ok = (pgid == pid);
    if (!pgid_ok) {
        list_t *pos2 = init_task_union.task.list.next;
        while (pos2 != &init_task_union.task.list) {
            task_t *t2 = container_of(pos2, task_t, list);
            pos2 = task_list_next(pos2);
            if (t2->pgrp == pgid && t2->session == current->session) {
                pgid_ok = 1; break;
            }
        }
    }
    if (!pgid_ok) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    target->pgrp = pgid;
    // ── v3 自动 fg_pgrp 更新──────────────────
    // 任一成功 setpgid（含 join 现有 pgrp）且 fd 0 指向控制台 TTY 时
    // （file_t->tty == get_dev_tty()，由 §4.1.1 在 open 路径置位），
    // 把 dev_tty.fg_pgrp 同步到新 pgid——替代 POSIX 要求的"shell 调 tcsetpgrp"
    tty_t *dev_tty = get_dev_tty();
    if (dev_tty && current->files && current->files->fd[0]) {
        file_t *f0 = current->files->fd[0];
        if (f0->tty == dev_tty) {
            uint64_t ftf = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
            dev_tty->fg_pgrp = pgid;
            spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, ftf);
        }
    }
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = 0;
    break;
}
case SYS_getpgid: {
    int pid = (int)(int64_t)regs->rdi;
    if (pid == 0) pid = current->pid;
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    int ret = -ESRCH;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) { ret = t->pgrp; break; }
    }
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = ret;
    break;
}
case SYS_setsid: {
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    if (current->pgrp == current->pid) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EBUSY; break;
    }
    current->session = current->pid;
    current->pgrp = current->pid;
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = current->pid;
    break;
}
case SYS_getsid: {
    regs->rax = current->session;
    break;
}
    case SYS_nanosleep: {
        // nanosleep(const struct timespec *req, struct timespec *rem)
        const struct timespec *req = (const struct timespec *)regs->rdi;
        struct timespec *rem = (struct timespec *)regs->rsi;
        uint64_t ns = 0;
        if (req && (uint64_t)req < current->addr_limit) {
            ns = req->tv_sec * 1000000000ULL + req->tv_nsec;
        }

        uint64_t target_ns = clocksource_read_ns() + ns;
        current->wakeup_ns = target_ns;

        // Real sleep via the blocker framework: sched_unblock_blocked()
        // (run from every schedule(), i.e. every tick) wakes us once
        // clocksource reaches target_ns.  The loop absorbs spurious wakes.
        int r;
        do {
            r = blocker_wait(nanosleep_should_unblock, BLOCKER_NANOSLEEP, true);
        } while (r == 0 && clocksource_read_ns() < target_ns);
        current->wakeup_ns = 0;

        if (r == -EINTR) {
            // Interrupted by a signal before the deadline: report the
            // remaining time (guarded against unsigned underflow).
            uint64_t now_ns = clocksource_read_ns();
            uint64_t remain_ns = (now_ns < target_ns) ? (target_ns - now_ns) : 0;
            if (rem && (uint64_t)rem < current->addr_limit) {
                rem->tv_sec  = remain_ns / 1000000000ULL;
                rem->tv_nsec = remain_ns % 1000000000ULL;
            }
            regs->rax = -EINTR;
        } else {
            regs->rax = 0;
        }
        break;
    }
    case SYS_chmod: {
        // chmod(const char *path, mode_t mode) — stub: always success
        regs->rax = 0;
        break;
    }
    case SYS_fchmod: {
        // fchmod(int fd, mode_t mode) — stub: always success
        regs->rax = 0;
        break;
    }
    case SYS_times: {
        // times(struct tms *buf) — stub: return 0
        struct tms *buf = (struct tms *)regs->rdi;
        if (buf && (uint64_t)buf < current->addr_limit) {
            memset(buf, 0, sizeof(struct tms));
        }
        regs->rax = 0;
        break;
    }
    case SYS_uname: {
        // uname(struct utsname *buf) → 0 / -EFAULT
        struct utsname *buf = (struct utsname *)regs->rdi;
        if (!buf || (uint64_t)buf >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        memset(buf, 0, sizeof(struct utsname));
        strcpy(buf->sysname, "OS01");
        strcpy(buf->nodename, "os01");
        strcpy(buf->release, "0.1.0");
        strcpy(buf->version, "0.1.0");
        strcpy(buf->machine, "x86_64");
        regs->rax = 0;
        break;
    }
    case SYS_getppid: {
        // getppid() → parent PID (or 0 for init)
        if (current->parent)
            regs->rax = current->parent->pid;
        else
            regs->rax = 0;
        break;
    }
    case SYS_umask: {
        // umask(mode_t mode) — stub: always return 0
        regs->rax = 0;
        break;
    }
    case SYS_kill: {
        // kill(pid, sig) — POSIX process-group semantics:
        //   pid > 0   → signal single task (pid)
        //   pid == 0  → signal caller's process group
        //   pid == -1 → broadcast: all non-init, non-kthread, non-self
        //   pid < -1  → signal process group (-pid)
        int pid = (int)(int64_t)regs->rdi;
        int sig = (int)regs->rsi;

        if (sig < 1 || sig >= NSIG) {
            regs->rax = -EINVAL;
            break;
        }

        if (pid > 0) {
            regs->rax = task_send_signal(pid, sig);
        } else if (pid == 0) {
            regs->rax = signal_pgrp(current->pgrp, sig);
        } else if (pid == -1) {
            // POSIX pid==-1: signal to all tasks the caller may signal —
            // everyone except init (pid 1), kernel threads, and self.
            uint64_t f = spin_lock_irqsave(&task_list_lock);
            int matched = 0;
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t == current) continue;
                if (t->flags & PF_KTHREAD) continue;
                if (t->pid == 1) continue;
                t->signal |= (1ULL << sig);
                if (t->state == TASK_INTERRUPTIBLE)
                    task_wake(t);
                matched++;
            }
            spin_unlock_irqrestore(&task_list_lock, f);
            regs->rax = matched > 0 ? 0 : -ESRCH;
        } else { // pid < -1
            regs->rax = signal_pgrp(-pid, sig);
        }
        break;
    }
    case SYS_signal: {
        // sigaction(int signum, const struct sigaction *act,
        //           struct sigaction *oldact) → 0 / -errno
        int signum = (int)regs->rdi;
        const struct sigaction *act = (const struct sigaction *)regs->rsi;
        struct sigaction *oldact = (struct sigaction *)regs->rdx;

        if (signum < 1 || signum >= NSIG) {
            regs->rax = -EINVAL;
            break;
        }
        if (act && (uint64_t)act >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }
        if (oldact && (uint64_t)oldact >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        // Return old action if requested
        if (oldact) {
            // Copy from kernel sighand to user oldact
            oldact->sa_handler = current->sighand[signum].sa_handler;
            oldact->sa_flags   = current->sighand[signum].sa_flags;
            oldact->sa_restorer = current->sighand[signum].sa_restorer;
            oldact->sa_mask    = current->sighand[signum].sa_mask;
        }

        // Install new handler (SIGKILL and SIGSTOP cannot be caught or ignored)
        if (act && signum != SIGKILL && signum != SIGSTOP) {
            if ((uint64_t)act->sa_restorer >= current->addr_limit ||
                (uint64_t)act->sa_handler >= current->addr_limit) {
                regs->rax = -EINVAL;
                break;
            }
            current->sighand[signum].sa_handler  = act->sa_handler;
            current->sighand[signum].sa_flags    = act->sa_flags;
            current->sighand[signum].sa_restorer = act->sa_restorer;
            current->sighand[signum].sa_mask     = act->sa_mask;
        }
        regs->rax = 0;
        break;
    }
    case SYS_sigprocmask: {
        // sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
        int how = (int)regs->rdi;
        const sigset_t *set = (const sigset_t *)regs->rsi;
        sigset_t *oldset = (sigset_t *)regs->rdx;

        // Return current mask if requested
        if (oldset) {
            if ((uint64_t)oldset >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            *oldset = (sigset_t)current->blocked;
        }

        // Update mask if set is provided
        if (set) {
            if ((uint64_t)set >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            switch (how) {
            case SIG_BLOCK:
                current->blocked |= *set;
                break;
            case SIG_UNBLOCK:
                current->blocked &= ~*set;
                break;
            case SIG_SETMASK:
                current->blocked = (int64_t)*set;
                break;
            default:
                regs->rax = -EINVAL;
                break;
            }
        }
        regs->rax = 0;
        break;
    }
    case SYS_sigreturn: {
        // regs->rsp == sigframe start in user space (handler ret pop'd
        // trampoline, then int $0x80 saved this RSP as pt_regs->rsp).
        uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
        uint64_t frame_phys = user_va_to_phys(user_pml4, regs->rsp);
        if (!frame_phys) { regs->rax = -EFAULT; break; }
        struct sigframe *kframe = (struct sigframe *)Phy_To_Virt(frame_phys);

        // Validate sigframe: iretq CS must be ring-3
        if ((kframe->cs & 3) != 3) { regs->rax = -EINVAL; break; }

        // Restore blocked mask
        current->blocked = kframe->blocked;

        // Restore all GPRs
        regs->r15=kframe->r15; regs->r14=kframe->r14; regs->r13=kframe->r13;
        regs->r12=kframe->r12; regs->r11=kframe->r11; regs->r10=kframe->r10;
        regs->r9=kframe->r9;   regs->r8=kframe->r8;
        regs->rbx=kframe->rbx; regs->rcx=kframe->rcx; regs->rdx=kframe->rdx;
        regs->rsi=kframe->rsi; regs->rdi=kframe->rdi; regs->rbp=kframe->rbp;
        regs->ds=kframe->ds;   regs->es=kframe->es;    regs->rax=kframe->rax;

        // Restore iretq frame → RESTORE_ALL → iretq to original context
        regs->rip=kframe->rip; regs->cs=kframe->cs; regs->rflags=kframe->rflags;
        regs->rsp=kframe->rsp; regs->ss=kframe->ss;

        // Note: do_system_call's tail arch_do_signal_delivery() runs after
        // this break.  If there are additional pending signals, they
        // will be delivered on the freshly-restored stack — matching
        // Linux behavior (sigreturn processes remaining signals before
        // the final iretq to userspace).
        break;
    }
    case SYS_sync: {
        // sync() — flush filesystem caches to disk
        // For OS01 (FAT32 without write-back cache), this is a no-op.
        // Future: flush AHCI/FAT buffers here.
        regs->rax = 0;
        break;
    }
    case SYS_reboot: {
        int cmd = (int)(int64_t)regs->rdi;

        log_info("syscall: reboot(cmd=%d) from pid=%d\n",
                 cmd, (int)current->pid);

        // ── ACPI power-off ─────────────────────────────────
        if (cmd == RB_POWER_OFF && apic_info.pm1a_port) {
            // SLP_EN (bit 13) toggles sleep.  SLP_TYPa=0 for
            // S5 on QEMU q35 (the \_S5 object reports {0, 0}).
            log_info("ACPI: powering off via PM1a=%#x\n",
                     (unsigned)apic_info.pm1a_port);
            outw(apic_info.pm1a_port, 0x2000);
            // Block — platform powers off asynchronously.
            while (1) __asm__ __volatile__("hlt");
        }

        // ── ACPI reboot / halt fallback ────────────────────
        // Keyboard controller pulse-reset ($0xFE → port $0x64)
        while ((inb(0x64) & 0x02) != 0) { /* wait */ }
        outb(0xFE, 0x64);
        while (1) __asm__ __volatile__("hlt");
    }
    case SYS_mmap: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        uint64_t prot   = regs->rdx;
        uint64_t flags  = regs->r10;
        uint64_t fd     = regs->r8;
        uint64_t offset = regs->r9;
        regs->rax = do_mmap(addr, length, prot, flags, fd, offset);
        break;
    }
    case SYS_mprotect: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        uint64_t prot   = regs->rdx;
        regs->rax = do_mprotect(addr, length, prot);
        break;
    }
    case SYS_munmap: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        regs->rax = do_munmap(addr, length);
        break;
    }
    case SYS_futex: {
        int *uaddr = (int *)regs->rdi;
        int op = (int)regs->rsi;
        int val = (int)regs->rdx;

        if ((uint64_t)uaddr >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        switch (op) {
        case FUTEX_WAIT:
            regs->rax = do_futex_wait(uaddr, val);
            break;
        case FUTEX_WAKE:
            regs->rax = do_futex_wake(uaddr, val);
            break;
        default:
            regs->rax = -EINVAL;
        }
        break;
    }
    case SYS_poll: {
        int64_t nfds64 = (int64_t)regs->rsi;
        regs->rax = do_poll((struct pollfd *)regs->rdi,
                            (uint64_t)nfds64,
                            (int)regs->rdx);
        break;
    }
    case SYS_ppoll: {
        regs->rax = -ENOSYS;
        break;
    }
    case SYS_select: {
        regs->rax = do_select((int)regs->rdi,
                              (void *)regs->rsi, (void *)regs->rdx,
                              (void *)regs->r10, (void *)regs->r8);
        break;
    }
    case SYS_pselect6: {
        regs->rax = do_pselect6((int)regs->rdi,
                                (void *)regs->rsi, (void *)regs->rdx,
                                (void *)regs->r10, (void *)regs->r8,
                                (const void *)regs->r9);
        break;
    }
    case SYS_socket: {
        regs->rax = do_socket((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
        break;
    }
    case SYS_connect: {
        struct sockaddr_in addr;
        if (regs->rdx < sizeof(addr) ||
            !syscall_user_range_ok(regs->rsi, sizeof(addr))) {
            regs->rax = -EFAULT; break;
        }
        memcpy(&addr, (void *)regs->rsi, sizeof(addr));
        // sin_port is network byte order; lwIP netconn_connect wants host order.
        regs->rax = do_connect((int)regs->rdi, addr.sin_addr, os01_ntohs(addr.sin_port));
        break;
    }
    case SYS_sendto: {
        uint64_t len = regs->rdx;
        uint32_t ip = 0; uint16_t port = 0;
        uint64_t addr_ptr = regs->r8;
        if (len && !syscall_user_range_ok(regs->rsi, len)) {
            regs->rax = -EFAULT; break;
        }
        if (addr_ptr) {
            if (regs->r9 < sizeof(struct sockaddr_in) ||
                !syscall_user_range_ok(addr_ptr, sizeof(struct sockaddr_in))) {
                regs->rax = -EFAULT; break;
            }
            struct sockaddr_in a;
            memcpy(&a, (void *)addr_ptr, sizeof(a));
            ip = a.sin_addr; port = a.sin_port;
        }
        regs->rax = do_sendto((int)regs->rdi, (void *)regs->rsi,
                              len, (int)regs->r10, ip, os01_ntohs(port));
        break;
    }
    case SYS_recvfrom: {
        uint64_t addr_ptr = regs->r8;
        uint64_t addrlen_ptr = regs->r9;
        if (regs->rdx && !syscall_user_range_ok(regs->rsi, regs->rdx)) {
            regs->rax = -EFAULT; break;
        }
        uint32_t ip = 0;
        uint16_t port = 0;
        uint32_t addrlen = 0;
        if (addr_ptr) {
            if (!syscall_user_range_ok(addrlen_ptr, sizeof(addrlen))) {
                regs->rax = -EFAULT; break;
            }
            memcpy(&addrlen, (void *)addrlen_ptr, sizeof(addrlen));
            if (addrlen < sizeof(struct sockaddr_in) ||
                !syscall_user_range_ok(addr_ptr, sizeof(struct sockaddr_in))) {
                regs->rax = -EINVAL; break;
            }
        }
        int64_t ret = do_recvfrom((int)regs->rdi, (void *)regs->rsi,
                                  regs->rdx, (int)regs->r10,
                                  addr_ptr ? &ip : NULL,
                                  addr_ptr ? &port : NULL);
        if (ret >= 0 && addr_ptr) {
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            src.sin_family = AF_INET;
            src.sin_port = os01_htons(port);
            src.sin_addr = ip;
            memcpy((void *)addr_ptr, &src, sizeof(src));
            addrlen = sizeof(src);
            memcpy((void *)addrlen_ptr, &addrlen, sizeof(addrlen));
        }
        regs->rax = ret;
        break;
    }
    case SYS_bind: {
        struct sockaddr_in a;
        if (regs->rdx < sizeof(a) ||
            !syscall_user_range_ok(regs->rsi, sizeof(a))) {
            regs->rax = -EFAULT; break;
        }
        memcpy(&a, (void *)regs->rsi, sizeof(a));
        regs->rax = do_bind((int)regs->rdi, a.sin_addr, os01_ntohs(a.sin_port));
        break;
    }
    case SYS_listen: {
        regs->rax = do_listen((int)regs->rdi, (int)regs->rsi);
        break;
    }
    case SYS_accept: {
        regs->rax = do_accept((int)regs->rdi, NULL, NULL);
        break;
    }
    case SYS_setsockopt: {
        if (regs->r8 && !syscall_user_range_ok(regs->r10, regs->r8)) {
            regs->rax = -EFAULT; break;
        }
        regs->rax = do_setsockopt((int)regs->rdi, (int)regs->rsi,
                                  (int)regs->rdx, (void *)regs->r10, regs->r8);
        break;
    }
    case SYS_getsockname: {
        if (!syscall_user_range_ok(regs->rdx, sizeof(uint32_t)) ||
            !syscall_user_range_ok(regs->rsi, sizeof(struct sockaddr_in))) {
            regs->rax = -EFAULT; break;
        }
        regs->rax = do_getsockname((int)regs->rdi,
                                   (void *)regs->rsi, (uint32_t *)regs->rdx);
        break;
    }
    case SYS_getifaddr: {
        regs->rax = do_getifaddr();
        break;
    }
    case SYS_getsockopt: {
        if (!syscall_user_range_ok(regs->r8, sizeof(uint32_t))) {
            regs->rax = -EFAULT; break;
        }
        regs->rax = do_getsockopt((int)regs->rdi, (int)regs->rsi,
                                  (int)regs->rdx, (void *)regs->r10, (uint32_t *)regs->r8);
        break;
    }
    case SYS_shutdown: {
        regs->rax = do_shutdown((int)regs->rdi, (int)regs->rsi);
        break;
    }
    default:
        log_err("syscall: unknown nr=%d from pid=%d\n",
                (int)regs->rax, (int)current->pid);
        regs->rax = -EINVAL;
        break;
    }

    // ── Signal delivery ──────────────────────────────────────
    // Runs after every syscall that returns to user mode.
    // For signals that kill (SIG_DFL + fatal), do_exit() calls
    // switch_to() and never returns.
    if (regs->cs & 3)
        arch_do_signal_delivery(regs);
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
	set_trap_gate(14,0,page_fault);   // IST 0: run on task's kernel stack.
	                                     // COW handler can trigger a reschedule via
	                                     // ret_from_intr, and schedule()'s get_current_task()
	                                     // (RSP & ~0x7FFF) breaks on IST stacks.
	//15 Intel reserved. Do not use.
	set_trap_gate(16,1,x87_FPU_error);
	set_trap_gate(17,1,alignment_check);
	set_trap_gate(18,1,machine_check);
	set_trap_gate(19,1,SIMD_exception);
	set_trap_gate(20,1,virtualization_exception);

	// int 0x80 syscall gate — DPL=3 so user code can call it
	set_system_gate(0x80, 0, system_call);
}

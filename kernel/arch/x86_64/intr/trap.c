#include <arch/x86_64/trap.h>
#include <arch/irq.h>
#include <arch/x86_64/gate.h>
#include <arch/x86_64/hw.h>
#include <arch/segment.h>
#include <arch/x86_64/tss.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <core/printk.h>
#include <log/log.h>
#include <core/trace.h>
#include <arch/x86_64/asm.h>
#include <sched/task.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <percpu/percpu.h>
#include <intr/apic.h>
#include <memory/slab.h>
#include <driver/serial.h>
#include <errno.h>
#include <uapi/syscall.h>
#include <syscall/dispatch.h>
#include <syscall/compat.h>
#include <string.h>
typedef int pid_t;
#include <core/debug.h>
#include <memory/uaccess.h>
#include <kernel.h>
#include <memory/vma.h>
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
		// uaccess _ft recovery: user-range fault while a copy buffer is armed.
		// current == task_from_ist0 (verified by Task 0; eq=1 on kernel-mode PF
		// because the #PF frames on IST 0 = the task's kernel stack).  Longjmp
		// value is the Clang-required compile-time constant 1.
		if (cr2 < current->addr_limit && current->fault_jmp)
			__builtin_longjmp(current->fault_jmp, 1);
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
			// Forked-image COW write: the ELF loader installs
			// page-table entries for the image but NO VMA for
			// them (no demand paging for the image), so
			// vma_find() legitimately returns NULL here.  When
			// such a process is forked, the parent's writable
			// image PTEs become R/O + PAGE_COW — a write fault
			// (P=1, W=1) on that PTE must privatize the page,
			// exactly like the VMA-covered COW path below, not
			// SIGSEGV.  Anything else (no COW PTE, or a
			// non-write fault) is a genuine segfault.
			if ((error_code & 0x03) == 0x03) {
				uint64_t *upte = vmm_pt_walk(
				    (uint64_t *)Phy_To_Virt(
				        (uint64_t)t->mm->pgdir),
				    cr2, 0, 0);
				if (upte && (*upte & PAGE_VALID) &&
				    (*upte & PAGE_COW)) {
					uint64_t old_phys =
					    *upte & PAGE_4K_MASK;
					uint64_t new_flags =
					    (*upte & ~PAGE_4K_MASK) |
					    PAGE_WRITE;
					new_flags &= ~(uint64_t)PAGE_COW;
					if (page_cow_refs(old_phys) > 1) {
						uint64_t nphys =
						    alloc_4k_page();
						if (!nphys) {
							kill_current_user_task(
							    regs);
							return;
						}
						memcpy(
						    (void *)Phy_To_Virt(nphys),
						    (void *)Phy_To_Virt(old_phys),
						    PAGE_4K_SIZE);
						*upte = nphys | new_flags;
						page_cow_put(old_phys);
					} else {
						(void)page_cow_put(old_phys);
						*upte = old_phys | new_flags;
					}
					flush_tlb();
					return;
				}
			}
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
		// For the heap VMA, take mm->lock IRQ-safely, re-check
		// the fault address is within the COMMITTED heap range
		// (start_brk ≤ ALIGN_UP(end_brk, 4 KiB)), then privatize
		// under the lock and drop it before returning.  For
		// other VMAs the lock is not held (do_mprotect et al.
		// only block concurrent PTEs during a full PTE walk).
		if ((error_code & 0x03) == 0x03) {
			uint64_t *user_pgd =
			    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pgdir);
			uint64_t *pte = vmm_pt_walk(user_pgd, cr2, 0, 0);
			if (pte && (*pte & PAGE_COW)) {
				int heap_vma = !!(vma->vm_flags & VM_HEAP);
				uint64_t lock_flags = 0;
				if (heap_vma) {
					lock_flags = spin_lock_irqsave(&t->mm->lock);
					uint64_t committed_top =
					    (t->mm->end_brk +
					     (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;
					if (cr2 >= t->mm->start_brk &&
					    cr2 < committed_top) {
						/* still in committed
						 * heap — fall through
						 * to the COW resolve */
					} else {
						spin_unlock_irqrestore(
						    &t->mm->lock, lock_flags);
						kill_current_user_task(regs);
						return;
					}
				}

				uint64_t old_phys = *pte & PAGE_4K_MASK;
				uint64_t new_phys = 0;
				int resolved = 0;
				if (page_cow_refs(old_phys) > 1) {
					new_phys = alloc_4k_page();
					if (!new_phys) {
						if (heap_vma)
							spin_unlock_irqrestore(
							    &t->mm->lock,
							    lock_flags);
						kill_current_user_task(regs);
						return;
					}
					memcpy((void *)Phy_To_Virt(new_phys),
					       (void *)Phy_To_Virt(old_phys),
					       PAGE_4K_SIZE);
					*pte = new_phys | vma->vm_page_prot;
					page_cow_put(old_phys);
					resolved = 1;
				} else {
					(void)page_cow_put(old_phys);
					*pte = old_phys | vma->vm_page_prot;
					resolved = 1;
				}

				if (heap_vma)
					spin_unlock_irqrestore(
					    &t->mm->lock, lock_flags);
				if (resolved)
					flush_tlb();
				return;
			}
		}

		// -- Page not present (P=0) - demand allocation --
		//
		// The HEAP VMA is special: every 4 KiB leaf in the
		// committed range is pre-mapped by mm_set_brk, and the
		// user process MUST NOT be able to lazily add a new heap
		// leaf via a #PF (kernel-side demand paging is gone for
		// the heap).  An absent-leaf fault in the heap range is
		// always -EFAULT — the user will receive SIGSEGV via
		// kill_current_user_task.  This closes the latent hole
		// where a stale VM_HEAP VMA or a leftover 2 MiB huge
		// leaf could re-expose unmapped heap as writable.
		if (!(error_code & 0x01)) {
			/* Heap VMA absent-leaf guard: mm_set_brk pre-maps
			 * every committed leaf before publishing end_brk,
			 * so a P=0 fault inside the heap VMA can only be a
			 * stale/foreign access — never demand-map it (the
			 * kernel has no kernel-side demand paging for the
			 * heap; see isolation-design.md §5.2). */
			if (vma->vm_flags & VM_HEAP) {
				log_debug("PF: pid=%d cr2=%p heap absent-leaf -> EFAULT\n",
				          t->pid, cr2);
				kill_current_user_task(regs);
				return;
			}

			uint64_t *user_pgd =
			    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pgdir);

			if (vma->vm_flags & VM_ANON) {
				uint64_t phys = alloc_4k_page();
				if (!phys) {
					log_debug("PF: pid=%d OOM\n", t->pid);
					kill_current_user_task(regs);
					return;
				}
				memset((void *)Phy_To_Virt(phys), 0, PAGE_4K_SIZE);
				int rc = vmm_map_4k_page(user_pgd, phys,
							     cr2 & PAGE_4K_MASK, vma->vm_page_prot);
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
				int rc = vmm_map_4k_page(user_pgd, phys,
							     cr2 & PAGE_4K_MASK, vma->vm_page_prot);
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
// User VA range (code + data + heap envelope): [USER_CODE_ADDR, USER_CODE_ADDR + USER_ENVELOPE_SIZE).
// Envelope expanded to 512 MiB.
// The user stack page is relocated above this region in kernel/include/sched/task.h (USER_STACK_BASE).
#define USER_ENVELOPE_SIZE 0x20000000UL  // 512 MiB
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE  // Compatibility alias

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

        // 3. Fast-reject + authoritative write via user VIRTUAL addresses.
        //    copy_to_user_ft walks pgd per page, so a write that crosses
        //    a 4KB page boundary lands in the correct second page (the
        //    old Phy_To_Virt+memcpy translated only the start address and
        //    would corrupt the wrong physical page for 4KB mappings).
        //    If either copy fails the signal stays pending and regs are
        //    untouched — the next return-to-userspace retries with a
        //    possibly fixed user stack.
        if (!syscall_check_user_range(new_rsp, total, true))
            continue;   // keep pending, retry
        uint64_t tramp = restorer;
        if (copy_to_user_ft((void *)new_rsp, &tramp, 8) < 0)
            continue;   // keep pending — do NOT touch regs
        if (copy_to_user_ft((void *)(new_rsp + 8), &frame,
                            sizeof(frame)) < 0)
            continue;   // keep pending — do NOT touch regs

        // 4. Rewrite pt_regs → RESTORE_ALL → iretq → handler
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

    syscall_ctx_t syscall_ctx = {
        .nr = regs->rax,
        .args = { regs->rdi, regs->rsi, regs->rdx,
                  regs->r10, regs->r8, regs->r9 },
        .arch_frame = regs,
        .suppress_writeback = false,
    };

    int64_t result;
    if (current->flags & PF_LINUX_ABI) {
        result = compat_linux_dispatch(&syscall_ctx);
    } else {
        const char *sname = syscall_name(syscall_ctx.nr);
        debug_syscall("[strace] pid=%d syscall(%s, arg1=%#lx, arg2=%#lx, arg3=%#lx)\n",
                      (int)current->pid, sname ? sname : "?",
                      (unsigned long)regs->rdi,
                      (unsigned long)regs->rsi,
                      (unsigned long)regs->rdx);
        result = syscall_dispatch(&syscall_ctx);
    }
    if (!syscall_ctx.suppress_writeback)
        regs->rax = (uint64_t)result;

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

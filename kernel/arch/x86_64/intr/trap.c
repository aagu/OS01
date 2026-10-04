#include <arch/x86_64/trap.h>
#include <arch/irq.h>
#include <arch/x86_64/gate.h>
#include <arch/x86_64/hw.h>
#include <arch/segment.h>
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
#include <sys/stat.h>
#include <string.h>
typedef int pid_t;
#include <termios.h>
#include <tty/tty.h>
#include <stdlib.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <core/debug.h>
#include <memory/uaccess.h>   // strnlen_user, copy_from_user_ft, VFS_NAME_MAX
#include <fs/file.h>
#include <fs/poll.h>     // struct pollfd, do_poll()
#include <fs/select.h>   // sigset_t, do_select(), do_pselect6()
#include <time/timer.h>
#include <arch/x86_64/clocksource.h>  // clocksource_read_ns()
#include <uapi/time.h>
#include <kernel.h>
#include <memory/vma.h>
#include <sys/random.h>   // GRND_NONBLOCK, GRND_RANDOM (for SYS_getrandom)
#include <random/random.h>  // get_random_bytes(), RANDOM_MAX_LEN
#include <uapi/futex.h>
#include <sync/futex.h>
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
uint64_t user_va_to_phys(uint64_t *pgd, uint64_t va)
{
    size_t l4 = (va >> PAGE_PGD_SHIFT) & 0x1ff;
    size_t l3 = (va >> PAGE_1G_SHIFT) & 0x1ff;
    size_t l2 = (va >> PAGE_2M_SHIFT) & 0x1ff;
    if (!(pgd[l4] & PAGE_VALID)) return 0;
    uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);
    if (!(pud[l3] & PAGE_VALID)) return 0;
    uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l3] & PAGE_4K_MASK);
    if (!(pmd[l2] & PAGE_VALID)) return 0;
    return (pmd[l2] & PAGE_2M_MASK & ~PAGE_NO_EXEC) | (va & 0x1FFFFF);
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

// SOCKOPT_MAX: ABI cap on the optlen argument of setsockopt/getsockopt
// (regs->r8 in the trap handlers).  A hostile r8 (e.g. 0xFFFFFFFF)
// would otherwise allow a kmalloc(4GB) DoS.  4 KiB matches typical
// socket option payloads (IP_PKTINFO, SO_LINGER, TCP_* etc.).
#define SOCKOPT_MAX 4096

// ── exec argv/envp bounded deep-copy (Task 5, Cat A') ───────
// Allocates a kernel-heap copy of a user-space NULL-terminated array
// of string pointers, including a kernel copy of every string.
//
// Bounds:
//   * at most MAX_ARGV pointers in the array
//   * each string is at most MAX_ARG_STRLEN bytes (incl. NUL)
//   * total bytes across all strings <= MAX_ARG_TOTAL
//
// On any fault or bound violation:
//   * array element pointer < USER_MIN_ADDR or >= addr_limit → -EFAULT
//   * strnlen_user fault → -EFAULT
//   * strnlen_user returns MAX_ARG_STRLEN (no NUL within cap) → -E2BIG
//   * element count > MAX_ARGV → -E2BIG
//   * total bytes > MAX_ARG_TOTAL → -E2BIG
//   * kmalloc failure → -ENOMEM
//
// On failure all already-allocated strings + the partial array are freed.
// On success *out_arr is a NULL-terminated kmalloc'd array of kmalloc'd
// strings; caller frees with free_deep_argv().
static void free_deep_argv(char **kargv, char **kenvp)
{
    if (kargv) {
        for (size_t i = 0; kargv[i] != NULL; i++) kfree(kargv[i]);
        kfree(kargv);
    }
    if (kenvp) {
        for (size_t i = 0; kenvp[i] != NULL; i++) kfree(kenvp[i]);
        kfree(kenvp);
    }
}

// Internal helper: free all strings + the array (allocated so far).
// Used on the failure paths before the array is fully populated.
static void free_partial_argv(char **arr, size_t filled)
{
    if (!arr) return;
    for (size_t i = 0; i < filled; i++) kfree(arr[i]);
    kfree(arr);
}

// Test-only export: deep_copy_argv is normally static.  Under
// OS01_SELFTEST the storage class is dropped so the regression test in
// kernel/selftest/test_deep_copy_argv.c can call it directly without
// going through do_system_call.  No other callers exist outside this
// translation unit.
#ifdef OS01_SELFTEST
int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#else
static int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#endif
{
    *out_arr = NULL;
    if (user_arr == NULL) return 0;

    // Phase 1: scan the array (fault-tolerant per pointer) to count
    // entries and validate every element pointer.  Bound the loop by
    // MAX_ARGV so a hostile unbounded array cannot loop forever.
    const char *ptrs[MAX_ARGV + 1];
    size_t count = 0;
    bool null_found = false;
    uint64_t addr_limit = current->addr_limit;

    for (size_t i = 0; i <= MAX_ARGV; i++) {
        uint64_t p = 0;
        if (copy_from_user_ft(&p, &user_arr[i], sizeof(p)) < 0)
            return -EFAULT;
        if (p == 0) {                       // NULL terminator
            count = i;
            null_found = true;
            break;
        }
        // Bad element pointer: kernel address or below USER_MIN_ADDR.
        if (p < USER_MIN_ADDR || p >= addr_limit)
            return -EFAULT;
        ptrs[i] = (const char *)p;
    }
    // If the loop ran to MAX_ARGV+1 without seeing NULL, either the
    // array has more than MAX_ARGV entries (over cap) or it's not
    // NUL-terminated within MAX_ARGV+1 (treated the same).  Reject
    // with -E2BIG per the deep_copy_argv contract (line above).
    //
    // Distinguish this from the legitimate empty case (argv={NULL},
    // NULL found at i=0, null_found=true) which Phase 2/3 handles
    // naturally: zero strnlen iterations, kmalloc(8) for the array,
    // zero copies, arr[0]=NULL terminator.  setup_user_stack (task.c)
    // accepts both argv=NULL and argv={NULL}.
    if (!null_found) return -E2BIG;

    // Phase 2: for each element, strnlen + bounded total accumulator.
    size_t total = 0;
    size_t lens[MAX_ARGV];
    for (size_t i = 0; i < count; i++) {
        int n = strnlen_user(ptrs[i], MAX_ARG_STRLEN);
        if (n < 0) return -EFAULT;
        if (n >= MAX_ARG_STRLEN) return -E2BIG;     // no NUL within cap
        lens[i] = (size_t)n + 1;                    // incl. NUL
        total += lens[i];
        if (total > MAX_ARG_TOTAL) return -E2BIG;
    }

    // Phase 3: allocate the kernel array (NULL-terminated) and copy
    // every string.  Any failure mid-way frees everything we already
    // allocated.
    char **arr = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!arr) return -ENOMEM;
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        char *kstr = (char *)kmalloc(lens[i]);
        if (!kstr) {
            free_partial_argv(arr, filled);
            return -ENOMEM;
        }
        if (copy_from_user_ft(kstr, ptrs[i], lens[i]) < 0) {
            kfree(kstr);
            free_partial_argv(arr, filled);
            return -EFAULT;
        }
        arr[filled++] = kstr;
    }
    arr[count] = NULL;
    *out_arr = arr;
    return 0;
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
            [6]   = 73, // lstat      → SYS_lstat     (was -1 unsupported)
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
            [88]  = 71, // symlink    → SYS_symlink   (was missing)
            [89]  = 72, // readlink   → SYS_readlink  (was 26 = SYS_rename; pre-existing bug)
            [102] = 36,// getppid -> SYS_getppid (Linux: 110? no, 102)
            [110] = 36,// getppid -> SYS_getppid
            [162] = 31,// nanosleep -> SYS_nanosleep
            [201] = 34,// times -> SYS_times
            [217] = 21,// getdents64 -> SYS_getdents64
            [231] = 2, // exit_group -> SYS_exit
            [262] = 74,// newfstatat → SYS_fstatat   (was missing)

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
    syscall_ctx_t syscall_ctx = {
        .nr = regs->rax,
        .args = { regs->rdi, regs->rsi, regs->rdx,
                  regs->r10, regs->r8, regs->r9 },
        .arch_frame = regs,
    };
    if (syscall_has_handler(syscall_ctx.nr)) {
        regs->rax = (uint64_t)syscall_dispatch(&syscall_ctx);
    } else {
    switch (regs->rax) {
    // ── Syscall name table (for strace) ─────────────────────
    static const char *syscall_names[75] = {
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
        [71] = "symlink",
        [72] = "readlink",
        [73] = "lstat",
        [74] = "fstatat",
    };
    const char *sname = (regs->rax < 75 && syscall_names[regs->rax])
                        ? syscall_names[regs->rax] : "?";
    (void)sname;
    debug_syscall("[strace] pid=%d syscall(%s, arg1=%#lx, arg2=%#lx, arg3=%#lx)\n",
                  (int)current->pid, sname,
                  (unsigned long)regs->rdi,
                  (unsigned long)regs->rsi,
                  (unsigned long)regs->rdx);
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
    case SYS_getpid: {
        regs->rax = current->pid;
        break;
    }
// ── exec argv/envp bounded deep-copy (Task 5, Cat A') ───────
// (Definitions live at file scope, just above do_system_call.)

    case SYS_exec: {
        // exec(const char *path, char *const argv[], char *const envp[])
        // If argv == NULL: old behavior (no args)
        // If argv != NULL: deep-copy argv/envp arrays+strings to kernel
        // heap (Task 5) before calling sys_exec.  sys_exec then operates
        // ONLY on the kernel copies — no user-memory dereference.
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

        // Copy path to kernel heap (bounded + fault-tolerant) so that
        // user-mapped pages cannot fault us mid-VFS-walk.
        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { regs->rax = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { regs->rax = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { regs->rax = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            regs->rax = -EFAULT;
            break;
        }

        // Deep-copy argv/envp arrays+strings to kernel heap.  Done
        // BEFORE sys_exec builds the new pgd / frees the old space
        // (no UAF on the old address space).  sys_exec never touches
        // user memory after this point.
        char **kargv = NULL;
        char **kenvp = NULL;
        if (argv != NULL) {
            int64_t r = deep_copy_argv(argv, &kargv);
            if (r < 0) {
                kfree(path_copy);
                regs->rax = r;
                break;
            }
        }
        if (envp != NULL) {
            int64_t r = deep_copy_argv(envp, &kenvp);
            if (r < 0) {
                free_deep_argv(kargv, NULL);
                kfree(path_copy);
                regs->rax = r;
                break;
            }
        }

        // sys_exec builds argv/envp on the user stack using a fixed
        // str_offset[128] table (task.c) — the COMBINED argc+envc must
        // fit (each array alone is bounded to MAX_ARGV=128 by
        // deep_copy_argv, but both together could reach 256).  Enforce
        // the combined cap here so sys_exec's fixed table cannot be
        // overrun (kernel-stack corruption).
        if ((argv != NULL) && (envp != NULL)) {
            size_t ac = 0, ec = 0;
            while (kargv[ac]) ac++;
            while (kenvp[ec]) ec++;
            if (ac + ec > 128) {
                free_deep_argv(kargv, kenvp);
                kfree(path_copy);
                regs->rax = -E2BIG;
                break;
            }
        }

        int64_t ret = sys_exec(path_copy, regs,
                               (const char *const *)kargv,
                               (const char *const *)kenvp);
        free_deep_argv(kargv, kenvp);
        kfree(path_copy);
        regs->rax = ret;
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

        // Validate status pointer (NULL legal → skip).
        // check_user_range is a fast reject; the actual _ft copy in
        // do_waitpid is the authority (handles racing munmap).
        if (status &&
            !syscall_check_user_range((uint64_t)status, sizeof(int), true)) {
            regs->rax = -EFAULT;
            break;
        }

        regs->rax = do_waitpid(pid, status, options);
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
        // sigaction(signum, const struct sigaction *act,
        //           struct sigaction *oldact) → 0 / -errno
        // Cat B: read user act into kernel copy via _ft, validate, install;
        // write kernel → user oldact via _ft.
        int signum = (int)regs->rdi;
        const struct sigaction *act = (const struct sigaction *)regs->rsi;
        struct sigaction *oldact = (struct sigaction *)regs->rdx;

        if (signum < 1 || signum >= NSIG) {
            regs->rax = -EINVAL;
            break;
        }

        // Read user act into kernel copy (NULL legal → skip).
        struct sigaction kact;
        if (act) {
            if (!syscall_check_user_range((uint64_t)act, sizeof(kact), false)) {
                regs->rax = -EFAULT;
                break;
            }
            if (copy_from_user_ft(&kact, act, sizeof(kact)) < 0) {
                regs->rax = -EFAULT;
                break;
            }
        }

        // Return old action if requested.  Build kernel copy, then _ft
        // write to user.  No bare field writes into user.
        if (oldact) {
            if (!syscall_check_user_range((uint64_t)oldact, sizeof(kact), true)) {
                regs->rax = -EFAULT;
                break;
            }
            struct sigaction kold = {
                .sa_handler  = current->sighand[signum].sa_handler,
                .sa_flags    = current->sighand[signum].sa_flags,
                .sa_restorer = current->sighand[signum].sa_restorer,
                .sa_mask     = current->sighand[signum].sa_mask,
            };
            {
                ssize_t user_copy_rc = copy_to_user_ft(oldact, &kold, sizeof(kold));
                if (user_copy_rc < 0) {
                    regs->rax = user_copy_rc;
                    break;
                }
            }
        }

        // Install new handler (SIGKILL and SIGSTOP cannot be caught or ignored)
        if (act && signum != SIGKILL && signum != SIGSTOP) {
            // Validate user function pointers are not in kernel space
            if ((uint64_t)kact.sa_restorer >= current->addr_limit ||
                (uint64_t)kact.sa_handler  >= current->addr_limit) {
                regs->rax = -EINVAL;
                break;
            }
            current->sighand[signum].sa_handler  = kact.sa_handler;
            current->sighand[signum].sa_flags    = kact.sa_flags;
            current->sighand[signum].sa_restorer = kact.sa_restorer;
            current->sighand[signum].sa_mask     = kact.sa_mask;
        }
        regs->rax = 0;
        break;
    }
    case SYS_sigprocmask: {
        // sigprocmask(int how, const sigset_t *set, sigset_t *oldset).
        // NULL → skip; otherwise _ft bounce.
        int how = (int)regs->rdi;
        const sigset_t *set = (const sigset_t *)regs->rsi;
        sigset_t *oldset = (sigset_t *)regs->rdx;

        // Return current mask if requested
        if (oldset) {
            if (!syscall_check_user_range((uint64_t)oldset,
                                          sizeof(sigset_t), true)) {
                regs->rax = -EFAULT;
                break;
            }
            sigset_t kold = (sigset_t)current->blocked;
            {
                ssize_t user_copy_rc = copy_to_user_ft(oldset, &kold, sizeof(kold));
                if (user_copy_rc < 0) {
                    regs->rax = user_copy_rc;
                    break;
                }
            }
        }

        // Update mask if set is provided
        if (set) {
            if (!syscall_check_user_range((uint64_t)set,
                                          sizeof(sigset_t), false)) {
                regs->rax = -EFAULT;
                break;
            }
            sigset_t kset;
            if (copy_from_user_ft(&kset, set, sizeof(kset)) < 0) {
                regs->rax = -EFAULT;
                break;
            }
            switch (how) {
            case SIG_BLOCK:
                current->blocked |= kset;
                break;
            case SIG_UNBLOCK:
                current->blocked &= ~kset;
                break;
            case SIG_SETMASK:
                current->blocked = (int64_t)kset;
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
        // Read the whole frame via VIRTUAL addresses (per-page walker)
        // so a frame that crosses a 4KB page boundary is read correctly;
        // the old user_va_to_phys+Phy_To_Virt only translated the start
        // and would read junk for the second page under 4KB mappings.
        struct sigframe frame;
        if (!syscall_check_user_range(regs->rsp, sizeof(frame), false) ||
            copy_from_user_ft(&frame, (void *)regs->rsp,
                              sizeof(frame)) < 0) {
            regs->rax = -EFAULT;
            break;                       // other regs untouched
        }

        // Validate sigframe: iretq CS must be ring-3
        if ((frame.cs & 3) != 3) { regs->rax = -EINVAL; break; }

        // Restore blocked mask
        current->blocked = frame.blocked;

        // Restore all GPRs (from kernel-stack frame only)
        regs->r15=frame.r15; regs->r14=frame.r14; regs->r13=frame.r13;
        regs->r12=frame.r12; regs->r11=frame.r11; regs->r10=frame.r10;
        regs->r9=frame.r9;   regs->r8=frame.r8;
        regs->rbx=frame.rbx; regs->rcx=frame.rcx; regs->rdx=frame.rdx;
        regs->rsi=frame.rsi; regs->rdi=frame.rdi; regs->rbp=frame.rbp;
        regs->ds=frame.ds;   regs->es=frame.es;    regs->rax=frame.rax;

        // Restore iretq frame → RESTORE_ALL → iretq to original context
        regs->rip=frame.rip; regs->cs=frame.cs; regs->rflags=frame.rflags;
        regs->rsp=frame.rsp; regs->ss=frame.ss;

        // Note: do_system_call's tail arch_do_signal_delivery() runs after
        // this break.  If there are additional pending signals, they
        // will be delivered on the freshly-restored stack — matching
        // Linux behavior (sigreturn processes remaining signals before
        // the final iretq to userspace).
        break;
    }
    case SYS_socket: {
        regs->rax = do_socket((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
        break;
    }
    case SYS_connect: {
        // Cat B: copy user sockaddr_in to kernel via _ft, then call.
        if (regs->rdx < sizeof(struct sockaddr_in) ||
            !syscall_check_user_range(regs->rsi,
                                      sizeof(struct sockaddr_in), false)) {
            regs->rax = -EFAULT; break;
        }
        struct sockaddr_in addr;
        if (copy_from_user_ft(&addr, (void *)regs->rsi, sizeof(addr)) < 0) {
            regs->rax = -EFAULT; break;
        }
        // sin_port is network byte order; lwIP netconn_connect wants host order.
        regs->rax = do_connect((int)regs->rdi, addr.sin_addr, os01_ntohs(addr.sin_port));
        break;
    }
    case SYS_sendto: {
        // Cat B: addr → kernel copy via _ft.  buf goes through
        // Task 8 (Cat C VFS bounce) — the kernel→user bounce
        // for write direction; here we only validate the buf range.
        uint64_t len = regs->rdx;
        uint32_t ip = 0; uint16_t port = 0;
        uint64_t addr_ptr = regs->r8;
        if (len && !syscall_check_user_range(regs->rsi, len, false)) {
            regs->rax = -EFAULT; break;
        }
        if (addr_ptr) {
            if (regs->r9 < sizeof(struct sockaddr_in) ||
                !syscall_check_user_range(addr_ptr,
                                          sizeof(struct sockaddr_in), false)) {
                regs->rax = -EFAULT; break;
            }
            struct sockaddr_in a;
            if (copy_from_user_ft(&a, (void *)addr_ptr, sizeof(a)) < 0) {
                regs->rax = -EFAULT; break;
            }
            ip = a.sin_addr; port = a.sin_port;
        }
        regs->rax = do_sendto((int)regs->rdi, (void *)regs->rsi,
                              len, (int)regs->r10, ip, os01_ntohs(port));
        break;
    }
    case SYS_recvfrom: {
        // Cat B: addrlen → kernel via _ft; addr/buf write-back via _ft.
        // Only the _ft write-back success commits the recv (do_recvfrom
        // already consumed the netbuf; on _ft failure we return -EFAULT
        // but the data is gone — POSIX semantics; the user must retry).
        uint64_t addr_ptr = regs->r8;
        uint64_t addrlen_ptr = regs->r9;
        if (regs->rdx && !syscall_check_user_range(regs->rsi, regs->rdx, true)) {
            regs->rax = -EFAULT; break;
        }
        uint32_t ip = 0;
        uint16_t port = 0;
        uint32_t addrlen = 0;
        if (addr_ptr) {
            if (!syscall_check_user_range(addrlen_ptr, sizeof(addrlen), false)) {
                regs->rax = -EFAULT; break;
            }
            if (copy_from_user_ft(&addrlen, (void *)addrlen_ptr, sizeof(addrlen)) < 0) {
                regs->rax = -EFAULT; break;
            }
            if (addrlen < sizeof(struct sockaddr_in) ||
                !syscall_check_user_range(addr_ptr,
                                          sizeof(struct sockaddr_in), true)) {
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
            {
                ssize_t user_copy_rc = copy_to_user_ft((void *)addr_ptr, &src, sizeof(src));
                if (user_copy_rc < 0) {
                    regs->rax = user_copy_rc; break;
                }
            }
            uint32_t new_addrlen = sizeof(src);
            {
                ssize_t user_copy_rc = copy_to_user_ft((void *)addrlen_ptr, &new_addrlen,
                                sizeof(new_addrlen));
                if (user_copy_rc < 0) {
                    regs->rax = user_copy_rc; break;
                }
            }
        }
        regs->rax = ret;
        break;
    }
    case SYS_bind: {
        // Cat B: copy user sockaddr_in to kernel via _ft.
        if (regs->rdx < sizeof(struct sockaddr_in) ||
            !syscall_check_user_range(regs->rsi,
                                      sizeof(struct sockaddr_in), false)) {
            regs->rax = -EFAULT; break;
        }
        struct sockaddr_in a;
        if (copy_from_user_ft(&a, (void *)regs->rsi, sizeof(a)) < 0) {
            regs->rax = -EFAULT; break;
        }
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
        // Cat B: optlen is a hostile r8 — cap + reject 0 BEFORE kmalloc
        // to prevent DoS via giant allocations.  Read optval via _ft.
        uint64_t optlen = regs->r8;
        if (optlen == 0 || optlen > SOCKOPT_MAX) {
            regs->rax = -EINVAL; break;
        }
        if (!syscall_check_user_range(regs->r10, optlen, false)) {
            regs->rax = -EFAULT; break;
        }
        void *optval = kmalloc(optlen);
        if (!optval) { regs->rax = -ENOMEM; break; }
        if (copy_from_user_ft(optval, (void *)regs->r10, optlen) < 0) {
            kfree(optval);
            regs->rax = -EFAULT; break;
        }
        regs->rax = do_setsockopt((int)regs->rdi, (int)regs->rsi,
                                  (int)regs->rdx, optval, optlen);
        kfree(optval);
        break;
    }
    case SYS_getsockname: {
        // Cat B: do_getsockname into kernel sockaddr_in + klen, then
        // _ft write both back to user (so a hostile user pointer in
        // rsi/rdx can't make the kernel scribble into itself).
        if (!syscall_check_user_range(regs->rsi,
                                      sizeof(struct sockaddr_in), true) ||
            !syscall_check_user_range(regs->rdx, sizeof(uint32_t), true)) {
            regs->rax = -EFAULT; break;
        }
        struct sockaddr_in kaddr;
        uint32_t klen = sizeof(kaddr);
        int64_t ret = do_getsockname((int)regs->rdi, &kaddr, &klen);
        if (ret < 0) { regs->rax = ret; break; }
        ssize_t wr = copy_to_user_ft((void *)regs->rsi, &kaddr, sizeof(kaddr));
        if (wr >= 0) wr = copy_to_user_ft((void *)regs->rdx, &klen, sizeof(klen));
        if (wr < 0) { regs->rax = wr; break; }
        regs->rax = ret;
        break;
    }
    case SYS_getifaddr: {
        regs->rax = do_getifaddr();
        break;
    }
    case SYS_getsockopt: {
        // Cat B: read user optlen (r8 points to user uint32_t) FIRST,
        // bounded by SOCKOPT_MAX.  Only then allocate + do_getsockopt
        // + _ft write-back.  This prevents DoS via r8=0xFFFFFFFF
        // (which would kmalloc a 4GB buffer).
        if (!syscall_check_user_range(regs->r8, sizeof(uint32_t), false)) {
            regs->rax = -EFAULT; break;
        }
        uint32_t klen = 0;
        if (copy_from_user_ft(&klen, (void *)regs->r8, sizeof(klen)) < 0) {
            regs->rax = -EFAULT; break;
        }
        if (klen > SOCKOPT_MAX) { regs->rax = -EINVAL; break; }
        void *kopt = kmalloc(klen ? klen : 1);
        if (!kopt) { regs->rax = -ENOMEM; break; }
        int64_t ret = do_getsockopt((int)regs->rdi, (int)regs->rsi,
                                    (int)regs->rdx, kopt, &klen);
        if (ret < 0) {
            kfree(kopt);
            regs->rax = ret;
            break;
        }
        ssize_t wr = copy_to_user_ft((void *)regs->r10, kopt, klen);
        ssize_t wlr = copy_to_user_ft((void *)regs->r8, &klen, sizeof(klen));
        kfree(kopt);
        if (wr < 0 || wlr < 0) { regs->rax = wr < 0 ? wr : wlr; break; }
        regs->rax = ret;
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

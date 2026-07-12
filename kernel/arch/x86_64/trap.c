#include <kernel/arch/x86_64/trap.h>
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
#include <stdlib.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/file.h>
#include <device/timer.h>
#include <uapi/time.h>
#include <kernel.h>
#include <kernel/vma.h>
// ── Local signal constants (kernel has its own signal.h) ──
#ifndef SIG_BLOCK
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2
typedef unsigned long sigset_t;
#endif

// ── User address translation ─────────────────────────────────
// Walk the user page table to resolve a user-space virtual
// address to its physical address.  Returns 0 on failure.
// The caller passes Phy_To_Virt(result) to get a kernel pointer.
static uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va)
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

    // Mark the task ZOMBIE now, so the reaper can clean it up later.
    // The actual resource cleanup (vmm_free_user_map, kfree) happens
    // in do_exit() which we redirect to via iretq.
    task->state = TASK_ZOMBIE;

    // Overwrite the iretq target so the task "returns" to do_exit
    // running in ring 0 on its own kernel stack — where
    // get_current_task() works correctly.
    regs->rip  = (uint64_t)do_exit;
    regs->cs   = ARCH_KERNEL_CS;
    regs->ss   = ARCH_KERNEL_DS;
    regs->rsp  = (uint64_t)task + STACK_SIZE;  // task's kernel stack (NOT user RSP!)
    regs->ds   = ARCH_KERNEL_DS;
    regs->es   = ARCH_KERNEL_DS;
    regs->rdi  = 0;              // exit code for do_exit
    regs->rflags = (1 << 9);     // IF=1
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

			if (vma->vm_file) {
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

int do_signal_delivery(pt_regs_t *regs)
{
    uint64_t pending = current->signal;
    if (!pending)
        return 0;

    // ── NULL regs path (tty.c inline signal clear) ─────────
    // tty.c:267 calls do_signal_delivery(NULL) when a direct
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
                    current->signal &= ~(1ULL << sig);
                    do_exit((uint64_t)sig << 8);
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
                log_err("task %d killed by signal %d (default)\n",
                        (int)current->pid, sig);
                do_exit((uint64_t)sig << 8);
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
            do_exit((uint64_t)sig << 8);
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

int signal_pending_fatal(void)
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
    // Linux x86_64 ABI translation (for busybox etc.)
    if ((current->flags & PF_LINUX_ABI) && regs->rax < 256) {
        static const int8_t linux_to_os01[256] = {
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
        };
        int8_t os = linux_to_os01[regs->rax];
        if (os >= 0)
            regs->rax = os;
    }
    switch (regs->rax) {
    // ── Syscall name table (for strace) ─────────────────────
    static const char *syscall_names[64] = {
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
    };
    const char *sname = (regs->rax < 64 && syscall_names[regs->rax])
                        ? syscall_names[regs->rax] : "?";
    debug_syscall("[strace] pid=%d syscall(%s, arg1=%#lx, arg2=%#lx, arg3=%#lx)\n",
                  (int)current->pid, sname,
                  (unsigned long)regs->rdi,
                  (unsigned long)regs->rsi,
                  (unsigned long)regs->rdx);
    case SYS_putchar: {
        // putchar(int c) — write one char to framebuffer AND serial
        char c = (char)regs->rdi;
        color_printk(WHITE, BLACK, "%c", c);
        write_serial(c);  // also echo to serial for interactive shell
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
        memcpy(current->files->cwd, new_cwd, sizeof(current->files->cwd));

        log_info("chdir: pid=%d → '%s'\n", (int)current->pid, current->files->cwd);
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
        if (!f->node) { regs->rax = -ENOENT; break; }

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
            regs->rax = -EINVAL;
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
            int newfd = -1;
            for (int i = start; i < NOFILE; i++) {
                if (current->files->fd[i] == NULL) {
                    newfd = i;
                    break;
                }
            }
            if (newfd < 0) { regs->rax = -ENFILE; break; }
            f->refcount++;
            current->files->fd[newfd] = f;
            regs->rax = newfd;
            break;
        }
        case F_DUPFD_CLOEXEC: {
            // Same as F_DUPFD for now (close-on-exec not implemented)
            int start = (int)arg;
            if (start < 0) start = 0;
            int newfd = -1;
            for (int i = start; i < NOFILE; i++) {
                if (current->files->fd[i] == NULL) {
                    newfd = i;
                    break;
                }
            }
            if (newfd < 0) { regs->rax = -ENFILE; break; }
            f->refcount++;
            current->files->fd[newfd] = f;
            regs->rax = newfd;
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
        // ioctl(int fd, unsigned long request, uint64_t arg) → 0 / -errno
        int fd = (int)regs->rdi;
        uint64_t request = regs->rsi;
        uint64_t arg = regs->rdx;

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            regs->rax = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];

        switch (request) {
        case TIOCGWINSZ: {
            // Return terminal window size
            struct winsize *ws = (struct winsize *)arg;
            if ((uint64_t)ws >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            ws->ws_row = 24;
            ws->ws_col = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            regs->rax = 0;
            break;
        }
        case TCGETS: {
            // tcgetattr — return default termios (canonical + echo)
            struct termios *tio = (struct termios *)arg;
            if ((uint64_t)tio >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            tio->c_iflag = ICRNL | IXON | BRKINT;
            tio->c_oflag = OPOST | ONLCR;
            tio->c_cflag = CS8 | CREAD | CLOCAL;
            tio->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL;
            tio->c_line = 0;
            tio->c_cc[VINTR]    = 0x03;
            tio->c_cc[VQUIT]    = 0x1c;
            tio->c_cc[VERASE]   = 0x7f;
            tio->c_cc[VKILL]    = 0x15;
            tio->c_cc[VEOF]     = 0x04;
            tio->c_cc[VTIME]    = 0;
            tio->c_cc[VMIN]     = 1;
            tio->c_cc[VSTART]   = 0x11;
            tio->c_cc[VSTOP]    = 0x13;
            tio->c_cc[VSUSP]    = 0x1a;
            tio->c_cc[VEOL]     = 0;
            tio->c_cc[VREPRINT] = 0x12;
            tio->c_cc[VDISCARD] = 0x0f;
            tio->c_cc[VWERASE]  = 0x17;
            tio->c_cc[VLNEXT]   = 0x16;
            tio->c_cc[VEOL2]    = 0;
            tio->__c_ispeed = 38400;
            tio->__c_ospeed = 38400;
            regs->rax = 0;
            break;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            // tcsetattr — no-op
            regs->rax = 0;
            break;
        case TIOCGPGRP: {
            regs->rax = (int64_t)current->pid;
            break;
        }
        case TIOCSPGRP: {
            regs->rax = 0;
            break;
        }
        case TIOCNOTTY: {
            regs->rax = 0;
            break;
        }
        default:
            regs->rax = -ENOTTY;
            break;
        }
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
                if (!node->ops || !node->ops->write)
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
    case SYS_nanosleep: {
        // nanosleep(const struct timespec *req, struct timespec *rem)
        const struct timespec *req = (const struct timespec *)regs->rdi;
        uint64_t ns = 0;
        if (req && (uint64_t)req < current->addr_limit) {
            ns = req->tv_sec * 1000000000ULL + req->tv_nsec;
        }
        // Convert nanoseconds to jiffies (1 jiffy = 10ms = 10,000,000 ns)
        uint64_t ticks = (ns + 9999999) / 10000000;
        if (ticks == 0 && ns > 0) ticks = 1;

        uint64_t target = jiffies + ticks;
        while (jiffies < target) {
            current->state = TASK_INTERRUPTIBLE;
            schedule();
            // A signal (e.g. SIGINT via kill) may have woken
            // us early.  Break out so do_signal_delivery can
            // process it on syscall return.
            if (current->signal)
                break;
        }
        current->state = TASK_RUNNING;
        regs->rax = 0;
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
        // kill(int pid, int sig) → 0 / -ESRCH / -EINVAL
        int pid = (int)(int64_t)regs->rdi;
        int sig = (int)regs->rsi;

        if (sig < 1 || sig >= NSIG) {
            regs->rax = -EINVAL;
            break;
        }

        // Find the target task in the global task list
        task_t *target = NULL;
        {
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                pos = pos->next;
                if (t->pid == pid) {
                    target = t;
                    break;
                }
            }
        }

        if (!target) {
            regs->rax = -ESRCH;
            break;
        }

        // Set the pending signal bit
        target->signal |= (1ULL << sig);

        // Wake the target if it's sleeping interruptibly —
        // otherwise the signal won't take effect until the
        // target is woken by I/O or a timer tick.
        if (target->state == TASK_INTERRUPTIBLE)
            target->state = TASK_RUNNING;

        regs->rax = 0;
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

        // Note: do_system_call's tail do_signal_delivery() runs after
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
            outw(0x2000, apic_info.pm1a_port);
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
        do_signal_delivery(regs);
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
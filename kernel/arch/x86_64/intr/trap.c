#include <arch/x86_64/trap.h>
#include <arch/irq.h>
#include <arch/x86_64/gate.h>
#include <arch/x86_64/hw.h>
#include <arch/x86_64/unwind.h>
#include <arch/segment.h>
#include <arch/x86_64/tss.h>
#include <arch/x86_64/msr_safe.h>   /* arch_msr_gp_fixup (Task 1 safe-RDMSR fixup) */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <core/printk.h>
#include <log/log.h>
#include <core/trace.h>
#include <arch/early_print.h>
#include <arch/x86_64/asm.h>
#include <sched/task.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <memory/pmm.h>
#include <percpu/percpu.h>
#include <intr/apic.h>
#include <memory/slab.h>
#include <memory/shared_backing.h>
#include <driver/serial.h>
#include <errno.h>
#include <uapi/syscall.h>
#include <uapi/time.h>            // SIGSEGV / SIGFPE / SIGBUS / SIGILL
#include <syscall/dispatch.h>
#include <syscall/compat.h>
#include <string.h>
#include <core/debug.h>
#include <memory/uaccess.h>
#include <kernel.h>
#include <memory/vma.h>
#include <arch/x86_64/vma.h>   // x86_vma_pte_flags: logical bits -> leaf
#include <intr/dispatch.h>       // arch_intr_dispatch + struct intr_event (intr-dispatch phase 1)
#include <arch/intr.h>           // strong backend facade
#include <arch/x86_64/intr_decode.h>   // production event decoder
#include <arch/x86_64/intr_dispatch.h>
#include <arch/x86_64/intr_vectors.h> // private business handler handoff
#include <arch/x86_64/vmm.h>
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
//
// `sig` is the fatal signal to record as the task's termination reason.
// It is forwarded to do_exit() which stores it verbatim into
// current->exit_code; waitpid() decodes the low byte as a signal
// (WIFSIGNALED / WTERMSIG) per the encoding in kernel/syscall/sys_native.c
// (the SYS_exit path puts the user-supplied code in the high byte; signal
// deaths put the signal in the low byte).  This used to always pass 0,
// which made a parent waitpid() see WIFEXITED && WEXITSTATUS == 0 —
// indistinguishable from a clean exit(0), silently breaking POSIX signal
// semantics for any shell / init / busybox `wait` / `trap` consumer.
static void kill_current_user_task(pt_regs_t *regs, int sig)
{
    task_t *task = task_from_tss();

    if (!task || (task->flags & PF_KTHREAD)) {
        log_err("User fault with no user task\n");
        return;
    }

    log_err("Killing task %d (user fault at RIP=%p, signal=%d)\n",
            task->pid, regs->rip, sig);

    // do_exit owns the ZOMBIE transition after cleanup and exit_code are
    // ready. Once back on the task stack IRQs may be restored: publishing
    // ZOMBIE here would let a timer switch us out and a waiter reap it with
    // the default exit_code before do_exit has finished.

    // Switch to the task's own kernel stack and call do_exit.
    // We CANNOT use iretq for a ring-0 return here.  When iretq
    // keeps the same privilege level (ring 0 → ring 0) it does
    // NOT pop RSP/SS from the frame, so we would remain on the
    // IST exception stack.  get_current_task() (RSP masking)
    // would return garbage, causing memory corruption or a crash.
    //
    // Instead, switch RSP to the task's kernel stack directly and
    // call do_exit with `sig` as the exit_code (low byte = signal
    // per the encoding above).  RSP is set to (task + STACK_SIZE - 8)
    // so that get_current_task() returns the correct task struct
    // (RSP masking rounds down to the STACK_SIZE-aligned base).
    // The -8 also mimics the stack state after a normal function
    // call (return address below the top) so do_exit's prologue
    // and sub-functions work correctly.  Since do_exit() calls
    // schedule() (which context-switches away and never returns),
    // the compiler is told this path is unreachable.
    __asm__ __volatile__(
        "movq %[stack_top], %%rsp\n\t"
        "movl %[sig], %%edi\n\t"        // rdi = sig (low byte = signal)
        /* Never enable IRQs on IST. Restore only saved IF after the pivot;
         * user exit cleanup may block while dropping file references. */
        "testq $0x200, %[saved_flags]\n\t"
        "jz 1f\n\t"
        "sti\n\t"
        "1: call do_exit\n\t"
        :
        : [stack_top] "r"((uint64_t)task + STACK_SIZE - 8),
          [sig]      "r"(sig),
          [saved_flags] "r"(regs->rflags)
        : "edi", "cc", "memory"
    );
    __builtin_unreachable();
}

// Check for user-mode fault and kill the task if so.  Returns 1 if
// the fault was user-mode (caller should return immediately), 0 if
// kernel-mode (caller should continue with kernel fault handling).
// `sig` is the fatal signal to record (passed through to
// kill_current_user_task and ultimately do_exit).
static inline int handle_user_fault(pt_regs_t *regs, const char *name, int sig)
{
    if (regs->cs & 3) {
        log_err("%s from user, killing task (signal=%d)\n", name, sig);
        kill_current_user_task(regs, sig);
        return 1;
    }
    return 0;
}

/* ── Lock-free fatal exception reporter (Task 3) ─────────────────── */

static void x86_fatal_put_hex64(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    arch_early_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        arch_early_putc(hex[(val >> shift) & 0xfU]);
    }
}

static void x86_fatal_put_dec(unsigned val)
{
    char buf[16];
    int len = 0;
    if (val == 0) {
        arch_early_putc('0');
        return;
    }
    while (val != 0 && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (int)(val % 10U));
        val /= 10U;
    }
    while (len > 0) {
        arch_early_putc(buf[--len]);
    }
}

static uint32_t x86_fatal_winner = 0;

static void x86_fatal_begin(const char *name, unsigned vector,
                            const struct pt_regs *regs, uint64_t error)
{
    __asm__ __volatile__("cli" ::: "memory");
    if (__atomic_exchange_n(&x86_fatal_winner, 1U, __ATOMIC_ACQ_REL) != 0U) {
        for (;;) {
            hlt();
        }
    }
    arch_early_puts(name);
    arch_early_putc('(');
    x86_fatal_put_dec(vector);
    arch_early_puts("),ERROR_CODE:");
    x86_fatal_put_hex64(error);
    arch_early_puts(",RSP:");
    x86_fatal_put_hex64(regs->rsp);
    arch_early_puts(",RIP:");
    x86_fatal_put_hex64(regs->rip);
    arch_early_putc('\n');
}

__attribute__((no_stack_protector))
static _Noreturn void x86_fatal_exception(const char *name, unsigned vector,
                                          const struct pt_regs *regs,
                                          uint64_t error, enum trace_mode mode)
{
    x86_fatal_begin(name, vector, regs, error);
    trace_exception(regs, mode);
    for (;;) {
        hlt();
    }
}

__attribute__((no_stack_protector))
static _Noreturn void x86_fatal_page_fault(const struct pt_regs *regs,
                                           uint64_t error, uint64_t cr2)
{
    x86_fatal_begin("do_page_fault", 14U, regs, error);
    arch_early_puts("PF-KRN: err=");
    x86_fatal_put_hex64(error);
    arch_early_puts(" rip=");
    x86_fatal_put_hex64(regs->rip);
    arch_early_puts(" rsp=");
    x86_fatal_put_hex64(regs->rsp);
    arch_early_puts(" rbp=");
    x86_fatal_put_hex64(regs->rbp);
    arch_early_puts(" cr2=");
    x86_fatal_put_hex64(cr2);
    arch_early_putc('\n');
    trace_exception(regs, TRACE_FULL);
    for (;;) {
        hlt();
    }
}

/* Installed exception adapters preserve the native assembly ABI. */

void x86_handle_divide_error(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_divide_error", SIGFPE)) return;
    x86_fatal_exception("do_divide_error", 0U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_divide_error(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_DIVIDE_ERROR, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

__attribute__((no_stack_protector))
void do_debug(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_DEBUG, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_debug(pt_regs_t * regs, uint64_t error_code)
{
    x86_fatal_exception("do_debug", 1U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_nmi(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_NMI, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_nmi(pt_regs_t * regs, uint64_t error_code)
{
    x86_fatal_exception("do_nmi", 2U, regs, error_code, TRACE_PC_ONLY);
}

__attribute__((no_stack_protector))
void do_int3(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_INT3, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_int3(pt_regs_t * regs, uint64_t error_code)
{
    x86_fatal_exception("do_int3", 3U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_overflow(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_OVERFLOW, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_overflow(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_overflow", SIGFPE)) return;
    x86_fatal_exception("do_overflow", 4U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_bounds(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_BOUNDS, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_bounds(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_bounds", SIGFPE)) return;
    x86_fatal_exception("do_bounds", 5U, regs, error_code, TRACE_FULL);
}

void x86_handle_undefined_opcode(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_undefined_opcode", SIGILL)) return;
    x86_fatal_exception("do_undefined_opcode", 6U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_undefined_opcode(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_UNDEFINED_OPCODE, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

__attribute__((no_stack_protector))
void do_dev_not_available(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_DEV_NOT_AVAILABLE, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_dev_not_available(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_dev_not_available", SIGFPE)) return;
    x86_fatal_exception("do_dev_not_available", 7U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_double_fault(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_DOUBLE_FAULT, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_double_fault(pt_regs_t * regs, uint64_t error_code)
{
    x86_fatal_exception("do_double_fault", 8U, regs, error_code, TRACE_PC_ONLY);
}

__attribute__((no_stack_protector))
void do_coprocessor_segment_overrun(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_COPROCESSOR_SEGMENT_OVERRUN, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_coprocessor_segment_overrun(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_coprocessor_segment_overrun", SIGFPE)) return;
    x86_fatal_exception("do_coprocessor_segment_overrun", 9U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_invalid_TSS(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_INVALID_TSS, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_invalid_TSS(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_invalid_TSS", SIGSEGV)) return;
    x86_fatal_exception("do_invalid_TSS", 10U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_segment_not_present(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_SEGMENT_NOT_PRESENT, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_segment_not_present(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_segment_not_present", SIGBUS)) return;
    x86_fatal_exception("do_segment_not_present", 11U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_stack_segment_fault(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_STACK_SEGMENT_FAULT, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_stack_segment_fault(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_stack_segment_fault", SIGBUS)) return;
    x86_fatal_exception("do_stack_segment_fault", 12U, regs, error_code, TRACE_FULL);
}

void x86_handle_general_protection(pt_regs_t * regs, uint64_t error_code)
{
	// Instruction-scoped safe RDMSR fixup (thermal plan Task 1).
	// MUST run before any logging / user-mode dispatch: a marked
	// rdmsr (CPL==0, err==0, RIP==arch_rdmsr_safe_try) is silently
	// redirected to the fail label and the original #GP is consumed.
	// Any other fault falls through unchanged.
	if (arch_msr_gp_fixup(regs, error_code))
		return;

	// User-mode fault → kill the task, don't halt the kernel
	// NOTE: on IST stack — do NOT use current (get_current_task).
	if (regs->cs & 3) {
		task_t *t = task_from_tss();
		log_err("do_general_protection(13) from user, killing task %d\n",
		        t ? t->pid : -1);
		kill_current_user_task(regs, SIGSEGV);
		return;
	}
	{
		uint64_t gp_cr2 = 0;
		__asm__ __volatile__("movq %%cr2, %0" : "=r"(gp_cr2) :: "memory");
		x86_fatal_begin("do_general_protection", 13U, regs, error_code);
		arch_early_puts(" CS=");
		x86_fatal_put_hex64(regs->cs);
		arch_early_puts(" SS=");
		x86_fatal_put_hex64(regs->ss);
		arch_early_puts(" EFLAGS=");
		x86_fatal_put_hex64(regs->rflags);
		arch_early_puts(" CR2=");
		x86_fatal_put_hex64(gp_cr2);
		arch_early_putc('\n');
		trace_exception(regs, TRACE_FULL);
		for (;;) {
			hlt();
		}
	}
}

__attribute__((no_stack_protector))
void do_general_protection(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    /* A #GP owns no fault address: cr2=0 here so the event decodes with
     * fault_addr_valid=false and native_address=0 (spec §6.1).  The
     * kernel fatal diagnostics inside the private handler keep their
     * own CR2 read purely for the log line. */
    x86_intr_decode(&ev, regs, X86_VECTOR_GENERAL_PROTECTION, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_page_fault(pt_regs_t * regs, uint64_t error_code, uint64_t cr2)
{
	// CR2 was snapshotted ONCE at the very front of the thin public
	// entry (do_page_fault); every use below consumes that snapshot —
	// this body never re-reads CR2 (spec §6.1).

	// Detect kernel-mode PF (IST 0 = task stack)
	if (!(regs->cs & 3)) {
		uint32_t lo = 0, hi = 0;
		__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000101U));
		uintptr_t gs_base = ((uintptr_t)hi << 32) | (uintptr_t)lo;
		if (x86_unwind_resolve_stack(gs_base, (uintptr_t)regs->rsp, NULL, NULL)) {
			task_t *t = (task_t *)(regs->rsp & ~(STACK_SIZE - 1));
			if (cr2 < t->addr_limit && t->fault_jmp)
				__builtin_longjmp(t->fault_jmp, 1);
		}
		x86_fatal_page_fault(regs, error_code, cr2);
	}

	// User-mode PF - VMA-based demand paging
	// NOTE: on IST stack - do NOT use current (get_current_task).
	if (regs->cs & 3) {
		task_t *t = task_from_tss();
		if (!t || !t->mm) {
			kill_current_user_task(regs, SIGSEGV);
			return;
		}

		vma_t *vma = vm_area_lookup(t->mm, cr2);
		if (!vma) {
			// Forked-image COW write: the ELF loader installs
			// page-table entries for the image but NO VMA for
			// them (no demand paging for the image), so
			// vm_area_lookup() legitimately returns NULL here.  When
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
					    x86_pte_4k_pa(*upte);
					uint64_t new_flags =
					    x86_pte_4k_flags(*upte) |
					    PAGE_WRITE;
					new_flags &= ~(uint64_t)PAGE_COW;
					bool privatized =
					    page_cow_refs(old_phys) > 1;
					if (privatized) {
						uint64_t nphys =
						    alloc_4k_page();
						if (!nphys) {
							kill_current_user_task(
							    regs, SIGSEGV);
							return;
						}
						memcpy(
						    (void *)Phy_To_Virt(nphys),
						    (void *)Phy_To_Virt(old_phys),
						    PAGE_4K_SIZE);
						*upte = nphys | new_flags;
					} else {
						/* In-place upgrade: the leaf
						 * keeps this PA, so the put
						 * must NEVER free it. */
						*upte = old_phys | new_flags;
					}
					flush_tlb();
					/* The put runs after the sync so
					 * another owner's reference is
					 * visible until then; only the
					 * privatized leaf's OLD PA is
					 * released after the flush. */
					bool last = page_cow_put(old_phys);
					if (privatized && last)
						free_4k_page(old_phys);
					return;
				}
			}
			log_debug("PF: pid=%d cr2=%p no vma\n", t->pid, cr2);
			kill_current_user_task(regs, SIGSEGV);
			return;
		}

		// -- Permission check --
		// error_code: bit 0=P, bit 1=W/R, bit 4=I/D

		// PROT_NONE VMA -> any access is SIGSEGV
		if (!(vma->vm_flags & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC))) {
			log_debug("PF: pid=%d cr2=%p PROTNONE\n", t->pid, cr2);
			kill_current_user_task(regs, SIGSEGV);
			return;
		}

		// Write protection violation (W=1, regardless of P): an
		// unauthorized first write must be rejected BEFORE the
		// demand path allocates a leaf — a P=0 write fault on a
		// read-only VMA never gets a page (PTE permissions plan).
		if ((error_code & 0x02) && !(vma->vm_flags & VMA_PROT_WRITE)) {
			log_debug("PF: pid=%d cr2=%p write to RO page\n",
			          t->pid, cr2);
			kill_current_user_task(regs, SIGSEGV);
			return;
		}

		// Instruction fetch (I=1)
		if ((error_code & 0x10) && !(vma->vm_flags & VMA_PROT_EXEC)) {
			kill_current_user_task(regs, SIGSEGV);
			return;
		}
		// -- COW resolution (P=1, W=1, VMA_PROT_WRITE is set) --
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
				int heap_vma = !!(vma->vm_flags & VMA_HEAP);
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
						kill_current_user_task(regs, SIGSEGV);
						return;
					}
				}

				uint64_t old_phys = x86_pte_4k_pa(*pte);
				uint64_t new_phys = 0;
				int resolved = 0;
				bool privatized = page_cow_refs(old_phys) > 1;
				if (privatized) {
					new_phys = alloc_4k_page();
					if (!new_phys) {
						if (heap_vma)
							spin_unlock_irqrestore(
							    &t->mm->lock,
							    lock_flags);
						kill_current_user_task(regs, SIGSEGV);
						return;
					}
					memcpy((void *)Phy_To_Virt(new_phys),
					       (void *)Phy_To_Virt(old_phys),
					       PAGE_4K_SIZE);
					*pte = x86_pte_4k_make(new_phys, x86_vma_pte_flags(vma->vm_flags));
					resolved = 1;
				} else {
					/* In-place upgrade: the leaf keeps
					 * this PA — put without free. */
					*pte = x86_pte_4k_make(old_phys, x86_vma_pte_flags(vma->vm_flags));
					resolved = 1;
				}

				if (heap_vma)
					spin_unlock_irqrestore(
					    &t->mm->lock, lock_flags);
				if (resolved)
					flush_tlb();
				/* The put runs after the sync (see the
				 * no-VMA branch above). */
				bool last = page_cow_put(old_phys);
				if (privatized && last)
					free_4k_page(old_phys);
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
		// where a stale VMA_HEAP VMA or a leftover 2 MiB huge
		// leaf could re-expose unmapped heap as writable.
		if (!(error_code & 0x01)) {
			/* Heap VMA absent-leaf guard: mm_set_brk pre-maps
			 * every committed leaf before publishing end_brk,
			 * so a P=0 fault inside the heap VMA can only be a
			 * stale/foreign access — never demand-map it (the
			 * kernel has no kernel-side demand paging for the
			 * heap; see isolation-design.md §5.2). */
			if (vma->vm_flags & VMA_HEAP) {
				log_debug("PF: pid=%d cr2=%p heap absent-leaf -> EFAULT\n",
				          t->pid, cr2);
				kill_current_user_task(regs, SIGSEGV);
				return;
			}

			uint64_t *user_pgd =
			    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pgdir);

			if (vma->vm_shared_backing) {
				uint64_t page_va = cr2 & PAGE_4K_MASK;
				uint64_t index = vma->vm_pgoff +
				    ((page_va - vma->vm_start) >> PAGE_4K_SHIFT);
				uint64_t phys = 0;
				if (shared_backing_page(vma->vm_shared_backing,
				                        index, &phys) != 0) {
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				if (vma->vm_flags & VMA_PROT_WRITE)
					shared_backing_mark_dirty(
					    vma->vm_shared_backing, index);
				if (x86_vmm_map_4k(user_pgd, phys, page_va,
				                   x86_vma_pte_flags(vma->vm_flags)) != 0) {
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				return;
			}

			if (vma->vm_flags & VMA_ANON) {
				uint64_t phys = alloc_4k_page();
				if (!phys) {
					log_debug("PF: pid=%d OOM\n", t->pid);
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				memset((void *)Phy_To_Virt(phys), 0, PAGE_4K_SIZE);
				int rc = x86_vmm_map_4k(user_pgd, phys,
							     cr2 & PAGE_4K_MASK,
							     x86_vma_pte_flags(vma->vm_flags));
				if (rc != 0) {
					free_4k_page(phys);
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				return;
			}

			if (vma->vm_file && !(vma->vm_flags & VMA_IO)) {
				uint64_t phys = alloc_4k_page();
				if (!phys) {
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				/* File reads are page-granular: the page
				 * base VA (cr2 aligned down) minus the
				 * VMA start, never the faulting byte
				 * offset — otherwise the whole page is
				 * loaded shifted by (cr2 & 0xFFF). */
				uint64_t file_off =
				    ((cr2 & PAGE_4K_MASK) - vma->vm_start)
				    + (vma->vm_pgoff << PAGE_4K_SHIFT);
				int n = vfs_read(vma->vm_file, file_off,
						  PAGE_4K_SIZE,
						  (void *)Phy_To_Virt(phys));
				if (n < 0) {
					free_4k_page(phys);
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				// Zero-fill tail to avoid leaking kernel data
				if ((size_t)n < PAGE_4K_SIZE)
				    memset((char *)Phy_To_Virt(phys) + n, 0,
				           PAGE_4K_SIZE - (size_t)n);
				int rc = x86_vmm_map_4k(user_pgd, phys,
							     cr2 & PAGE_4K_MASK,
							     x86_vma_pte_flags(vma->vm_flags));
				if (rc != 0) {
					free_4k_page(phys);
					kill_current_user_task(regs, SIGSEGV);
					return;
				}
				return;
			}
		}

		// Unhandled -> SIGSEGV
		kill_current_user_task(regs, SIGSEGV);
		return;
	}
	x86_fatal_page_fault(regs, error_code, cr2);
}

__attribute__((no_stack_protector))
int do_page_fault(pt_regs_t * regs, uint64_t error_code)
{
    /* CR2 is read EXACTLY ONCE, here at the very front — before the
     * decoder and before any helper that could itself fault (spec
     * §6.1).  The same snapshot feeds the decoder and the private
     * business handler. */
    uint64_t cr2 = 0;
    __asm__	__volatile__("movq	%%cr2,	%0":"=r"(cr2)::"memory");

    if (x86_unwind_fixup(regs, error_code, cr2)) {
        return 1;
    }

    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_PAGE_FAULT, error_code, cr2);
    arch_intr_dispatch(&ev, regs);
    return 0;
}

__attribute__((no_stack_protector))
void do_x87_FPU_error(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_X87_FPU_ERROR, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_x87_FPU_error(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_x87_FPU_error", SIGFPE)) return;
    x86_fatal_exception("do_x87_FPU_error", 16U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_alignment_check(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_ALIGNMENT_CHECK, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_alignment_check(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_alignment_check", SIGBUS)) return;
    x86_fatal_exception("do_alignment_check", 17U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_machine_check(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_MACHINE_CHECK, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_machine_check(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_machine_check", SIGBUS)) return;
    x86_fatal_exception("do_machine_check", 18U, regs, error_code, TRACE_PC_ONLY);
}

__attribute__((no_stack_protector))
void do_SIMD_exception(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_SIMD_EXCEPTION, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_SIMD_exception(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_SIMD_exception", SIGFPE)) return;
    x86_fatal_exception("do_SIMD_exception", 19U, regs, error_code, TRACE_FULL);
}

__attribute__((no_stack_protector))
void do_virtualization_exception(pt_regs_t * regs, uint64_t error_code)
{
    struct intr_event ev;
    x86_intr_decode(&ev, regs, X86_VECTOR_VIRTUALIZATION_EXCEPTION, error_code, 0);
    arch_intr_dispatch(&ev, regs);
}

void x86_handle_virtualization_exception(pt_regs_t * regs, uint64_t error_code)
{
    if (handle_user_fault(regs, "do_virtualization_exception", SIGBUS)) return;
    x86_fatal_exception("do_virtualization_exception", 20U, regs, error_code, TRACE_FULL);
}


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
    uint64_t pending = (uint64_t)__atomic_load_n(&current->signal,
                                                 __ATOMIC_ACQUIRE);
    if (!pending)
        return 0;

    // ── NULL regs path (tty.c inline signal clear) ─────────
    // tty.c calls arch_do_signal_delivery(NULL) when a direct
    // switch bypassed ret_from_intr.  A CPL0 return keeps pending
    // signals pending (unmap/TLB ownership plan Task 1): here we
    // ONLY clear SIG_IGN and non-fatal SIG_DFL signals.  Fatal
    // signals stay pending and are delivered on the normal return
    // to ring3, where the fatal path below runs.
    if (!regs) {
        for (int sig = 1; sig < NSIG; sig++) {
            if (!(pending & (1ULL << sig)))
                continue;
            void (*handler)(int) = current->sighand[sig].sa_handler;
            if (handler == SIG_IGN) {
                __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                                   __ATOMIC_ACQ_REL);
                continue;
            }
            if (handler == SIG_DFL) {
                switch (sig) {
                case SIGCHLD: case SIGURG: case SIGWINCH:
                case SIGCONT: case SIGTSTP: case SIGTTIN: case SIGTTOU:
                    __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                                       __ATOMIC_ACQ_REL);
                    break;
                default:
                    // Fatal on a NULL frame: keep pending for the
                    // normal ring3 return (PID 1 also just clears,
                    // matching the init contract).
                    if (current->pid == 1)
                        __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                                           __ATOMIC_ACQ_REL);
                    break;
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
            __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                               __ATOMIC_ACQ_REL);
            continue;
        }

        if (handler == SIG_DFL) {
            __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                               __ATOMIC_ACQ_REL);
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
                /* Fatal delivery runs ONLY on ring3 frames (spec:
                 * "IRQ 返回仅在 ring3 交付致命信号").  A CPL0
                 * IRQ-return frame — the task was interrupted inside
                 * a syscall — keeps the signal pending: it dies at
                 * the syscall return, where the delivery runs with
                 * the user frame and no kernel locks are abandoned
                 * mid-syscall. */
                if (!(regs->cs & 3)) {
                    __atomic_fetch_or(&current->signal, (1ULL << sig),
                                      __ATOMIC_RELEASE);
                    break;
                }
                log_err("task %d killed by signal %d (default)\n",
                        (int)current->pid, sig);
                /* do_exit can block; the IRQ-return path may reach
                 * here with interrupts disabled.  Restore the IF
                 * flag the user frame saved before entering the
                 * exit path (unmap/TLB ownership plan Task 1). */
                if (regs->rflags & 0x200ULL) {
                    __asm__ __volatile__("sti" ::: "memory");
                }
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
            __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                               __ATOMIC_ACQ_REL);
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

        __atomic_fetch_and(&current->signal, ~(1ULL << sig),
                           __ATOMIC_ACQ_REL);
        return 1;  // handler delivered
    }
    return 0;  // nothing deliverable
}

int arch_signal_pending_fatal(void)
{
    uint64_t pending = (uint64_t)__atomic_load_n(&current->signal,
                                                 __ATOMIC_ACQUIRE);
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
    context_run_return_work(regs, CONTEXT_RETURN_SIGNAL_ONLY);
}

void sys_vector_install()
{
    /* IST stacks are not task stacks: ordinary IRQ accounting/return work
     * cannot use current there. Interrupt gates mask IRQs before the first
     * instruction; saved RFLAGS still preserves the interrupted IF state.
     * Keep user-callable DPL=3, and leave task-stack #PF/syscall unchanged. */
    set_intr_gate_raw(0,1,divide_error);
    set_intr_gate_raw(1,1,debug);
	set_intr_gate_raw(2, 1, nmi);
	set_system_intr_gate(3,1,int3);
	set_system_intr_gate(4,1,overflow);
	set_system_intr_gate(5,1,bounds);
	set_intr_gate_raw(6,1,undefined_opcode);
	set_intr_gate_raw(7,1,dev_not_available);
	set_intr_gate_raw(8,3,double_fault);  // IST 3 = dedicated double fault stack
	set_intr_gate_raw(9,1,coprocessor_segment_overrun);
	set_intr_gate_raw(10,1,invalid_TSS);
	set_intr_gate_raw(11,1,segment_not_present);
	set_intr_gate_raw(12,1,stack_segment_fault);
	set_intr_gate_raw(13,1,general_protection);
	set_trap_gate(14,0,page_fault);   // IST 0: run on task's kernel stack.
	                                     // COW handler can trigger a reschedule via
	                                     // ret_from_intr, and schedule()'s get_current_task()
	                                     // (RSP & ~0x7FFF) breaks on IST stacks.
	//15 Intel reserved. Do not use.
	set_intr_gate_raw(16,1,x87_FPU_error);
	set_intr_gate_raw(17,1,alignment_check);
	set_intr_gate_raw(18,1,machine_check);
	set_intr_gate_raw(19,1,SIMD_exception);
	set_intr_gate_raw(20,1,virtualization_exception);

	// int 0x80 syscall gate — DPL=3 so user code can call it
	set_system_gate(0x80, 0, system_call);
}

#include <arch/x86_64/linkage.h>
#include <arch/context.h>
#include <percpu/percpu.h>

R15	=	0x00
R14	=	0x08
R13	=	0x10
R12	=	0x18
R11	=	0x20
R10	=	0x28
R9	=	0x30
R8	=	0x38
RBX	=	0x40
RCX	=	0x48
RDX	=	0x50
RSI	=	0x58
RDI	=	0x60
RBP	=	0x68
DS	=	0x70
ES	=	0x78
RAX	=	0x80
FUNC	=	0x88
ERRCODE	=	0x90
RIP	=	0x98
CS	=	0xa0
RFLAGS	=	0xa8
OLDRSP	=	0xb0
OLDSS	=	0xb8

.global ARCH_CONTEXT_RESTORE_FRAME
ARCH_CONTEXT_RESTORE_FRAME:
.global x86_intr_restore_only
x86_intr_restore_only:
RESTORE_ALL:
	popq	%r15;		
	popq	%r14;		
	popq	%r13;		
	popq	%r12;		
	popq	%r11;		
	popq	%r10;		
	popq	%r9;		
	popq	%r8;		
	popq	%rbx;		
	popq	%rcx;		
	popq	%rdx;		
	popq	%rsi;		
	popq	%rdi;		
	popq	%rbp;		
	popq	%rax;		
	movq	%rax,	%ds;	
	popq	%rax;		
	movq	%rax,	%es;	
	popq	%rax;		
	addq	$0x10,	%rsp;	
	iretq;

#define GET_CURRENT(reg)	\
	movq	$-32768,reg;	\
	andq	%rsp,	reg

ret_from_exception:
	// Exception handlers on IST stacks must NOT go through
	// ret_from_intr: softirq / schedule() use get_current_task()
	// (RSP masking) which returns garbage on the IST stack.
	// User faults are redirected to do_exit via the modified
	// iretq frame; kernel faults never return.  Any pending
	// softirqs or resched will be picked up by the very next
	// interrupt return (which runs on the task kernel stack).
	jmp	RESTORE_ALL
ENTRY(ret_from_intr)
#ifdef OS01_SELFTEST
	# Frame already owns all GPRs. Count return-work entry per CPU.
	movl	%gs:PERCPU_CPU_ID_OFFSET,	%eax
	leaq	selftest_x86_intr_returns(%rip),	%rdx
	incq	(%rdx,%rax,8)
#elif defined(X86_INTR_FAULT_TEST_TRACE_READ)
	leaq	x86_trace_read_intr_returns(%rip),	%rdx
	incq	(%rdx)
#endif
	cld
	movq	%rsp,	%rbx
	movq	%rbx,	%rdi
	movl	$ARCH_CONTEXT_RETURN_FULL,	%esi
	andq	$-16,	%rsp
	call	context_run_return_work
#ifdef X86_BOOT_IRQ_TIME_TEST
/* BOOT-2 test tail: keep %rbx pointing at the saved frame across
 * boot2_test_irq_return, then restore %rsp and jump to the native
 * frame restore facade. */
boot2_test_restore:
	movq	%rbx,	%rdi
	call	boot2_test_irq_return
#endif
	movq	%rbx,	%rsp
	jmp	ARCH_CONTEXT_RESTORE_FRAME

ENTRY(divide_error)
	pushq	$0
	pushq	%rax
	leaq	do_divide_error(%rip),	%rax
	xchgq	%rax,	(%rsp)

error_code:
	pushq	%rax
	movq	%es,	%rax
	pushq	%rax
	movq	%ds,	%rax
	pushq	%rax
	xorq	%rax,	%rax

	pushq	%rbp
	pushq	%rdi
	pushq	%rsi
	pushq	%rdx
	pushq	%rcx
	pushq	%rbx
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15	
	
	cld
	movq	ERRCODE(%rsp),	%rsi
	movq	FUNC(%rsp),	%rdx	

	movq	$0x10,	%rdi
	movq	%rdi,	%ds
	movq	%rdi,	%es

	movq	%rsp,	%rdi
	////GET_CURRENT(%ebx)

	callq 	*%rdx

	# Only #PF and syscall enter this frame on a task stack. IST faults
	# (including recoverable MSR #GP) must defer all task return work.
	# Unwind read fixup (#PF returning 1) restores directly without running
	# ret_from_intr return work.
	leaq	do_page_fault(%rip), %rcx
	cmpq	%rcx, FUNC(%rsp)
	jne	1f
	cmpl	$1, %eax
	je	ret_from_exception
	jmp	ret_from_intr
1:
	leaq	do_system_call(%rip), %rcx
	cmpq	%rcx, FUNC(%rsp)
	je	ret_from_intr
	jmp	ret_from_exception

ENTRY(debug)
	pushq	$0
	pushq	%rax
	leaq	do_debug(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(nmi)
	pushq	%rax
	cld;			
	pushq	%rax;	
	
	pushq	%rax
	movq	%es,	%rax
	pushq	%rax
	movq	%ds,	%rax
	pushq	%rax
	xorq	%rax,	%rax
	
	pushq	%rbp;		
	pushq	%rdi;		
	pushq	%rsi;		
	pushq	%rdx;		
	pushq	%rcx;		
	pushq	%rbx;		
	pushq	%r8;		
	pushq	%r9;		
	pushq	%r10;		
	pushq	%r11;		
	pushq	%r12;		
	pushq	%r13;		
	pushq	%r14;		
	pushq	%r15;
	
	movq	$0x10,	%rdx;	
	movq	%rdx,	%ds;	
	movq	%rdx,	%es;
	
	movq	$0,	%rsi
	movq	%rsp,	%rdi

	callq	do_nmi

	jmp	RESTORE_ALL

ENTRY(int3)
	pushq	$0
	pushq	%rax
	leaq	do_int3(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(overflow)
	pushq	$0
	pushq	%rax
	leaq	do_overflow(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(bounds)
	pushq	$0
	pushq	%rax
	leaq	do_bounds(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(undefined_opcode)
	pushq	$0
	pushq	%rax
	leaq	do_undefined_opcode(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(dev_not_available)	/* need rewrite */
	pushq	$0
	pushq	%rax
	leaq	do_dev_not_available(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code


ENTRY(double_fault)
	pushq	%rax
	leaq	do_double_fault(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(coprocessor_segment_overrun)
	pushq	$0
	pushq	%rax
	leaq	do_coprocessor_segment_overrun(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(invalid_TSS)
	pushq	%rax
	leaq	do_invalid_TSS(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(segment_not_present)
	pushq	%rax
	leaq	do_segment_not_present(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(stack_segment_fault)
	pushq	%rax
	leaq	do_stack_segment_fault(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(general_protection)
	pushq	%rax
	leaq	do_general_protection(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(page_fault)
	pushq	%rax
	leaq	do_page_fault(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(x87_FPU_error)
	pushq	$0
	pushq	%rax
	leaq	do_x87_FPU_error(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(alignment_check)
	pushq	%rax
	leaq	do_alignment_check(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(machine_check)
	pushq	$0
	pushq	%rax
	leaq	do_machine_check(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(SIMD_exception)
	pushq	$0
	pushq	%rax
	leaq	do_SIMD_exception(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

ENTRY(virtualization_exception)
	pushq	$0
	pushq	%rax
	leaq	do_virtualization_exception(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

// int $0x80 syscall entry (user-callable, DPL=3)
ENTRY(system_call)
	pushq	$0
	pushq	%rax
	leaq	do_system_call(%rip),	%rax
	xchgq	%rax,	(%rsp)
	jmp	error_code

// Resume path for the idle task when switched back to.
// The first switch_to inside task_init() saved RSP after
// pushq rbp/rax.  We undo those, restore task_init's frame
// pointer, and then leave+ret to unwind back to kernel_main.
ENTRY(idle_resume)
	sti               // re-enable IRQs (disabled by switch_to's cli)
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp      // restore task_init's frame pointer
	leave             // mov rsp, rbp; pop rbp — unwinds task_init's frame
	ret               // return to kernel_main → while(1) hlt()

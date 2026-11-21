#include <arch/x86_64/linkage.h>
#include <arch/x86_64/regs.h>

.section .text.head, "ax"

ENTRY(_start)

    /* BOOT-2: the FIRST executed instruction is cli — the boot must not
     * depend on UEFI-legacy IF state.  (RCX/RAX protocol reads follow.) */
    cli
    // Bootloader may use MS ABI (RCX) or SysV ABI (RDI)
    // Prefer RCX (clang-built EFI), fallback to RDI (gcc-built EFI)
    movq    %rcx,   %rax
    testq   %rax,   %rax
    jnz     1f
    movq    %rdi,   %rax
1:
    movq    %rax,   BOOT_CONTEXT(%rip)
	mov	%ss,	%ax
	mov	%ax,	%ds
	mov	%ax,	%es
	mov	%ax,	%fs
	mov	%ax,	%ss
	mov	$0x7e00,	%esp

//=======	load	cr3

	movq	$0x101000, %rax
	movq	%rax,	%cr3

//=======	load GDTR

	lgdt	GDT_POINTER(%rip)

//=======	load	IDTR

	lidt	IDT_POINTER(%rip)

	mov $X86_KERNEL_DS,	%ax
	mov %ax,	%ds
	mov %ax,	%es
	mov %ax,	%fs
	mov %ax,	%gs
	mov %ax,	%ss

	movq    $0xffff800000007e00,	%rsp

//======    switch seg to match vma

	movq	switch_seg(%rip),	%rax
	pushq	$X86_KERNEL_CS
	pushq	%rax
	lretq

//=======	64-bit mode code

switch_seg:
	.quad	entry64

entry64:
	movq	$X86_KERNEL_DS,	%rax
	movq	%rax,	%ds
	movq	%rax,	%es
	movq	%rax,	%gs
	movq	%rax,	%ss
	movq	_stack_start(%rip),	%rsp		/* rsp address */

	/* Enable SSE/SSE2 for user-space (x86-64 ABI requires this) */
	movq	%cr4,	%rax
	orq	$(CR4_OSFXSR|CR4_OSXMMEXCPT),	%rax		/* OSFXSR | OSXMMEXCPT */
	movq	%rax,	%cr4

setup_IDT:
	leaq	ignore_int(%rip),	%rdx
	movq	$(X86_KERNEL_CS << 16),	%rax
	movw	%dx,	%ax
	movq	$(0x8e00 << 32),	%rcx		
	addq	%rcx,	%rax
	movl	%edx,	%ecx
	shrl	$16,	%ecx
	shlq	$48,	%rcx
	addq	%rcx,	%rax
	shrq	$32,	%rdx
	leaq	IDT_Table(%rip),	%rdi
	mov	$256,	%rcx
rp_sidt:
	movq	%rax,	(%rdi)
	movq	%rdx,	8(%rdi)
	addq	$0x10,	%rdi
	dec	%rcx
	jne	rp_sidt

setup_TSS64:
	leaq	TSS64_Table(%rip),	%rdx
	xorq	%rax,	%rax
	xorq	%rcx,	%rcx
	movq	$0x89,	%rax
	shlq	$40,	%rax
	movl	%edx,	%ecx
	shrl	$24,	%ecx
	shlq	$56,	%rcx
	addq	%rcx,	%rax
	xorq	%rcx,	%rcx
	movl	%edx,	%ecx
	andl	$0xffffff,	%ecx
	shlq	$16,	%rcx
	addq	%rcx,	%rax
	addq	$103,	%rax
	leaq	GDT_Table(%rip),	%rdi
	movq	%rax,	64(%rdi)	//tss segment offset
	shrq	$32,	%rdx
	movq	%rdx,	72(%rdi)	//tss+1 segment offset

//=======	call kernel_main

	movq    BOOT_CONTEXT(%rip),    %rax
    movq    %rax,   %rdi
	movq    go_to_kernel(%rip),     %rax
    pushq   $X86_KERNEL_CS
    pushq   %rax
    lretq

go_to_kernel:
#if defined(OS01_KASAN) || defined(OS01_UBSAN)
	/* Sanitizer builds enter through the uninstrumented kasan_boot_entry:
	 * under OS01_KASAN it copies the low-identity-map UEFI handoff struct
	 * into the kernel image before ANY instrumented C runs (shadow of
	 * low-half VAs is non-canonical), and under both knobs it pre-seeds
	 * the BSP GS base so per-CPU reads see a canonical high address
	 * instead of the raw identity map (see kernel/kasan/kasan_boot.c). */
	.quad	kasan_boot_entry
#else
	.quad	kernel_main
#endif

//=======	ignore_int

ignore_int:
	cld
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rbp
	pushq	%rdi
	pushq	%rsi

	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%es,	%rax
	pushq	%rax
	movq	%ds,	%rax
	pushq	%rax

	movq	$X86_KERNEL_DS,	%rax
	movq	%rax,	%ds
	movq	%rax,	%es

	leaq	int_msg(%rip),	%rax			/* leaq get address */
	pushq	%rax
	movq	%rax,	%rdx
	movq	$0x00000000,	%rsi
	movq	$0x00ff0000,	%rdi
	movq	$0,	%rax
	callq	color_printk
	addq	$0x8,	%rsp

Loop:
	hlt
	jmp	Loop

int_msg:
	.asciz "Unknown interrupt or fault at RIP\n"

ENTRY(_stack_start)
	.quad   init_task_union + 32768

//=======	init page
.align 8

.org	0x1000

__PML4E:

	.quad	0x102003
	.fill	255,8,0
	.quad	0x102003
#if defined(OS01_KASAN)
	/* KASan phase-1 shadow window: PML4 slot 472 -> static PDPT at
	 * 0x104000 (see the __KASAN_* tables below).  Slot 472 covers the
	 * shadow VAs [0xffffec0000000000, +512G) computed as
	 * (VA >> 3) + 0xdffffc0000000000 for the kernel direct-map band.
	 * The two static PDs map 1 GiB of shadow VAs to ONE reserved 2 MiB
	 * zero frame (PA 0x200000, inside the kernel image), read-only:
	 * every shadow check reads 0 = "valid" until the kernel swaps in
	 * real shadow (kasan_shadow_install).  Normal builds keep the
	 * original layout byte-for-byte. */
	.fill	215,8,0
	.quad	0x104003
	.fill	39,8,0
#else
	.fill	255,8,0
#endif

.org	0x2000

__PDPTE:

	.quad	0x103003	/* 0x103003 */
	.fill	511,8,0

.org	0x3000

__PDE:

	.quad	0x000083
	.quad	0x200083
	.quad	0x400083
	.quad	0x600083
	.quad	0x800083		/* 0x800083 */
	.quad	0xa00083
	.quad	0xc00083
	.quad	0xe00083
	.quad	0x1000083
	.quad	0x1200083
	.quad	0x1400083
	.quad	0x1600083
	.quad	0x1800083
	.quad	0x1a00083
	.quad	0x1c00083
	.quad	0x1e00083

	.fill	496,8,0

#if defined(OS01_KASAN)
/* ── KASan phase-1 shadow tables (instrumented builds only) ──
 * .text.head is linked at PA 0x100000 (2 MiB aligned), so these .org
 * offsets resolve to the PAs shown in the comments.  The zero frame is
 * PROGBITS zeros (2 MiB of the raw kernel binary) and sits inside the
 * kernel image range [ _text, _end ] that pmm_init reserves, so the PMM
 * never hands it out. */
.org	0x4000

__KASAN_PDPT:
	.quad	0x105003			/* PA 0x105000: shadow PD 0 */
	.quad	0x106003			/* PA 0x106000: shadow PD 1 */
	.fill	510,8,0

.org	0x5000

__KASAN_PD0:
	/* 512 × 2 MiB pages, all → the shared zero frame; P|RO|huge. */
	.fill	512,8,0x200081

.org	0x6000

__KASAN_PD1:
	.fill	512,8,0x200081

/* Zero frame: PA 0x200000..0x3fffff (2 MiB aligned). */
.org	0x100000
__KASAN_ZERO_FRAME:
	.fill	0x200000,1,0
#endif

//=======	GDT_Table

.section .data

.globl GDT_Table

GDT_Table:
	.quad	0x0000000000000000			/*0	NULL descriptor		       	00*/
	.quad	0x0020980000000000			/*1	KERNEL	Code	64-bit	Segment	08*/
	.quad	0x0000920000000000			/*2	KERNEL	Data	64-bit	Segment	10*/
	.quad	0x0000000000000000			/*3	USER	Code	32-bit	Segment 18*/
	.quad	0x0000000000000000			/*4	USER	Data	32-bit	Segment 20*/
	.quad	0x0020f80000000000			/*5	USER	Code	64-bit	Segment	28*/
	.quad	0x0000f20000000000			/*6	USER	Data	64-bit	Segment	30*/
	.quad	0x00cf9a000000ffff			/*7	KERNEL	Code	32-bit	Segment	38*/
	.quad	0x00cf92000000ffff			/*8	KERNEL	Data	32-bit	Segment	40*/
	.fill	100,8,0					/* TSS descriptors: slots 8-9 overwritten by setup_TSS64; remaining slots for per-CPU TSS at SMP boot */
GDT_END:

GDT_POINTER:
GDT_LIMIT:	.word	GDT_END - GDT_Table - 1
GDT_BASE:	.quad	GDT_Table

//=======	IDT_Table

.globl IDT_Table

IDT_Table:
	.fill  512,8,0
IDT_END:

IDT_POINTER:
IDT_LIMIT:	.word	IDT_END - IDT_Table - 1
IDT_BASE:	.quad	IDT_Table

//=======	BOOT_CONTEXT

.globl BOOT_CONTEXT

BOOT_CONTEXT:
    .quad 0xffffffffffffffff

//=======	TSS64_Table

.globl	TSS64_Table

TSS64_Table:
	.fill  13,8,0
TSS64_END:


TSS64_POINTER:
TSS64_LIMIT:	.word	TSS64_END - TSS64_Table - 1
TSS64_BASE:	.quad	TSS64_Table
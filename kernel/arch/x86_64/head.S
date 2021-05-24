#include "linkage.h"

.section .text

ENTRY(_start)

    movq    %rdi,   %rax
    movq    %rax,   BOOT_INFO(%rip)
	mov	%ss,	%ax
	mov	%ax,	%ds
	mov	%ax,	%es
	mov	%ax,	%fs
	mov	%ax,	%ss
	mov	$0x7e00,	%esp

//=======	load GDTR

	lgdt	GDT_POINTER(%rip)

//=======	load	IDTR

	lidt	IDT_POINTER(%rip)

	mov $0x10,	%ax
	mov %ax,	%ds
	mov %ax,	%es
	mov %ax,	%fs
	mov %ax,	%gs
	mov %ax,	%ss

	movq    $0x7e00,	%rsp

//=======	load	cr3

	movq	switch_seg(%rip),	%rax
	pushq	$0x08
	pushq	%rax
	lretq

//=======	64-bit mode code

switch_seg:
	.quad	entry64

entry64:
	movq	$0x10,	%rax
	movq	%rax,	%ds
	movq	%rax,	%es
	movq	%rax,	%gs
	movq	%rax,	%ss
	movq	$0x7e00,	%rsp		/* rsp address */

    movq    BOOT_INFO(%rip),    %rax
    movq    %rax,   %rdi
	movq    go_to_kernel(%rip),     %rax
    pushq   $0x08
    pushq   %rax
    lretq

go_to_kernel:
	.quad	kernel_main

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
	.fill	100,8,0					/*10 ~ 11 TSS (jmp one segment <9>) in long-mode 128-bit 50*/
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

.globl BOOT_INFO

BOOT_INFO:
    .quad 0xffffffffffffffff

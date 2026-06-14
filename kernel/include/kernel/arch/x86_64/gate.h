#ifndef _KERNEL_ARCH_GATE_H
#define _KERNEL_ARCH_GATE_H

#include <stdint.h>
#include <kernel/arch/x86_64/regs.h>

struct desc_struct
{
	unsigned char x[8];
};

struct gate_struct
{
	unsigned char x[16];
};

extern struct desc_struct GDT_Table[];
extern struct gate_struct IDT_Table[];
extern unsigned int TSS64_Table[26];

/*
*/

#define _set_gate(gate_selector_addr,attr,ist,code_addr)	\
do								\
{	unsigned long __d0,__d1;				\
	__asm__ __volatile__	(	"movw	%%dx,	%%ax	\n\t"	\
					"andq	$0x7,	%%rcx	\n\t"	\
					"addq	%4,	%%rcx	\n\t"	\
					"shlq	$32,	%%rcx	\n\t"	\
					"addq	%%rcx,	%%rax	\n\t"	\
					"xorq	%%rcx,	%%rcx	\n\t"	\
					"movl	%%edx,	%%ecx	\n\t"	\
					"shrq	$16,	%%rcx	\n\t"	\
					"shlq	$48,	%%rcx	\n\t"	\
					"addq	%%rcx,	%%rax	\n\t"	\
					"movq	%%rax,	%0	\n\t"	\
					"shrq	$32,	%%rdx	\n\t"	\
					"movq	%%rdx,	%1	\n\t"	\
					:"=m"(*((unsigned long *)(gate_selector_addr)))	,					\
					 "=m"(*(1 + (unsigned long *)(gate_selector_addr))),"=&a"(__d0),"=&d"(__d1)		\
					:"i"(attr << 8),									\
					 "3"((unsigned long *)(code_addr)),"2"(0x8 << 16),"c"(ist)				\
					:"memory"		\
				);				\
}while(0)

/*
*/

inline void __attribute__((always_inline)) set_tss_descriptor(unsigned int n,void * addr)
{
	unsigned long limit = 103;
	*(unsigned long *)(GDT_Table + n) = (limit & 0xffff) | (((unsigned long)addr & 0xffff) << 16) | (((unsigned long)addr >> 16 & 0xff) << 32) | ((unsigned long)0x89 << 40) | ((limit >> 16 & 0xf) << 48) | (((unsigned long)addr >> 24 & 0xff) << 56);	/////89 is attribute
	*(unsigned long *)(GDT_Table + n + 1) = ((unsigned long)addr >> 32 & 0xffffffff) | 0;
}

/*
*/

#define load_TR(n) 							\
do{									\
	__asm__ __volatile__(	"ltr	%%ax"				\
				:					\
				:"a"(n << 3)				\
				:"memory");				\
}while(0)

// ──────────────────────────────────────────────
//  IDT gate setter (low-level)
//
//  WARNING: _set_intr_gate_raw puts `addr` directly
//  into the IDT descriptor.  When the CPU delivers
//  the interrupt it pushes only RIP, CS, RFLAGS
//  (and SS/RSP on ring change).  A bare C function
//  returns via `ret` which pops only RIP — CS and
//  RFLAGS leak on the stack.
//
//  THE ADDRESS MUST BE AN ASSEMBLY STUB that:
//    1. Saves all registers (pt_regs_t layout)
//    2. Calls the C handler
//    3. Returns via ret_from_intr → RESTORE_ALL → iretq
//
//  For C handlers, use REGISTER_INTR_HANDLER below.
// ──────────────────────────────────────────────

inline void __attribute__((always_inline)) set_intr_gate_raw(unsigned int n,unsigned char ist,void * addr)
{
	_set_gate(IDT_Table + n , 0x8E , ist , addr);	//P,DPL=0,TYPE=E
}

/*
*/

inline void __attribute__((always_inline)) set_trap_gate(unsigned int n,unsigned char ist,void * addr)
{
	_set_gate(IDT_Table + n , 0x8F , ist , addr);	//P,DPL=0,TYPE=F
}

/*
*/

inline void __attribute__((always_inline)) set_system_gate(unsigned int n,unsigned char ist,void * addr)
{
	_set_gate(IDT_Table + n , 0xEF , ist , addr);	//P,DPL=3,TYPE=F
}

/*
*/

inline void __attribute__((always_inline)) set_system_intr_gate(unsigned int n,unsigned char ist,void * addr)	//int3
{
	_set_gate(IDT_Table + n , 0xEE , ist , addr);	//P,DPL=3,TYPE=E
}


// ──────────────────────────────────────────────
//  Safe interrupt handler registration
//
//  Two-step pattern:
//
//    [file scope]  DEFINE_INTR_STUB(name, vector)
//    [runtime]     REGISTER_INTR_HANDLER(name, vector, handler)
//
//  DEFINE_INTR_STUB generates the assembly trampoline
//  at file scope — it must NOT appear inside a function,
//  or the stub code will be executed inline.
//
//  REGISTER_INTR_HANDLER stores the C handler in the
//  dispatch table and installs the IDT gate.  Call this
//  at runtime (typically during init).
//
//  Usage:
//    DEFINE_INTR_STUB(tlb,    0x40);
//    DEFINE_INTR_STUB(resched,0x41);
//
//    void ipi_init(void) {
//        REGISTER_INTR_HANDLER(tlb,    0x40, ipi_tlb_handler);
//        REGISTER_INTR_HANDLER(resched,0x41, ipi_resched_handler);
//    }
// ──────────────────────────────────────────────

// C handler signature: void fn(uint64_t nr, uint64_t param, pt_regs_t *regs)
typedef void (*intr_handler_fn)(uint64_t nr, uint64_t parameter,
                                pt_regs_t *regs);

// Per-vector handler table.  generic_intr_dispatch (in intr/dispatch.c)
// indexes this table by vector number.
extern intr_handler_fn intr_handler_table[256];
extern void *        intr_handler_param[256];

void generic_intr_dispatch(pt_regs_t *regs, uint64_t vector);

static inline void _intr_table_set(uint8_t vector, intr_handler_fn handler,
                                   void *param)
{
    intr_handler_table[vector] = handler;
    intr_handler_param[vector]  = param;
}

// ── Register-save sequence used by interrupt stubs ────────
#define INTR_SAVE_ALL                                       \
    "cld;                   \n\t"                           \
    "pushq  %rax;           \n\t"                           \
    "pushq  %rax;           \n\t"                           \
    "movq   %es,    %rax;   \n\t"                           \
    "pushq  %rax;           \n\t"                           \
    "movq   %ds,    %rax;   \n\t"                           \
    "pushq  %rax;           \n\t"                           \
    "xorq   %rax,   %rax;   \n\t"                           \
    "pushq  %rbp;           \n\t"                           \
    "pushq  %rdi;           \n\t"                           \
    "pushq  %rsi;           \n\t"                           \
    "pushq  %rdx;           \n\t"                           \
    "pushq  %rcx;           \n\t"                           \
    "pushq  %rbx;           \n\t"                           \
    "pushq  %r8;            \n\t"                           \
    "pushq  %r9;            \n\t"                           \
    "pushq  %r10;           \n\t"                           \
    "pushq  %r11;           \n\t"                           \
    "pushq  %r12;           \n\t"                           \
    "pushq  %r13;           \n\t"                           \
    "pushq  %r14;           \n\t"                           \
    "pushq  %r15;           \n\t"                           \
    "movq   $0x10,  %rdx;   \n\t"                           \
    "movq   %rdx,   %ds;    \n\t"                           \
    "movq   %rdx,   %es;    \n\t"

// ── File-scope: generate the assembly stub ────────────────
// MUST be used at file scope (outside any function).
#define DEFINE_INTR_STUB(name, vector)                          \
    __asm__(                                                    \
        ".globl _intr_stub_" #name "\n\t"                       \
        "_intr_stub_" #name ":\n\t"                             \
        "pushq  $0\n\t"                /* dummy error code */   \
        INTR_SAVE_ALL                                           \
        "movq   %rsp,   %rdi\n\t"      /* pt_regs* */           \
        "movq   $" #vector ", %rsi\n\t" /* vector number */     \
        "leaq   ret_from_intr(%rip), %rax\n\t"                  \
        "pushq  %rax\n\t"              /* return address */     \
        "jmp    generic_intr_dispatch\n\t"                      \
    )

// ── Runtime: register a C handler for an interrupt vector ──
// Call from init code AFTER the matching DEFINE_INTR_STUB.
#define REGISTER_INTR_HANDLER(name, vector, handler_fn)         \
    do {                                                        \
        extern void _intr_stub_##name(void);                    \
        _intr_table_set((vector), (intr_handler_fn)(handler_fn), NULL); \
        set_intr_gate_raw((vector), 0, _intr_stub_##name);      \
    } while(0)


/*
*/

inline void __attribute__((always_inline)) set_tss64(unsigned long rsp0,unsigned long rsp1,unsigned long rsp2,unsigned long ist1,unsigned long ist2,unsigned long ist3,
unsigned long ist4,unsigned long ist5,unsigned long ist6,unsigned long ist7)
{
	*(unsigned long *)(TSS64_Table+1) = rsp0;
	*(unsigned long *)(TSS64_Table+3) = rsp1;
	*(unsigned long *)(TSS64_Table+5) = rsp2;

	*(unsigned long *)(TSS64_Table+9) = ist1;
	*(unsigned long *)(TSS64_Table+11) = ist2;
	*(unsigned long *)(TSS64_Table+13) = ist3;
	*(unsigned long *)(TSS64_Table+15) = ist4;
	*(unsigned long *)(TSS64_Table+17) = ist5;
	*(unsigned long *)(TSS64_Table+19) = ist6;
	*(unsigned long *)(TSS64_Table+21) = ist7;
}

#endif

#ifndef _KERNEL_ARCH_TRAP_H

#define _KERNEL_ARCH_TRAP_H

#include <stdint.h>
#include <kernel/arch/x86_64/regs.h>

void do_signal_delivery(pt_regs_t *regs);
int  signal_pending_fatal(void);   // non-zero if a fatal signal is pending

void divide_error();
void debug();
void nmi();
void int3();
void overflow();
void bounds();
void undefined_opcode();
void dev_not_available();
void double_fault();
void coprocessor_segment_overrun();
void invalid_TSS();
void segment_not_present();
void stack_segment_fault();
void general_protection();
void page_fault();
void x87_FPU_error();
void alignment_check();
void machine_check();
void SIMD_exception();
void virtualization_exception();

// int 0x80 syscall
void system_call();
void do_system_call(pt_regs_t *regs, uint64_t error_code);

void sys_vector_install();

#endif
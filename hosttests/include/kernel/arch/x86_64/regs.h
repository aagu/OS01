#ifndef _KERNEL_ARCH_REGS_H
#define _KERNEL_ARCH_REGS_H

// ─────────────────────────────────────────────────────────
//  x86_64 CPU Register Definitions — single source of truth
//
//  Section 1 (#ifndef __ASSEMBLER__):  C-only  (structs, typedefs)
//  Section 2 (unconditionally):       shared  (#define, usable in .S)
// ─────────────────────────────────────────────────────────

#ifndef __ASSEMBLER__
#include <stdint.h>

// ── pt_regs_t: exception/interrupt stack frame ──────────
// Layout pushed by INTR_SAVE_ALL / SAVE_ALL assembly stubs.
// Must match the push order in gate.h:INTR_SAVE_ALL.

typedef struct pt_regs
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t ds;
    uint64_t es;
    uint64_t rax;
    uint64_t func;
    uint64_t errcode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} pt_regs_t;

#endif /* !__ASSEMBLER__ */

// ─────────────────────────────────────────────────────────
//  Register bit definitions (usable in both C and assembly)
// ─────────────────────────────────────────────────────────

// ── CR0 (Control Register 0) ────────────────────────────

#define CR0_PE             (1 << 0)    // Protection Enable
#define CR0_MP             (1 << 1)    // Monitor Coprocessor
#define CR0_EM             (1 << 2)    // Emulation
#define CR0_TS             (1 << 3)    // Task Switched
#define CR0_ET             (1 << 4)    // Extension Type
#define CR0_NE             (1 << 5)    // Numeric Error
#define CR0_WP             (1 << 16)   // Write Protect
#define CR0_AM             (1 << 18)   // Alignment Mask
#define CR0_NW             (1 << 29)   // Not Write-through
#define CR0_CD             (1 << 30)   // Cache Disable
#define CR0_PG             (1 << 31)   // Paging

// ── CR4 (Control Register 4) ────────────────────────────

#define CR4_VME            (1 << 0)    // Virtual 8086 Mode Extensions
#define CR4_PVI            (1 << 1)    // Protected-Mode Virtual Interrupts
#define CR4_TSD            (1 << 2)    // Time Stamp Disable
#define CR4_DE             (1 << 3)    // Debugging Extensions
#define CR4_PSE            (1 << 4)    // Page Size Extensions
#define CR4_PAE            (1 << 5)    // Physical Address Extension
#define CR4_MCE            (1 << 6)    // Machine-Check Enable
#define CR4_PGE            (1 << 7)    // Page Global Enable
#define CR4_PCE            (1 << 8)    // Perf-Monitoring Counter Enable
#define CR4_OSFXSR         (1 << 9)    // OS FXSAVE/FXRSTOR Support
#define CR4_OSXMMEXCPT     (1 << 10)   // OS Unmasked Exception Support (SSE)
#define CR4_UMIP           (1 << 11)   // User-Mode Instruction Prevention
#define CR4_LA57           (1 << 12)   // 57-bit Linear Addresses
#define CR4_VMXE           (1 << 13)   // VMX-Enable
#define CR4_SMXE           (1 << 14)   // SMX-Enable
#define CR4_FSGSBASE       (1 << 16)   // FSGSBASE Enable
#define CR4_PCIDE          (1 << 17)   // PCID Enable
#define CR4_OSXSAVE        (1 << 18)   // XSAVE Enable
#define CR4_SMEP           (1 << 20)   // Supervisor Mode Execution Protection
#define CR4_SMAP           (1 << 21)   // Supervisor Mode Access Protection
#define CR4_PKE            (1 << 22)   // Protection Key Enable

// ── EFER (Extended Feature Enable Register, MSR 0xC0000080) ─

#define EFER_SYSCALL_ENABLE (1 << 0)   // SYSCALL Enable
#define EFER_LME            (1 << 8)   // Long Mode Enable
#define EFER_LMA            (1 << 10)  // Long Mode Active
#define EFER_NXE            (1 << 11)  // No-Execute Enable
#define EFER_SVME           (1 << 12)  // Secure Virtual Machine Enable

// ── RFLAGS (Status/Control Flags) ───────────────────────

#define RFLAGS_CF           (1UL << 0)   // Carry Flag
#define RFLAGS_PF           (1UL << 2)   // Parity Flag
#define RFLAGS_AF           (1UL << 4)   // Auxiliary Carry Flag
#define RFLAGS_ZF           (1UL << 6)   // Zero Flag
#define RFLAGS_SF           (1UL << 7)   // Sign Flag
#define RFLAGS_TF           (1UL << 8)   // Trap Flag
#define RFLAGS_IF           (1UL << 9)   // Interrupt Enable Flag
#define RFLAGS_DF           (1UL << 10)  // Direction Flag
#define RFLAGS_OF           (1UL << 11)  // Overflow Flag
#define RFLAGS_IOPL_SHIFT   12
#define RFLAGS_IOPL_MASK    (3UL << RFLAGS_IOPL_SHIFT)
#define RFLAGS_NT           (1UL << 14)  // Nested Task
#define RFLAGS_RF           (1UL << 16)  // Resume Flag
#define RFLAGS_VM           (1UL << 17)  // Virtual-8086 Mode
#define RFLAGS_AC           (1UL << 18)  // Alignment Check
#define RFLAGS_VIF          (1UL << 19)  // Virtual Interrupt Flag
#define RFLAGS_VIP          (1UL << 20)  // Virtual Interrupt Pending
#define RFLAGS_ID           (1UL << 21)  // CPUID Available

// ── Segment Selectors (GDT indices) ─────────────────────

#define X86_KERNEL_CS       0x08
#define X86_KERNEL_DS       0x10
#define X86_USER_CS         0x2B        // GDT index 5 | RPL=3
#define X86_USER_DS         0x33        // GDT index 6 | RPL=3

// ── MSR (Model-Specific Register) Numbers ───────────────

#define IA32_EFER           0xC0000080
#define IA32_FS_BASE        0xC0000100
#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_APIC_BASE      0x1B
#define IA32_TSC_ADJUST     0x3B

// IA32_APIC_BASE bit definitions
#define APIC_BASE_BSP         (1UL << 8)   // Bootstrap Processor
#define APIC_BASE_ENABLE      (1UL << 11)  // APIC Global Enable (xAPIC)
#define APIC_BASE_X2APIC      (1UL << 10)  // x2APIC mode enable
#define APIC_BASE_ADDR_MASK   0xFFFFFFFFFFFFF000ULL

// ── CPUID Feature Bits (Leaf 1) ─────────────────────────

// EDX bits
#define CPUID_FEAT_EDX_APIC    (1 << 9)    // APIC supported on chip

// ECX bits
#define CPUID_FEAT_ECX_X2APIC  (1 << 21)   // x2APIC supported

#endif /* _KERNEL_ARCH_REGS_H */

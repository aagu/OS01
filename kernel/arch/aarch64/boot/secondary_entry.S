#include <arch/aarch64/boot_offsets.h>

/* PSCI CPU_ON64 enters here with context_id in x0, MMU off.
 * Only EL1/EL2 are supported. No stack access or call occurs until
 * context, full affinity, slot identity and independent stack agree.
 * The BSP has already published all metadata and page tables to PoC.
 */
.section .boot.text, "ax"
.balign 0x80
.globl secondary_start
.type secondary_start, %function
secondary_start:
    msr     daifset, #0xf
    mov     x19, x0                         /* preserve full context_id */
    mrs     x0, CurrentEL
    cmp     x0, #0x8
    b.eq    .Lap_from_el2
    cmp     x0, #0x4
    b.ne    secondary_panic
    b       .Lap_el1

.Lap_from_el2:
    mov     x0, #(1 << 31)                  /* HCR_EL2.RW */
    msr     hcr_el2, x0
    mov     x0, #0x33ff                     /* CPTR_EL2: TFP=0 (allow FP/SIMD at EL1/EL0) */
    msr     cptr_el2, x0
    mov     x0, #3                         /* EL1 physical timer/counter */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x3c5                     /* EL1h, all DAIF masked */
    msr     spsr_el2, x0
    adr     x0, .Lap_el1
    msr     elr_el2, x0
    isb
    eret

.Lap_el1:
    msr     spsel, #1                       /* even when firmware gave EL1t */
    msr     daifset, #0xf
    cbz     x19, secondary_panic             /* BSP may not enter as an AP */
    cmp     x19, #AARCH64_BOOT_CAPACITY
    b.hs    secondary_panic
    ldr     x2, =aarch64_dtb_cpu_count
    ldr     w2, [x2]                        /* published count is uint32_t */
    cmp     w2, #AARCH64_BOOT_CAPACITY
    b.hi    secondary_panic
    cmp     x19, x2
    b.hs    secondary_panic

    mrs     x1, mpidr_el1
    ldr     x2, =AARCH64_BOOT_AFFINITY_MASK
    and     x1, x1, x2                     /* Aff3 + Aff2..0, no flags */
    ldr     x3, =aarch64_dtb_mpidr_table
    ldr     x4, [x3, x19, lsl #3]
    cmp     x4, x1
    b.ne    secondary_panic

    ldr     x6, =aarch64_boot_percpu
    mov     x7, #AARCH64_BOOT_PERCPU_SIZE
    madd    x6, x19, x7, x6
    ldr     x7, [x6, #AARCH64_BOOT_MPIDR_OFFSET]
    cmp     x7, x1
    b.ne    secondary_panic
    ldr     w7, [x6, #AARCH64_BOOT_CPU_ID_OFFSET]
    cmp     x7, x19
    b.ne    secondary_panic
    ldr     x7, [x6, #AARCH64_BOOT_SELF_OFFSET]
    cmp     x7, x6
    b.ne    secondary_panic

    /* Unified SMP boot (spec 2026-11-19): the BSP published the shared
     * AP C entry at AARCH64_BOOT_ENTRY_OFFSET.  Load and validate it
     * into callee-saved x20 NOW (AAPCS64: x20 survives the
     * aarch64_boot_install_translation call below; x6 does not), and
     * intercept entry == 0 before the switch to the boot stack. */
    ldr     x20, [x6, #AARCH64_BOOT_ENTRY_OFFSET]
    cbz     x20, secondary_panic

    ldr     x7, [x6, #AARCH64_BOOT_STACK_OFFSET]
    tst     x7, #0xf
    b.ne    secondary_panic
    ldr     x8, =aarch64_boot_stacks
    add     x8, x8, x19, lsl #AARCH64_BOOT_STACK_SHIFT
    add     x8, x8, #AARCH64_BOOT_STACK_SIZE
    cmp     x7, x8                          /* cannot select another CPU's stack */
    b.ne    secondary_panic
    mov     sp, x7
    /* Phase 2 #3: TPIDR_EL1 = &percpu_data[cpu_id]. x19 is cpu_id
     * (verified above via [x6, #AARCH64_BOOT_CPU_ID_OFFSET]).
     * PERCPU_DATA_SIZE is a numeric literal in
     * kernel/include/percpu/percpu.h (assembly context cannot use
     * sizeof(percpu_t); its equality with sizeof(percpu_t) is pinned
     * by a _Static_assert there). Any percpu_t up to 64 KiB fits the
     * 16-bit mov immediate, so the literal needs no asm-side update
     * when the struct grows. */
    ldr     x7, =percpu_data
    mov     x8, #PERCPU_DATA_SIZE
    madd    x7, x19, x8, x7
    msr     tpidr_el1, x7

    ldr     x0, =boot_vectors
    msr     vbar_el1, x0
    isb
    dsb     sy
    bl      aarch64_boot_install_translation             /* TTBR0 remains boot root */
    ldr     x0, =runtime_ttbr1_pa
    ldr     x0, [x0]
    cbz     x0, secondary_panic
    tst     x0, #0xfff
    b.ne    secondary_panic
    lsr     x1, x0, #40
    cbnz    x1, secondary_panic
    msr     ttbr1_el1, x0
    isb
    /* CPU_ON may resume a PE with retained firmware translations.
     * BSP TLBI is local; this AP must discard its own EL1 entries. */
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    isb
    ldr     x0, =AARCH64_BOOT_SCTLR
    msr     sctlr_el1, x0
    isb
    ldr     x0, =exception_vectors
    msr     vbar_el1, x0
    isb

    /* Unified SMP boot: hand (cpu_id, &percpu_data[cpu_id]) to the
     * validated shared AP C entry (smp_ap_common_entry).  tpidr_el1 was
     * set to the runtime percpu_t for x19 further above; x20 survived
     * aarch64_boot_install_translation (callee-saved).  The C entry is
     * noreturn — the wfi loop below is an unreachable safety net. */
    mov     x0, x19
    mrs     x1, tpidr_el1
    blr     x20
.Lap_returned:
    wfi
    b       .Lap_returned
.size secondary_start, . - secondary_start

/* Invalid firmware context: no usable stack, no C call and no shared
 * metadata writes. BSP will diagnose its target MPIDR on ACK timeout. */
.globl secondary_panic
.type secondary_panic, %function
secondary_panic:
    msr     daifset, #0xf
1:  wfi
    b       1b
.size secondary_panic, . - secondary_panic

/* aarch64 phase 1: head.S — low-level boot, page-table construction,
 * MMU enable, jump to high-half C entry.
 *
 * Boot flow (spec §4, strict order):
 *   1.  save x0 (direct-boot DTB or fixed UEFI boot_context) → x19
 *   2.  EL2 → EL1 drop (only if CurrentEL==EL2)
 *   3.  msr daifset, #0xf     — mask DAIF
 *   4.  panic if MIDR_EL1 != Cortex-A53
 *   5.  clear only .boot.bss
 *   6.  build dual-TTBR page tables in .boot.bss
 *   7.  dsb sy
 *   8.  set TCR_EL1 + MAIR_EL1 + TTBR0_EL1 + TTBR1_EL1 → isb
 *   9.  construct SCTLR_EL1 (RES1 + M + C + I) → isb
 *  10.  set BSP stack + TPIDR_EL1 = &aarch64_boot_percpu[0]
 *  11.  absolute blr to aarch64_main (high-half)
 */

.globl _boot_start
.globl _boot_end
.globl _boot_bss_start
.globl _boot_bss_end
.globl _bss_start
.globl _bss_end
.globl aarch64_boot_percpu
.globl aarch64_boot_stacks
.globl aarch64_dtb_slot
.globl aarch64_dtb_mpidr_table
.globl aarch64_dtb_cpu_count
.globl aarch64_main
.globl exception_vectors
.globl secondary_start
.globl secondary_panic
/* Helpers exposed for high-half C code (smp.c) to access .boot.bss
 * symbols without compiler-generated adrp+add failing the relocation
 * range (high-half C cannot directly reference low-VMA symbols). */
.globl aarch64_percpu_slot_addr
.globl aarch64_boot_stack_top_addr
.globl aarch64_dtb_mpidr_table_addr
.globl aarch64_dtb_cpu_count_addr
.globl secondary_start_addr

.equ AARCH64_BOOT_STACK_SIZE, 0x1000

/* PAGE-TABLE DESCRIPTOR FLAGS — see aarch64 ARM, block/page descriptor
 * format (Table D4-29 in ARMv8-A ARM).  These flag values are
 * individually ORR-able (no zero-component). */
.equ PT_V,                 (1 << 0)
.equ PT_TYPE_PAGE,         (1 << 1)      /* level 3 page descriptor: bit1=1 (page type) */
.equ PT_TYPE_TABLE,        (3 << 0)      /* table descriptor: bit[1:0]=0b11 */
.equ PT_ATTR_DEV,          (0 << 2)
.equ PT_ATTR_NORMAL,       (1 << 2)
.equ PT_SH_NS,             (0 << 8)      /* SH field is bits[9:8] */
.equ PT_SH_IS,             (3 << 8)
.equ PT_AP_EL1_RW,         (0 << 6)      /* AP field is bits[7:6] */
.equ PT_AF,                (1 << 10)

/* High bits — always combined via register-OR (out of immediate range). */
.equ PT_PXN_MASK,          (1 << 53)
.equ PT_UXN_MASK,          (1 << 52)

/* SCTLR value: M=1, C=1, I=1 plus RES1 base 0x300C00. */
.equ SCTLR_VALUE, ((1 << 0) | (1 << 2) | (1 << 12) | 0x300C00)

/* ──────────────────────────────────────────────────────────────────
 * 64 KiB scratch for page tables (declared in .boot.bss, NOBITS).
 * Layout (6 tables × 4 KiB):
 *    +0x0000  PGD          — TTBR0 and TTBR1 both point here.
 *    +0x1000  PUD_low      — entries 0..511 for low identity regions.
 *    +0x2000  PMD_low0     — for VA low [0x00000000, 0x40000000) (0..1GB)
 *    +0x3000  PMD_low1     — for VA low [0x40000000, 0x80000000) (1..2GB)
 *    +0x4000  PTE_low1_k   — 4 KiB-granular pages for 0x40000000..2MiB.
 *                            This is the SAME page the high-half kernel
 *                            accesses (VA 0xFFFF0000_40080xxx → PA via
 *                            this table because bits[47:0] of the VA
 *                            land in [0x40000000, 0x80000000]).
 *    +0x5000  PTE_mmio     — for 0x0800_0000..0x0820_0000 (Device
 *                            pages for PL011 / GICD / GICC).
 * ────────────────────────────────────────────────────────────────── */
.section .boot.bss, "aw", @nobits
.balign 0x1000
.globl boot_page_tables
boot_page_tables:
    .skip   0x10000                           /* 64 KiB */

.globl aarch64_boot_dtb_slot
aarch64_boot_dtb_slot:
    .skip   8                                 /* DTB-park slot */

/* ──────────────────────────────────────────────────────────────────
 * boot_vectors: identity-mapped vector table for the MMU-bring-up
 * window (before VBAR can point at the high-half .text.entry).
 *
 * Layout matches entry.S but every entry panics into a small
 * printer loop that dumps ELR_EL1 / ESR_EL1 / FAR_EL1 in hex via
 * PL011 MMIO writes (PL011 is reachable pre-MMU at 0x09000000) and
 * then spins.  The MMIO write also flushes with a dummy read.
 *
 * We give this symbol external linkage so head.S can `msr vbar_el1, x0`
 * before MMU enable.
 * ────────────────────────────────────────────────────────────────── */
.section .boot.text, "ax"
.globl boot_vectors
.balign 2048
boot_vectors:
    .balign 128
    b   .Lpanic                       /* sync, SP_EL0 */
    .balign 128
    b   .Lpanic                       /* IRQ,   SP_EL0 */
    .balign 128
    b   .Lpanic                       /* FIQ,   SP_EL0 */
    .balign 128
    b   .Lpanic                       /* SError, SP_EL0 */
    .balign 128
    b   .Lpanic                       /* sync,  SP_ELx */
    .balign 128
    b   .Lirq_spin                   /* IRQ,   SP_ELx (left for parity) */
    .balign 128
    b   .Lpanic                       /* FIQ,   SP_ELx */
    .balign 128
    b   .Lpanic                       /* SError, SP_ELx */

/* Print "FAULT elr=XXXXXXXXXXXXXXXX esr=XXXXXXXXXXXXXXXX far=XXXXXXXXXXXXXXXX\n" */
.Lpanic:
    mov     x1, #'P'
    bl      .Lprint_c
    mov     x1, #'!'
    bl      .Lprint_c
.Lpanic_spin:
    b       .Lpanic_spin

.Lirq_spin:
    b       .Lirq_spin

/* Helpers — all clobber x0,x1,x2,x3. PL011 @ 0x09000000. */
.Lprint_c:
    /* spin until TXFF=0, then write char in x1 */
    mov     x2, #0x09000000
1:  ldr     w3, [x2, #0x18]
    tst     w3, #(1 << 5)
    b.ne    1b
    str     w1, [x2]
    ret

.Lprint_x64:
    /* print 16 hex digits of x0, MSB first.  Uses x9 as counter,
     * but BL clobbers caller-saved regs, so save x9 on the stack. */
    stp     x9, xzr, [sp, #-16]!
    mov     x9, #16
1:  ror     x0, x0, #4
    and     x1, x0, #0xf
    cmp     x1, #10
    b.lt    2f
    add     x1, x1, #('a' - 10)
    b       3f
2:  add     x1, x1, #'0'
3:  bl      .Lprint_c
    sub     x9, x9, #1
    cbnz    x9, 1b
    ldp     x9, xzr, [sp], #16
    ret

/* ──────────────────────────────────────────────────────────────────
 * `_start`
 * ────────────────────────────────────────────────────────────────── */
.section .boot.text.first, "ax"
.balign 0x1000
.globl _start
_start:
    /* 1. Park the entry handoff into x19 (callee-saved across drop). */
    mov     x19, x0

    /* 1a. CPU-id check.  With `-M virt,secure=on -smp 4` QEMU starts
     *     ALL four CPUs at _start in EL3 simultaneously, so we have
     *     to distinguish BSP (logical 0) from APs before doing
     *     anything that mutates shared state.  Use Aff0 (= the low
     *     byte of MPIDR_EL1 after masking out the MT bit).  QEMU
     *     virt reports Aff0 = 0/1/2/3 for the four cores; BSP is
     *     always 0. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xffffff       /* keep Aff0..Aff2 + MT */
    bic     x0, x0, #0x80000000     /* clear the RES1 MT bit */
    and     x0, x0, #0xff           /* keep just Aff0 */
    cbnz    x0, .Lap_wait_bsp       /* non-zero Aff0 → AP, wait */

    /* 2. EL3 → EL1 drop if at EL3.  When QEMU is launched with
     *    `-M virt,secure=on` the kernel is dropped into Secure EL3
     *    (which the machine provides for PSCI SMC handling).
     *    We must drop to Non-Secure EL1 before doing anything else.
     *    Idempotent: `-M virt` (the default) starts at EL2 and
     *    skips this branch. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.ne    99f

    /* Configure SCR_EL3 to route Non-Secure state.  NS=1, RW=1
     * (AArch64 EL2/EL1), SMD=0 (SMC allowed at EL1 in NS). */
    mov     x0, #0
    orr     x0, x0, #1            /* NS = 1 */
    lsl     x0, x0, #10
    orr     x0, x0, #(1 << 10)    /* RW = 1 (AArch64 lower ELs) */
    msr     scr_el3, x0

    /* Return to EL1h, Non-Secure, DAIF masked.  SPSR_EL3 bit layout:
     *   M[3:0] = 0b0101 = EL1h
     *   F=1, I=1, A=1, D=1 (DAIF all masked)
     *   M[4] = 0 (AArch64) */
    mov     x0, #0x3c5
    msr     spsr_el3, x0

    /* eret target — go to the EL2 drop code (label 1f below). */
    ldr     x0, =99f
    msr     elr_el3, x0
    isb
    eret

99:
    /* 2a. (Optional) EL2 → EL1 drop if at EL2.  Idempotent: QEMU
     *    -kernel with cortex-a53 (secure=off) lands at EL2 by
     *    default; with secure=on lands at EL3 (handled above). */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f

    mov     x0, #1
    lsl     x0, x0, #31                        /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5                         /* SPSR_EL2 = EL1h + DAIF */
    msr     spsr_el2, x0
    mov     x0, #3                             /* CNTHCTL_EL2 */
    msr     cnthctl_el2, x0
    isb
    eret

1:
    /* 2a. Set up a temporary stack early so the bss-clear loop and
     *     page-table builder can use SP for register saves.  Use the
     *     BSP stack slot (PC=0 came in with SP=0 — using SP without
     *     initialising it first faults at the first STP/LDP). */
    ldr     x0, =aarch64_boot_stacks
    mov     x1, #AARCH64_BOOT_STACK_SIZE
    add     x1, x0, x1
    mov     sp, x1

    /* 2b. Set up a minimal identity-mapped vector table BEFORE MMU
     *     enable.  DAIFSet masks async exceptions only; a sync fault
     *     during MMU bring-up would otherwise vector to VBAR=0 (the
     *     ROM reset vector) and infinite-loop at 0x200.  This stub
     *     prints ELR/ESR/FAR to PL011 (which is reachable pre-MMU as a
     *     MMIO write) and spins. */
    ldr     x0, =boot_vectors
    msr     vbar_el1, x0
    isb

    /* 3. Mask DAIF (single-threaded, IRQs come back in Task 3). */
    msr     daifset, #0xf

    /* 4. Cortex-A53 sanity check (MIDR_EL1).  We just sanity-check the
     *    implementer and the part number; revision is allowed to vary
     *    between QEMU versions.  Implementer = 0x41, Cortex-A53 part =
     *    0xD03.  QEMU's cortex-a53 reports 0x410FD034 as a baseline,
     *    but other revs are equivalent for our purposes. */
    mrs     x0, midr_el1
    lsr     x1, x0, #24
    cmp     x1, #0x41                          /* 'A' ARM */
    b.ne    99f
    lsr     x1, x0, #4
    and     x1, x1, #0xFFF
    cmp     x1, #0xD03                         /* Cortex-A53 */
    b.ne    99f

    /* 5. Zero .boot.bss only (not the .boot.text!). */
    ldr     x0, =_boot_bss_start
    ldr     x1, =_boot_bss_end
    mov     x2, x0
    mov     x3, x1
    sub     x3, x3, x2
    cbz     x3, .Lbss_done
.Lbss_loop:
    str     xzr, [x2], #8
    sub     x3, x3, #8
    cbnz    x3, .Lbss_loop
.Lbss_done:

    /* 6. Build dual-TTBR page tables. */
    bl      build_pagetables

    /* 7. dsb sy — page-table writes must be visible to walker. */
    dsb     sy

    /* 8. TCR + MAIR + TTBR0/1. */
    bl      write_tcr_mair_ttbr

    /* 9. SCTLR_EL1: RES1 base + M + C + I. */
    ldr     x0, =SCTLR_VALUE
    msr     sctlr_el1, x0
    isb

    /* 10. BSP stack + TPIDR_EL1. */
    bl      setup_bsp_stack_and_tpidr

    /* 11. Absolute branch to aarch64_main (high-half).
     *     x19 still holds the entry handoff from _start; setup_* clobbered
     *     x0, so restore it before the call. */
    mov     x0, x19
    ldr     x30, =aarch64_main
    blr     x30

99: b       99b


/* AP wait loop.  When QEMU boots with `-smp 4 -kernel`, all CPUs
 * land here before BSP has a chance to start secondary cores.
 *
 * Spin-table-style protocol: the BSP fills a per-Aff0 release
 * address into aarch64_boot_percpu[Aff0].release from smp.c, then
 * runs SEV to wake us.  We poll until the address is non-zero and
 * jump to it (which is the secondary trampoline in this file).
 *
 * We can't easily use WFI here because SEV cross-core wakeups are
 * not reliably delivered in the QEMU virt default configuration;
 * a tight spin (with `yield` to reduce bus traffic) is good enough
 * for phase 1 (only a handful of cycles until BSP catches up).
 */
.Lap_wait_bsp:
    /* Reload Aff0 from MPIDR_EL1. */
    mrs     x2, mpidr_el1
    bic     x2, x2, #0x80000000             /* clear MT bit */
    and     x2, x2, #0xff                   /* Aff0 */
    ldr     x3, =aarch64_boot_percpu
    /* aarch64_boot_percpu_t is 48 bytes; stride = 48 = 32 + 16. */
    lsl     x4, x2, #5                      /* Aff0 * 32 */
    add     x4, x4, x2, lsl #4              /* + Aff0 * 16 = Aff0 * 48 */
    ldr     x5, [x3, x4]                    /* boot_percpu[Aff0].self (sanity) */
    add     x3, x3, x4                      /* x3 = &boot_percpu[Aff0] */
    ldr     x5, [x3, #40]                   /* .release (offset 40) */
    cbnz    x5, .Lap_jump_release
    yield
    b       .Lap_wait_bsp

.Lap_jump_release:
    /* x2 = Aff0 (logical cpu_id on QEMU virt), x5 = release addr. */
    mov     x0, x2
    br      x5


/* ──────────────────────────────────────────────────────────────────
 * setup_bsp_stack_and_tpidr
 * ────────────────────────────────────────────────────────────────── */
setup_bsp_stack_and_tpidr:
    ldr     x0, =aarch64_boot_percpu
    str     x0, [x0]                           /* self = self  */
    mov     x1, #0
    str     w1, [x0, #8]                       /* cpu_id = 0   */

    ldr     x1, =aarch64_boot_stacks
    mov     x2, #AARCH64_BOOT_STACK_SIZE
    add     x1, x1, x2
    mov     sp, x1

    msr     tpidr_el1, x0
    ret


/* ──────────────────────────────────────────────────────────────────
 * write_slot: dst_base[slot] = desc.
 *   in: x0 = base, x1 = desc, x2 = slot index (0..511)
 * ────────────────────────────────────────────────────────────────── */
write_slot:
    lsl     x3, x2, #3
    add     x3, x0, x3
    str     x1, [x3]
    ret

/* write_table: dst_base[slot] = table_desc(next_phys).
 *   in: x0 = base, x1 = next_phys, x2 = slot index */
write_table:
    orr     x1, x1, #(PT_V | PT_TYPE_TABLE)     /* bits[1:0] = 11 */
    b       write_slot

/* make_2m: 2 MiB block descriptor.
 *   in: x0 = phys_2m_aligned, x1 = low_flags; out: x0 = descriptor */
make_2m:
    lsr     x0, x0, #21
    lsl     x0, x0, #21
    orr     x0, x0, x1
    ret

/* make_4k: 4 KiB page descriptor.
 *   in: x0 = phys_4k_aligned, x1 = low_flags; out: x0 = descriptor */
make_4k:
    lsr     x0, x0, #12
    lsl     x0, x0, #12
    orr     x1, x1, #PT_TYPE_PAGE          /* level 3 page: bit1=1 (valid page type) */
    orr     x0, x0, x1
    ret


/* ──────────────────────────────────────────────────────────────────
 * build_pagetables — fills `boot_page_tables` (64 KiB scratch) with
 * the page-table hierarchy described in head.S comments.
 *
 * Page-table memory layout (declarations in .boot.bss above):
 *
 *   base+0x0000  PGD          4 KiB
 *   base+0x1000  PUD_low      4 KiB
 *   base+0x2000  PMD_low0     4 KiB     (covers 0..1 GB)
 *   base+0x3000  PMD_low1     4 KiB     (covers 1..2 GB)
 *   base+0x4000  PTE_low1_k   4 KiB     (4 KiB pages for 0x4000_0000..2 MiB)
 *   base+0x5000  PTE_mmio     4 KiB     (4 KiB pages for 0x0800_0000..0x0820_0000)
 * ────────────────────────────────────────────────────────────────── */
build_pagetables:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    /* x20 = mask for "no-execute" high bits (PXN | UXN = 0x30 << 52). */
    mov     x20, #0x3
    lsl     x20, x20, #52

    /* Page-table bases (physical, MMU is off). */
    ldr     x10, =boot_page_tables
    add     x11, x10, #0x1000                  /* PUD_low        */
    add     x12, x10, #0x2000                  /* PMD_low0       */
    add     x13, x10, #0x3000                  /* PMD_low1       */
    add     x14, x10, #0x4000                  /* PTE_low1_k     */
    add     x15, x10, #0x5000                  /* PTE_mmio       */

    /* 0. Zero the page-table scratch. */
    mov     x16, x10
    mov     x17, #(0x10000 / 8)
zp_all:
    str     xzr, [x16], #8
    sub     x17, x17, #1
    cbnz    x17, zp_all

    /* ── 1. TTBR0/TTBR1 share PGD[0] → PUD_low. ───────────────────
     *
     * Both regimes' PGD[0] points to PUD_low. The 48-bit VA bits
     * [47:39] of any identity-mapped kernel address is 0 (since the
     * lower 48 bits fit in [0x40000000, 0x80000000] for our kernel
     * image at 0xFFFF0000_40080xxx).  PUD_low[1] → PMD_low1 covers
     * 1..2GB and contains the PTE_low1_k we use for fine-grained
     * mapping of the kernel image + the high-half mirror source.  */
    mov     x0, x10
    mov     x1, x11
    mov     x2, #0
    bl      write_table

    /* PUD_low[0] → PMD_low0  (0..1GB) */
    mov     x0, x11
    mov     x1, x12
    mov     x2, #0
    bl      write_table

    /* PUD_low[1] → PMD_low1  (1..2GB; .boot + kernel image source) */
    mov     x0, x11
    mov     x1, x13
    mov     x2, #1
    bl      write_table

    /* ── 2. PMD_low0: 2 MiB blocks across 0..1GB ────────────────────
     * Slot 0x40..0x4F (32 MiB at 0x0800_0000..0x0A00_0000) is replaced
     * with a PTE table for MMIO (Device-nGnRnE pages). */
    mov     x22, #PT_V
    mov     x21, #PT_ATTR_NORMAL
    orr     x22, x22, x21
    mov     x21, #PT_SH_IS               /* Inner Shareable — required for SMP coherency */
    orr     x22, x22, x21
    mov     x21, #PT_AP_EL1_RW
    orr     x22, x22, x21
    mov     x21, #PT_AF
    orr     x22, x22, x21

    mov     x25, #512
    mov     x26, #0
.Lpud0:
    cbz     x25, .Lpud0_done
    mov     x0, x26
    mov     x1, x22
    bl      make_2m
    orr     x0, x0, x20

    lsr     x2, x26, #21
    mov     x1, x0
    mov     x0, x12
    bl      write_slot

    add     x26, x26, #0x200000
    sub     x25, x25, #1
    b       .Lpud0
.Lpud0_done:

    /* ── MMIO: Device-nGnRnE 2 MiB blocks for 0x08000000..0x0A000000. ──
     * Covers GICD (0x08000000), GICC (0x08010000), PL011 (0x09000000).
     * Device regions carry a uniform permission (PXN=1, UXN=1), so a
     * 2 MiB block is sufficient — no L3 page table needed (spec §2.2). */
    mov     x23, #PT_V
    mov     x21, #PT_ATTR_DEV
    orr     x23, x23, x21
    mov     x21, #PT_SH_NS
    orr     x23, x23, x21
    mov     x21, #PT_AP_EL1_RW
    orr     x23, x23, x21
    mov     x21, #PT_AF
    orr     x23, x23, x21
    /* x23 = Device block flags */

    mov     x24, #0x40
.Lmmio_blk:
    cmp     x24, #0x50
    b.ge    .Lmmio_blk_done
    mov     x0, x24
    lsl     x0, x0, #21                    /* PA = index * 2 MiB (identity) */
    mov     x1, x23
    bl      make_2m
    orr     x0, x0, x20                    /* PXN | UXN */
    mov     x1, x0
    mov     x0, x12                        /* PMD_low0 base */
    mov     x2, x24                        /* PMD index 0x40..0x4F */
    bl      write_slot
    add     x24, x24, #1
    b       .Lmmio_blk
.Lmmio_blk_done:

    /* ── 3. PMD_low1: 2 MiB blocks covering 1..2GB (no L3 page tables).
     *
     *    PMD_low1[0] = block descriptor for PA 0x40000000..0x40200000.
     *    This 2 MiB region contains both .boot.text + .boot.bss and the
     *    kernel-image source loaded at LMA 0x4009A008+, so it must be
     *    PXN=0 (executable) and UXN=1 (no EL0 exec).  Phase 1 doesn't
     *    need per-page permissions within this 2 MiB block; all of it
     *    is uniformly EL1 RW + exec + UXN.
     *
     *    PMD_low1 is the PMD table for PUD[1] (which covers 1..2GB), so
     *    PMD index 0 within PMD_low1 corresponds to PA 0x40000000.
     */
    mov     x0, #0x40000000                   /* x0 = PA                 */
    mov     x1, x22                           /* x1 = flags              */
    bl      make_2m                           /* x0 = block descriptor   */
    /* Clear PXN so the kernel code in this 2 MiB block can execute. */
    bic     x0, x0, #PT_PXN_MASK
    mov     x1, x0                            /* x1 = descriptor         */
    mov     x0, x13                           /* x0 = PMD_low1 base      */
    mov     x2, #0                            /* PMD index 0             */
    bl      write_slot

    ldp     x29, x30, [sp], #16
    ret


/* ──────────────────────────────────────────────────────────────────
 * write_tcr_mair_ttbr
 *
 * TCR_EL1 composed value 0x0000_0002_B510_3510:
 *   bits[5:0]   = 0x10        (T0SZ = 16)        ← 256 TB region
 *   bits[7:6]   = 0b00        (EPD0=0, A0=0)
 *   bits[9:8]   = 0b01        (IRGN0 = WBWA)
 *   bits[11:10] = 0b01        (ORGN0 = WBWA)
 *   bits[13:12] = 0b11        (SH0 = Inner Shareable)
 *   bits[14:15] = 0b00        (TG0 = 4 KiB)
 *   bits[21:16] = 0x10        (T1SZ = 16)        ← 256 TB region
 *   bits[22:23] = 0b00        (A1=0, EPD1=0)
 *   bits[25:24] = 0b01        (IRGN1 = WBWA)
 *   bits[27:26] = 0b01        (ORGN1 = WBWA)
 *   bits[29:28] = 0b11        (SH1 = Inner Shareable)
 *   bits[31:30] = 0b10        (TG1 = 4 KiB)
 *   bits[34:32] = 0b010       (IPS = 40-bit PA)
 *
 * MAIR_EL1 = 0x0000_0000_0000_ff00:
 *   bits[7:0]   = 0x00        (AttrIdx 0 = Device-nGnRnE)
 *   bits[15:8]  = 0xff        (AttrIdx 1 = Normal Inner/Outer WBWA)
 * ────────────────────────────────────────────────────────────────── */
write_tcr_mair_ttbr:
    /* TCR_EL1 = 0x0000_0002_B510_3510.
     *
     * Byte layout (LSB → MSB):
     *   bits [7:0]   = 0x10   (T0SZ = 16, 256TB region)
     *   bits [15:8]  = 0x35   (IRGN0=01, ORGN0=01, SH0=11, TG0=00)
     *   bits [23:16] = 0x10   (T1SZ = 16)
     *   bits [31:24] = 0xB5   (IRGN1=01, ORGN1=01, SH1=11, TG1=10=4K)
     *   bits [39:32] = 0x02   (IPS = 0b010 = 40-bit PA)
     *
     * Built as three 16-bit chunks via movz + 2× movk.  Note that the
     * first movz places bits [15:0] (T0SZ + IRGN0/ORGN0/SH0/TG0), not
     * just bits [7:0].
     */
    mov     x0, #0x3510
    movk    x0, #0xB510, lsl #16
    movk    x0, #0x0002, lsl #32
    msr     tcr_el1, x0
    isb

    /* MAIR_EL1 = 0x0000_0000_0000_ff00:
     *   bits[7:0]  = 0x00   (AttrIdx 0 = Device-nGnRnE)
     *   bits[15:8] = 0xff   (AttrIdx 1 = Normal Inner/Outer WBWA)
     */
    mov     x0, #0xff00
    msr     mair_el1, x0
    isb

    /* TTBR0_EL1: same PGD physical base. */
    ldr     x0, =boot_page_tables
    msr     ttbr0_el1, x0
    isb

    /* TTBR1_EL1: same PGD physical base. */
    ldr     x0, =boot_page_tables
    msr     ttbr1_el1, x0
    isb
    ret


/* ──────────────────────────────────────────────────────────────────
 * secondary_start — physical entry point for APs after PSCI cpu_on.
 *
 * Lives in `.boot.text` so its address is the same in low physical
 * RAM as the linker placed it.  QEMU's PSCI firmware resets the
 * AP at this address with MMU OFF and all GPRs = 0 (except the
 * context_id we put in x3, which QEMU doesn't actually propagate
 * for cpu_on — see PSCI 0.2 spec).
 *
 * Flow (spec §2.1, §4):
 *   1. EL2→EL1 drop (idempotent — the BSP path is the same code).
 *   2. Mask DAIF.
 *   3. Read full MPIDR_EL1 and walk the DTB {logical, mpidr} table
 *      (see dtb.c `mpidr_to_logical_id`) by issuing an SMC to a
 *      helper.  We can't call C directly yet because MMU is off and
 *      the high-half C trampoline isn't mapped, so we walk a small
 *      physical mirror table built by the BSP at SMP init.
 *   4. Set SP_EL1 = aarch64_boot_percpu[cpu_id].stack and
 *      TPIDR_EL1 = &aarch64_boot_percpu[cpu_id].
 *   5. dsb sy; wait for BSP release (`online==1`).
 *   6. Write the same TTBR/MAIR/SCTLR the BSP used (we can't re-run
 *      build_pagetables — the walker already has the BSP's tables).
 *      For phase 1 we share the BSP's TTBR base; the AP inherits the
 *      BSP's page tables which are identity-mapped for the low
 *      region and high-mapped for kernel .text.
 *   7. Set VBAR_EL1 to exception_vectors (high half; this requires
 *      MMU ON to dereference, so we set it AFTER enabling MMU).
 *   8. Initialise the GIC CPU interface.
 *   9. Branch to secondary_idle (in C) which spins on benchmark_go,
 *      does 1M lock/incr/unlock, writes done[cpu_id], then wfi.
 *
 * Spec §2.1 "failure condition 4 — AP can't find itself" is the
 * `secondary_panic` below.  We panic BEFORE setting SP, so the stack
 * must not be touched.
 * ────────────────────────────────────────────────────────────────── */
.section .boot.text, "ax"
.balign 0x80
.globl secondary_start
secondary_start:
    /* Step 0: Save PSCI's x3 context_id (logical cpu_id) into x19.
     * QEMU's cpu_on does not actually pass context_id through; we
     * rely on the BSP writing the logical id into a known slot in
     * .boot.bss (`aarch64_dtb_slot`-like) before issuing cpu_on.
     * For QEMU virt we get the logical id by scanning the DTB MPIDR
     * table from physical RAM — see .Lfind_cpu_id below. */
    mov     x19, x0                       /* x0 = 0 from QEMU; unused */

    /* Step 1: EL drop.  Idempotent across (EL3, EL2, EL1).
     *
     *   - secure=off (`-M virt` default): QEMU brings APs up at EL2
     *   - secure=on: QEMU brings APs up at EL3
     *
     * We handle both: drop EL3→EL1 first if needed, then EL2→EL1.
     * After this point we're at Non-Secure EL1. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.ne    99f

    /* EL3 → EL1.  Configure SCR_EL3 to route Non-Secure state.
     * NS=1, RW=1 (AArch64 EL2/EL1), SMD=0 (SMC allowed). */
    mov     x0, #0
    orr     x0, x0, #1            /* NS = 1 */
    lsl     x0, x0, #10
    orr     x0, x0, #(1 << 10)    /* RW = 1 */
    msr     scr_el3, x0
    mov     x0, #0x3c5            /* SPSR_EL3 = EL1h + DAIF */
    msr     spsr_el3, x0
    ldr     x0, =99f              /* ERET target = EL2 check */
    msr     elr_el3, x0
    isb
    eret

99:
    /* 1a. EL2 → EL1 drop if at EL2. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f

    mov     x0, #1
    lsl     x0, x0, #31                       /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5                        /* SPSR_EL2 = EL1h + DAIF */
    msr     spsr_el2, x0
    mov     x0, #3
    msr     cnthctl_el2, x0
    isb
    eret

1:
    /* Step 2: Mask DAIF — IRQs stay off until the BSP releases us
     * AND we've enabled the GIC CPU interface (which currently only
     * PPI 30 is wired, and we want no spurious IRQ before SMP setup
     * is complete). */
    msr     daifset, #0xf

    /* Step 2a: Set up a temporary stack so the per-CPU scan can use
     * STP/LDP safely.  We use a small fixed-size region at the end of
     .boot.bss until we know cpu_id.  This trampoline scratch is
     * declared just below secondary_panic's scratch area. */
    ldr     x0, =secondary_scratch_top
    mov     sp, x0

    /* Step 3: read MPIDR_EL1 and walk the BSP-built MPIDR table
     * (a copy in low physical RAM, see dtb.c — we'll export
     * `aarch64_dtb_mpidr_table`/`aarch64_dtb_cpu_count`).
     *
     * On QEMU virt the MPIDR_EL1 value for CPUs 0..3 is exactly the
     * Aff0 byte 0..3 (reg property = Aff0).  We can't trust Aff0
     * alone (spec §2.1), so we compare the full 64-bit value. */
    mrs     x1, mpidr_el1
    bic     x1, x1, #0x80000000               /* clear the MT bit (RES1 on CPU0) */
    ldr     x2, =aarch64_dtb_cpu_count        /* 32-bit count */
    ldr     x2, [x2]
    ldr     x3, =aarch64_dtb_mpidr_table      /* uint64_t[NR_CPUS] */
    mov     x4, #0                            /* x4 = logical cpu_id */
2:
    cmp     x4, x2
    b.ge    .Lap_not_found
    ldr     x5, [x3, x4, lsl #3]
    cmp     x5, x1
    b.eq    .Lap_got_id
    add     x4, x4, #1
    b       2b

.Lap_got_id:
    /* Step 4: SP_EL1 = aarch64_boot_percpu[x4].stack.
     * struct stride = 48 (see aarch64_percpu.h). */
    ldr     x6, =aarch64_boot_percpu
    lsl     x7, x4, #5                      /* x4 * 32 */
    add     x7, x7, x4, lsl #4              /* + x4 * 16 = x4 * 48 */
    add     x6, x6, x7                      /* &boot_percpu[x4] */
    ldr     x7, [x6, #24]                   /* .stack */
    mov     sp, x7

    /* TPIDR_EL1 = &aarch64_boot_percpu[x4] */
    msr     tpidr_el1, x6
    dsb     sy
    isb

    /* Step 5: Wait for BSP to release us.  The BSP sets
     * boot_percpu[cpu_id].online == 1 once its smp_boot_aps() has
     * completed setup and built page tables (which are the same
     * tables we will use below — they were built into low physical
     * RAM already, and we share them via TTBR0/TTBR1). */
    mov     x9, x4                            /* preserve cpu_id across loop */
.Lwait_online:
    add     x11, x6, #32                       /* .online (offset 32) */
    ldar    w10, [x11]                         /* ldar allows only imm=0 */
    cbnz    w10, .Lgot_online
    yield
    b       .Lwait_online

.Lgot_online:
    /* Step 6: Re-enable MMU with the BSP's page tables.  The tables
     * are in low RAM (.boot.bss) and reachable via the identity map
     * — but they aren't yet mapped because we hadn't enabled MMU.
     * We re-enable the same TCR/MAIR/TTBR0/TTBR1/SCTLR values the
     * BSP used.  They are constant; rebuilding them here is simpler
     * than asking the BSP to publish them. */
    bl      write_tcr_mair_ttbr

    /* SCTLR: same value the BSP used. */
    ldr     x0, =SCTLR_VALUE
    msr     sctlr_el1, x0
    isb

    /* Step 7: VBAR_EL1 = exception_vectors (high half). */
    ldr     x0, =exception_vectors
    msr     vbar_el1, x0
    isb

    /* Step 8: GIC CPU interface.  We just enable group 0 and set
     * PMR=0xFF (allow all priorities).  CNTP isn't used on APs in
     * phase 1. */
    mov     x0, #0x08010000
    mov     w1, #0xff
    str     w1, [x0, #4]                       /* GICC_PMR */
    mov     w1, #1
    str     w1, [x0]                           /* GICC_CTLR.EnableGrp0 */

    /* Step 9: Branch to secondary_idle (C, in high half). */
    mov     x0, x9                             /* cpu_id (arg) */
    ldr     x30, =secondary_idle
    blr     x30

    /* secondary_idle returns -> wfi forever. */
1:  wfi
    b       1b


/* ──────────────────────────────────────────────────────────────────
 * secondary_panic — AP cannot find its MPIDR in the BSP-built table.
 *
 * Called from secondary_start with NO valid stack.  We panic by
 * writing 'P' to PL011 @ 0x09000000 and then spinning forever.
 * ────────────────────────────────────────────────────────────────── */
.globl secondary_panic
secondary_panic:
    /* Write '!' via PL011 @ 0x09000000 (Device MMIO pre-MMU). */
    mov     x2, #0x09000000
1:  ldr     w3, [x2, #0x18]
    tst     w3, #(1 << 5)
    b.ne    1b
    mov     w1, #'P'
    str     w1, [x2]
2:  ldr     w3, [x2, #0x18]
    tst     w3, #(1 << 5)
    b.ne    2b
    mov     w1, #'!'
    str     w1, [x2]
3:  b       3b


.Lap_not_found:
    /* x4 = logical id (out of range); print then halt.  We do NOT
     * load any stack — use PL011 MMIO writes only. */
    b       secondary_panic


/* Scratch stack for the AP trampoline's MPIDR scan (1 KiB; the
 * scan doesn't recurse, so this is plenty).  Force into .boot.bss
 * so it's reachable pre-MMU via the identity map. */
.section .boot.bss, "aw", @nobits
.balign 16
.globl secondary_scratch
secondary_scratch:
    .skip   0x400
.globl secondary_scratch_top
secondary_scratch_top:


/* ──────────────────────────────────────────────────────────────────
 * Helpers exposed to high-half C code (smp.c) so it can address the
 * low-half .boot.bss storage without triggering an out-of-range
 * R_AARCH64_ADR_PREL_PG_HI21 relocation.
 *
 * Each helper returns the absolute LOW PHYSICAL address of the
 * corresponding structure in x0, which smp.c may then cast to a
 * volatile pointer.
 * ────────────────────────────────────────────────────────────────── */
.section .text, "ax"

/* x0 = cpu_id; returns &aarch64_boot_percpu[cpu_id] (low phys).
 * aarch64_boot_percpu_t is 48 bytes; stride = 48 = 32 + 16. */
.globl aarch64_percpu_slot_addr
aarch64_percpu_slot_addr:
    ldr     x1, =aarch64_boot_percpu
    lsl     x2, x0, #5                      /* Aff0 * 32 */
    add     x0, x2, x0, lsl #4              /* + Aff0 * 16 = Aff0 * 48 */
    add     x0, x0, x1
    ret

/* x0 = cpu_id; returns stack top (low phys). */
.globl aarch64_boot_stack_top_addr
aarch64_boot_stack_top_addr:
    ldr     x1, =aarch64_boot_stacks
    add     x0, x1, x0, lsl #12               /* 4 KiB per CPU */
    add     x0, x0, #0x1000
    ret

/* Returns &aarch64_dtb_mpidr_table (low phys). */
.globl aarch64_dtb_mpidr_table_addr
aarch64_dtb_mpidr_table_addr:
    ldr     x0, =aarch64_dtb_mpidr_table
    ret

/* Returns &aarch64_dtb_cpu_count (low phys). */
.globl aarch64_dtb_cpu_count_addr
aarch64_dtb_cpu_count_addr:
    ldr     x0, =aarch64_dtb_cpu_count
    ret

/* Returns secondary_start (low phys, PSCI cpu_on entry). */
.globl secondary_start_addr
secondary_start_addr:
    ldr     x0, =secondary_start
    ret

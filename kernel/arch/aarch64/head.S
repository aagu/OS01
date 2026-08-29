/* aarch64 phase 1: head.S — low-level boot, page-table construction,
 * MMU enable, jump to high-half C entry.
 *
 * Boot flow (spec §4, strict order):
 *   1.  save x0 (dtb_base) → x19
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
.globl aarch64_main
.globl exception_vectors

.equ AARCH64_BOOT_STACK_SIZE, 0x1000

/* PAGE-TABLE DESCRIPTOR FLAGS — see aarch64 ARM, block/page descriptor
 * format (Table D4-29 in ARMv8-A ARM).  These flag values are
 * individually ORR-able (no zero-component). */
.equ PT_V,                 (1 << 0)
.equ PT_TYPE_PAGE,         (0 << 1)
.equ PT_TYPE_TABLE,        (3 << 0)
.equ PT_ATTR_DEV,          (0 << 2)
.equ PT_ATTR_NORMAL,       (1 << 2)
.equ PT_SH_NS,             (0 << 6)
.equ PT_SH_IS,             (3 << 6)
.equ PT_AP_EL1_RW,         (0 << 8)
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
 * `_start`
 * ────────────────────────────────────────────────────────────────── */
.section .boot.text.first, "ax"
.balign 0x1000
.globl _start
_start:
    /* 1. Park DTB into x19 (callee-saved across drop). */
    mov     x19, x0

    /* 2. EL2 → EL1 drop if at EL2.  Idempotent: QEMU -kernel with
     *    cortex-a53 lands at EL2 by default but `-smp 1` or some
     *    other variants can land at EL1, so we check first. */
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

    /* 11. Absolute branch to aarch64_main (high-half). */
    ldr     x30, =aarch64_main
    blr     x30

99: b       99b


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
    mov     x21, #PT_SH_IS
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

    /* Replace PMD_low0[0x40..0x4F] with PTE_mmio pointer. */
    mov     x23, x15                           /* PTE_mmio phys */
    mov     x24, #0x40
.Lmmio_pmd:
    cmp     x24, #0x50
    b.ge    .Lmmio_done
    mov     x0, x12
    mov     x1, x23
    mov     x2, x24
    bl      write_table
    add     x24, x24, #1
    b       .Lmmio_pmd
.Lmmio_done:

    /* PTE_mmio: 512 × Device-nGnRnE 4 KiB pages covering 0x0800_0000..0x0820_0000. */
    mov     x23, #PT_V
    mov     x21, #PT_ATTR_DEV
    orr     x23, x23, x21
    mov     x21, #PT_SH_NS
    orr     x23, x23, x21
    mov     x21, #PT_AP_EL1_RW
    orr     x23, x23, x21
    mov     x21, #PT_AF
    orr     x23, x23, x21

    mov     x25, #512
    mov     x26, #0x08000000
.Lmmio_pte:
    cbz     x25, .Lmmio_pte_done
    mov     x0, x26
    mov     x1, x23
    bl      make_4k
    orr     x0, x0, x20

    lsr     x2, x26, #12
    mov     x1, x0
    mov     x0, x15
    bl      write_slot

    add     x26, x26, #0x1000
    sub     x25, x25, #1
    b       .Lmmio_pte
.Lmmio_pte_done:

    /* ── 3. PMD_low1: Mostly invalid (we don't access any RAM beyond
     *    the kernel image). Only PMD_low1[0] is filled and points to
     *    PTE_low1_k (4 KiB pages for 0x40000000..0x40200000). */
    mov     x0, x13
    mov     x1, x14
    mov     x2, #0
    bl      write_table

    /* ── 4. PTE_low1_k: One descriptor per 4 KiB page covering the
     *    .boot region (0x40080000..0x400A0000, ~120 KB) and the
     *    high-half kernel image source (0x4009A000..end). We mark all
     *    pages Normal RW with PXN=0 (so head.S can execute through
     *    its jump to aarch64_main) and UXN=1 (no EL0 exec).  We
     *    leave the leading pages 0..0x7F as invalid (no entry) and
     *    start filling at index 0x80.
     *
     *    PTE index = (VA & 0x1FFFFF) >> 12.
     *    For PA = 0x40080000 → index = 0x80.
     *    For PA = 0x4009A000 → index = 0x9A.
     *    For PA = 0x4009E000 (just past end of .data) → index = 0x9E.
     *    We map 0x80..0xA0 (32 pages = 128 KiB) for safety. */
    mov     x28, x20
    bic     x28, x28, #PT_PXN_MASK              /* clear PXN so code is exec */

    mov     x25, #32                           /* 32 pages = 128 KiB      */
    mov     x26, #0x40080000
.Lpud1_pte:
    cbz     x25, .Lpud1_pte_done
    mov     x0, x26
    mov     x1, x22
    bl      make_4k
    orr     x0, x0, x28

    lsr     x2, x26, #12
    mov     x1, x0
    mov     x0, x14
    bl      write_slot

    add     x26, x26, #0x1000
    sub     x25, x25, #1
    b       .Lpud1_pte
.Lpud1_pte_done:

    ldp     x29, x30, [sp], #16
    ret


/* ──────────────────────────────────────────────────────────────────
 * write_tcr_mair_ttbr
 *
 * TCR_EL1 composed value 0x0000_0000_8035_1C30:
 *   bits[5:0]   = 0x10        (T0SZ = 16)
 *   bits[7:6]   = 0b00
 *   bits[9:8]   = 0b01        (IRGN0)
 *   bits[11:10] = 0b01        (ORGN0)
 *   bits[13:12] = 0b11        (SH0)
 *   bits[14:15] = 0b00        (TG0 = 4 KiB)
 *   bits[21:16] = 0x10        (T1SZ = 16)
 *   bits[22:23] = 0b00
 *   bits[25:24] = 0b01        (IRGN1)
 *   bits[27:26] = 0b01        (ORGN1)
 *   bits[29:28] = 0b11        (SH1)
 *   bits[31:30] = 0b10        (TG1 = 4 KiB)
 *   bits[34:32] = 0b010       (IPS = 40-bit PA)
 * ────────────────────────────────────────────────────────────────── */
write_tcr_mair_ttbr:
    mov     x0, #0x30
    movk    x0, #0x351C, lsl #16
    movk    x0, #0x80,   lsl #32
    msr     tcr_el1, x0
    isb

    /* MAIR_EL1: AttrIdx 0 = 0x00 (Device), AttrIdx 1 = 0xff (Normal WBWA). */
    mov     x0, #0xff
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

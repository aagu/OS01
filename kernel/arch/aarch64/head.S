#include <arch/aarch64/boot_offsets.h>
#include <arch/aarch64/boot_layout.h>

.section .boot.bss, "aw", @nobits
.balign 0x1000
.globl boot_page_tables
boot_page_tables:
    .skip   0x6000                            /* four tables + two reserved pages */
.globl boot_page_tables_end
boot_page_tables_end:

/* Entry address recorded by the low boot path (adr _start, Task 3):
 * consumed via aarch64_boot_observed_entry() by arch_early_init. */
.globl aarch64_boot_observed_entry_value
.hidden aarch64_boot_observed_entry_value
aarch64_boot_observed_entry_value:
    .skip   8

/* Early-MMIO handoff parked by _start from a flag-bearing boot_context
 * (console PA, 2 MiB-aligned Device-window base, block count). Zero
 * values mean "no handoff" — consumers fall back to the compile-time
 * boot_layout.h QEMU constants. The UEFI loader zeroes the whole boot
 * image BSS on load, so these read 0 (fallback) even before the
 * .boot.bss clear runs. */
.globl aarch64_boot_early_mmio_console
.hidden aarch64_boot_early_mmio_console
aarch64_boot_early_mmio_console:
    .skip   8
.globl aarch64_boot_early_mmio_devbase
.hidden aarch64_boot_early_mmio_devbase
aarch64_boot_early_mmio_devbase:
    .skip   8
.globl aarch64_boot_early_mmio_devblocks
.hidden aarch64_boot_early_mmio_devblocks
aarch64_boot_early_mmio_devblocks:
    .skip   8

.section .boot.text.first, "ax"
.balign 0x1000
.globl _start
.type _start, %function
_start:
    msr     daifset, #0xf
    mov     x19, x0

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

    /* Resume at the CurrentEL check below. */
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
    msr     cntvoff_el2, xzr
    adr     x0, 1f                             /* resume locally at EL1 */
    msr     elr_el2, x0
    isb
    eret

1:
    msr     spsel, #1
    ldr     x0, =aarch64_boot_stacks
    mov     x1, #AARCH64_BOOT_STACK_SIZE
    add     x1, x0, x1
    mov     sp, x1

    ldr     x0, =boot_vectors
    msr     vbar_el1, x0
    isb

    msr     daifset, #0xf
    bl      aarch64_boot_check_midr

    /* The BSP stack is inside .boot.bss; there is no live stack frame here. */
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
    /* Record the entry address this low path actually executes from,
     * using a PC-relative adr (Task 3 evidence). Must run after the
     * .boot.bss clear (the store would otherwise be erased) and before
     * the MMU is enabled. */
    adr     x0, _start
    ldr     x1, =aarch64_boot_observed_entry_value
    str     x0, [x1]

    /* Park the early-MMIO handoff (console PA + Device window) from
     * the boot_context. x19 is the entry handoff, but the vector
     * printers cannot reach callee-saved registers, so the panic
     * printer and the page-table builder read the parked slots
     * instead. Any slot left zero by a missing/flag-less/invalid
     * handoff makes consumers use the compile-time fallbacks. */
    cbz     x19, .Learly_mmio_parked
    ldr     w0, [x19, #BOOT_CTX_MAGIC_OFF]
    ldr     w1, =BOOT_CTX_MAGIC
    cmp     w0, w1
    b.ne    .Learly_mmio_parked
    ldr     w0, [x19, #BOOT_CTX_FLAGS_OFF]
    tst     w0, #BOOT_CTX_FLAG_EARLY_MMIO
    b.eq    .Learly_mmio_parked
    ldr     x0, [x19, #BOOT_CTX_EARLY_CONSOLE_OFF]
    ldr     x1, =aarch64_boot_early_mmio_console
    str     x0, [x1]
    ldr     x0, [x19, #BOOT_CTX_EARLY_DEVBASE_OFF]
    ldr     x1, =aarch64_boot_early_mmio_devbase
    str     x0, [x1]
    ldr     w0, [x19, #BOOT_CTX_EARLY_DEVBLOCKS_OFF]
    ldr     x1, =aarch64_boot_early_mmio_devblocks
    str     w0, [x1]
.Learly_mmio_parked:

    /* Fail-closed on a flag-bearing handoff whose window cannot be
     * represented in the early identity map ([0, 2 GiB)) or is
     * malformed. The console itself is reachable pre-MMU, so the W!
     * marker is visible even though the window is unusable. */
    ldr     x0, =aarch64_boot_early_mmio_devbase
    ldr     x20, [x0]
    cbz     x20, .Ldev_window_ok            /* no handoff → fallback */
    tst     x20, #(0x200000 - 1)
    b.ne    .Ldev_window_bad
    ldr     x0, =aarch64_boot_early_mmio_devblocks
    ldr     w21, [x0]
    cbz     w21, .Ldev_window_bad
    ldr     x0, =1024
    cmp     w21, w0                          /* also bounds the loop below */
    b.hi    .Ldev_window_bad
    lsl     x22, x21, #21
    add     x22, x22, x20
    ldr     x0, =AARCH64_BOOT_MAP_END
    cmp     x22, x0
    b.hi    .Ldev_window_bad
    /* The executable boot block B must stay mapped Normal-X: a Device
     * descriptor over it would brick the kernel image fetch. */
    ldr     x0, =AARCH64_BOOT_BLOCK_END
    cmp     x20, x0
    b.hs    7f                              /* base >= B_END: clear */
    ldr     x0, =AARCH64_BOOT_BLOCK_BASE
    cmp     x22, x0
    b.hi    .Ldev_window_bad                /* end > B_BASE: overlap */
7:
.Ldev_window_ok:

    ldr     x0, =boot_page_tables
    ldr     x1, =AARCH64_BOOT_BLOCK_BASE
    /* x2/x3 = Device window (parked handoff, else QEMU constants). */
    ldr     x2, =aarch64_boot_early_mmio_devbase
    ldr     x2, [x2]
    cbnz    x2, 5f
    ldr     x2, =AARCH64_BOOT_DEVICE_BASE
    ldr     x3, =AARCH64_BOOT_DEVICE_END
    b       6f
5:  ldr     x3, =aarch64_boot_early_mmio_devblocks
    ldr     w4, [x3]
    lsl     x3, x4, #21
    add     x3, x3, x2
6:  bl      aarch64_build_boot_page_tables

    dsb     sy
    bl      aarch64_boot_install_translation
    ldr     x0, =AARCH64_BOOT_SCTLR
    msr     sctlr_el1, x0
    isb

    ldr     x0, =percpu_data
    msr     tpidr_el1, x0

    mov     x0, x19
    ldr     x30, =kernel_main
    blr     x30

99: b       99b
.size _start, . - _start

/* Device-window fail-closed: the parked handoff carries a window the
 * early identity map cannot represent (misaligned, empty, oversized,
 * or beyond [0, 2 GiB)). Print "W!" and spin — continuing would fault
 * on the first Device access or silently treat RAM as Device. */
.Ldev_window_bad:
    mov     x1, #'W'
    bl      aarch64_boot_print_c
    mov     x1, #'!'
    bl      aarch64_boot_print_c
.Ldev_window_bad_spin:
    b       .Ldev_window_bad_spin

.section .boot.text, "ax"
.globl aarch64_boot_install_translation
.hidden aarch64_boot_install_translation
aarch64_boot_install_translation:
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

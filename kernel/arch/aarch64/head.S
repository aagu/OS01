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
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #0
    bl      aarch64_boot_fb_mark        /* kernel entry reached */
    /* Slot 4 carries the current Exception Level as a colour:
     * green=EL1, blue=EL2, red=EL3, yellow=unexpected. */
    mrs     x10, CurrentEL
    lsr     x10, x10, #2
    mov     w9, #0xff0000
    movk    w9, #0xff00
    cmp     x10, #1
    mov     x11, #0x00ff00
    csel    x9, x11, x9, eq
    cmp     x10, #2
    mov     x11, #0x0000ff
    csel    x9, x11, x9, eq
    cmp     x10, #3
    mov     x11, #0xff0000
    csel    x9, x11, x9, eq
    mov     x10, x9
    mov     w1, #4
    bl      aarch64_boot_fb_mark        /* current EL indicator */
#endif

    /* 2. EL3 → EL1 drop if at EL3.  QEMU-ONLY PATH: this executes only
     *    when QEMU is launched with `-M virt,secure=on` and the bare
     *    ELF is reset into Secure EL3 (which the machine provides for
     *    PSCI SMC handling).  We must drop to Non-Secure EL1 before
     *    doing anything else.
     *
     *    Never taken on real hardware: TF-A and UEFI firmware hand the
     *    kernel off at EL2/EL1, not EL3, and the stores below blindly
     *    overwrite SCR_EL3/SPSR_EL3 — on a real secure monitor that
     *    would clobber secure-world-dependent bits.  Real-hardware
     *    entries take the EL2/EL1 drop in step 2a below instead.
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

#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    /* Slot 8: HCR_EL2.E2H (VHE) state - magenta set, white clear. */
    mrs     x9, hcr_el2
    lsr     x9, x9, #34
    and     x9, x9, #1
    cmp     x9, #1
    mov     x11, #0xffffff
    mov     w10, #0xff0000
    movk    w10, #0x00ff
    csel    x10, x10, x11, eq
    mov     w1, #8
    bl      aarch64_boot_fb_mark        /* E2H indicator */
#endif
    mov     x0, #1
    lsl     x0, x0, #31                        /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x0
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #5
    bl      aarch64_boot_fb_mark        /* after HCR_EL2 write */
#endif
    mov     x0, #0x33ff                        /* CPTR_EL2: TFP=0 (allow FP/SIMD at EL1/EL0) */
    msr     cptr_el2, x0
    mov     x0, #0x3c5                         /* SPSR_EL2 = EL1h + DAIF */
    msr     spsr_el2, x0
    mov     x0, #3                             /* CNTHCTL_EL2 */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #6
    bl      aarch64_boot_fb_mark        /* EL2 config MSRs done */
#endif
    adr     x0, 1f                             /* resume locally at EL1 */
    msr     elr_el2, x0
    isb
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #7
    bl      aarch64_boot_fb_mark        /* just before the EL2 -> EL1 eret */
#endif
    eret

1:
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #1
    bl      aarch64_boot_fb_mark        /* running at EL1, MMU off */
#endif
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
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #2
    bl      aarch64_boot_fb_mark        /* stack + vectors + BSS clear done */
#endif
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

#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    mov     x10, #0xffffff
    mov     w1, #3
    bl      aarch64_boot_fb_mark        /* handoff parked, before the UART store */
#endif

#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    /* Bring-up beacon: one write-only physical store to the UART the
     * firmware already initialized, before the MMU is on. On a board whose
     * UEFI console is HDMI this is the first serial evidence that the
     * kernel entry and the handoff park completed. No register read (a
     * read of an address with no device behind it faults); a host that
     * lacks the device drops the store instead. */
    ldr     x0, =aarch64_boot_early_mmio_console
    ldr     x0, [x0]
    cbz     x0, .Lkernel_beacon_done
    mov     x1, #'K'
    str     w1, [x0]
.Lkernel_beacon_done:
#endif

    /* Fail-closed on a flag-bearing handoff whose Device window is
     * malformed or cannot be represented in the boot tables. Two
     * placements are representable: wholly inside the identity map
     * [0, 2 GiB), or wholly above it — within the 512 GiB the boot L0
     * entry covers and inside a single 1 GiB L1 slot (one reserved L2
     * table per window). The console itself is reachable pre-MMU, so the
     * W! marker is visible even when the window is unusable. */
    ldr     x0, =aarch64_boot_early_mmio_devbase
    ldr     x20, [x0]
    cbnz    x20, 8f
    /* No handoff: the compile-time QEMU window, inside the identity map. */
    ldr     x20, =AARCH64_BOOT_DEVICE_BASE
    ldr     x22, =AARCH64_BOOT_DEVICE_END
    b       .Ldev_window_ok
8:  tst     x20, #(0x200000 - 1)
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
    b.ls    2f                              /* ends below 2 GiB: a low window */
    /* A window reaching above the identity map must start above it too. */
    cmp     x20, x0
    b.lo    .Ldev_window_bad
    ldr     x0, =AARCH64_BOOT_HIGH_END
    cmp     x22, x0
    b.hi    .Ldev_window_bad
    lsr     x0, x20, #30
    sub     x1, x22, #1
    lsr     x1, x1, #30
    cmp     x0, x1
    b.ne    .Ldev_window_bad
    b       .Ldev_window_ok
2:  /* The executable boot block B must stay mapped Normal-X: a Device
     * descriptor over it would brick the kernel image fetch. */
    ldr     x0, =AARCH64_BOOT_BLOCK_END
    cmp     x20, x0
    b.hs    .Ldev_window_ok                  /* base >= B_END: clear */
    ldr     x0, =AARCH64_BOOT_BLOCK_BASE
    cmp     x22, x0
    b.hi    .Ldev_window_bad                 /* end > B_BASE: overlap */
.Ldev_window_ok:
    /* The console only needs a Device window of its own when its 2 MiB
     * block lies outside the Device window (the loader keeps a device-tree
     * console inside it; a firmware-initialized one may sit anywhere).
     * x23 = that block's base, 0 when there is none. Same two placements
     * as the window; a low block must additionally keep clear of B. */
    ldr     x0, =aarch64_boot_early_mmio_console
    ldr     x23, [x0]
    bic     x23, x23, #0x1fffff
    cbz     x23, 3f
    cmp     x23, x20
    b.lo    4f                               /* below the window */
    cmp     x23, x22
    b.hs    4f                               /* at or above its end */
    mov     x23, #0                          /* covered by the window */
    b       3f
4:  ldr     x0, =AARCH64_BOOT_MAP_END
    add     x1, x23, #0x200000
    cmp     x1, x0
    b.hi    5f                               /* above the identity map */
    ldr     x0, =AARCH64_BOOT_BLOCK_BASE
    cmp     x23, x0                          /* B is one 2 MiB block */
    b.eq    .Ldev_window_bad
    b       3f
5:  ldr     x0, =AARCH64_BOOT_HIGH_END
    cmp     x1, x0
    b.hi    .Ldev_window_bad
3:
    ldr     x0, =boot_page_tables
    ldr     x1, =AARCH64_BOOT_BLOCK_BASE
    /* x2/x3 = the Device window while it lies inside the identity map, else
     * an empty one: what sits above gets its own L2 table below. */
    ldr     x4, =AARCH64_BOOT_MAP_END
    cmp     x20, x4
    b.hs    5f
    mov     x2, x20
    mov     x3, x22
    b       6f
5:  mov     x2, #0
    mov     x3, #0
6:  bl      aarch64_build_boot_page_tables

    ldr     x4, =AARCH64_BOOT_MAP_END
    cmp     x20, x4
    b.lo    7f
    ldr     x0, =boot_page_tables
    mov     x1, #5                          /* reserved L2 table: high Device window */
    mov     x2, x20
    mov     x3, x22
    bl      aarch64_build_boot_window
7:  cbz     x23, 8f
    ldr     x0, =boot_page_tables
    mov     x1, #4                          /* reserved L2 table: console block */
    mov     x2, x23
    add     x3, x23, #0x200000
    bl      aarch64_build_boot_window
8:

    dsb     sy
    bl      aarch64_boot_install_translation
    ldr     x0, =AARCH64_BOOT_SCTLR
    msr     sctlr_el1, x0
    isb

    /* Switch BSP stack pointer from low identity VA to TTBR1 high-half VA
     * before jumping to kernel_main, so installing the empty TTBR0 root
     * during boot-5 does not unmap the active boot stack. */
    movz    x0, #0xffff, lsl #48
    add     sp, sp, x0

    ldr     x0, =percpu_data
    msr     tpidr_el1, x0

    mov     x0, x19
    ldr     x30, =kernel_main
    blr     x30

99: b       99b
.size _start, . - _start

/* Device-window fail-closed: the parked handoff carries a Device window
 * or console block that is malformed or the boot tables cannot represent
 * (misaligned, empty, oversized, straddling the identity map, beyond the
 * 512 GiB the boot L0 entry reaches, split across 1 GiB L1 slots, or
 * covering the boot block). Print "W!" followed by the rejected handoff —
 * window base, block count, console PA, 16 hex digits each — and spin:
 * continuing would fault on the first Device access or silently treat
 * RAM as Device. A SoC whose firmware leaves its devices above 512 GiB
 * stops here, visibly. */
.Ldev_window_bad:
    mov     x1, #'W'
    bl      aarch64_boot_print_c
    mov     x1, #'!'
    bl      aarch64_boot_print_c
    ldr     x0, =aarch64_boot_early_mmio_devbase
    ldr     x0, [x0]
    bl      aarch64_boot_print_x64
    mov     x1, #' '
    bl      aarch64_boot_print_c
    ldr     x0, =aarch64_boot_early_mmio_devblocks
    ldr     x0, [x0]
    bl      aarch64_boot_print_x64
    mov     x1, #' '
    bl      aarch64_boot_print_c
    ldr     x0, =aarch64_boot_early_mmio_console
    ldr     x0, [x0]
    bl      aarch64_boot_print_x64
    mov     x1, #10
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

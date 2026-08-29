/* aarch64 phase 1: minimal GICv2 init (Task 3).
 *
 * Spec §2.3: only PPI 30 (CNTP_NS, non-secure EL1 physical timer) is
 * enabled.  We do not enable SPI 33 (PL011), so the PL011 stays
 * polled.  The CPU-interface PMR is set to the lowest value (0xFF)
 * so every priority above 0 is masked except level 0; we then
 * unmask by writing 0xFF to PMR (allow all priorities) and enable
 * the interface with GICC_CTLR.EnableGrp0 = 1.
 *
 * Distributor:
 *   - GICD_CTLR.EnableGrp0 = 1
 *   - GICD_ISENABLER0 has PPI 30 (bit 30 of reg[0]) set
 *   - GICD_ITARGETSR for PPI 30 routed to CPU 0
 *
 * All MMIO addresses come from the DTB (or fall back to QEMU virt
 * defaults); dtb_init() must have been called first.
 */

#include <stdint.h>
#include "reg.h"   /* GICv2 register definitions */

/* Forward from pl011.c. */
void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

/* DTB-parsed GIC base (see dtb.c). */
extern uint64_t dtb_gicd_base(void);
extern uint64_t dtb_gicc_base(void);
extern uint32_t dtb_cntp_ppi(void);

static void gic_panic(const char *what)
{
    kputs("[gic] PANIC: ");
    kputs(what);
    kputs("\n");
    for (;;) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}

void gic_init(void)
{
    /* If the DTB told us a different base, use it; the constants in
     * reg.h are the fall-back.  reg.h's gicd_write32 / gicc_write32
     * use the hard-coded GICD_BASE / GICC_BASE, so we re-derive
     * pointers here from dtb_gicd_base() / dtb_gicc_base(). */
    volatile uint32_t *gicd = (volatile uint32_t *)dtb_gicd_base();
    volatile uint32_t *gicc = (volatile uint32_t *)dtb_gicc_base();

    /* Sanity: IIDR must be non-zero (GICv2 implementations always
     * implement this register; zero implies we are not talking to a
     * GIC at all).  This is also a cheap way to confirm MMIO is
     * reachable. */
    uint32_t iidr = gicd[GICD_IIDR / 4];
    if (iidr == 0) {
        gic_panic("GICD IIDR=0 (no GIC?)");
    }

    /* Distributor control: enable group 0.  GICD_CTLR bit 0
     * (EnableGrp0) is the only one we need for a non-secure GICv2
     * (QEMU virt).  Bit 1 (EnableGrp1) is RAZ/WI; we do not touch
     * it.
     *
     * We don't touch ARE (bit 4) — GICv2 always uses the legacy
     * ITARGETSR for routing.
     */
    gicd[GICD_CTLR / 4] = (1U << 0);

    /* Configure PPI (default 30 = CNTP physical timer):
     *   - IGROUPR[30] = 0  (group 0, so it goes to the regular IRQ
     *     line — EL1 IRQ).  Default after reset is 0 already, but
     *     write explicitly to be safe.
     *   - IPRIORITYR[30] = 0  (highest priority).
     *
     * Note: GICD_ITARGETSR for PPIs (INTID 16..31) is read-only on
     * GICv2 (routing is hard-wired to the bank owner).  QEMU virt's
     * GICv2 model follows that.  We therefore skip the ITARGETSR
     * write and trust the bank routing.
     */
    uint32_t intid = dtb_cntp_ppi();
    if (intid > 31) {
        /* We only configured PPIs.  An SPI 30 would require bank
         * gymnastics we don't have; panic. */
        gic_panic("CNTP PPI is not a PPI (not in 16..31)");
    }

    /* Group 0: clear bit for INTID `intid`.  Each register holds 32
     * bits; the bit position is `intid` itself.
     *
     * On a non-secure GICv2 (QEMU virt) this register is RAZ/WI
     * (all INTIDs are implicitly group 0).  Writing it is harmless
     * on QEMU but we skip the write to avoid any side effect. */
    if (intid < 16) {
        /* SGI range — never enabled in phase 1, skip silently. */
    } else {
        gicd[(GICD_IGROUPR + (intid / 32) * 4) / 4] =
            gicd[(GICD_IGROUPR + (intid / 32) * 4) / 4] & ~(1U << (intid % 32));
    }

    /* Priority: byte at IPRIORITYR + (intid/4)*4, mask 0xFF << shift. */
    uint32_t prio_off = GICD_IPRIORITYR + (intid / 4) * 4;
    uint32_t prio_shift = (intid % 4) * 8;
    gicd[prio_off / 4] = (gicd[prio_off / 4] & ~(0xFFU << prio_shift));

    /* ICFGR: PPIs default to edge-triggered (per spec).  CNTP is
     * actually level-sensitive on the GIC, but the ARM Generic
     * Timer spec says the PPI is "level-high".  QEMU virt treats
     * CNTP's PPI as level-high regardless.  We don't change the
     * ICFGR — the reset value is fine for QEMU. */

    /* Enable: write 1 to ISENABLER bit. */
    gicd[(GICD_ISENABLER + (intid / 32) * 4) / 4] = (1U << (intid % 32));

    /* CPU interface:
     *   - GICC_CTLR.EnableGrp0 = 1
     *   - GICC_PMR = 0xFF (unmask all priorities; 0x00 would mask
     *     everything in legacy mode).
     *
     * No BPR write: the default value (binary point = 0, i.e. 8
     * group priority bits, 0 subpriority bits) is what we want
     * for the simple "ack any non-masked IRQ" use case.
     */
    gicc[GICC_PMR / 4]  = 0xFFU;
    gicc[GICC_CTLR / 4] = (1U << 0);

    /* Print a one-line confirmation. */
    kputs("[gic] distributor @ 0x");
    kputx(dtb_gicd_base());
    kputs(", CPU interface @ 0x");
    kputx(dtb_gicc_base());
    kputs(", PPI ");
    kputu(intid);
    kputs(" enabled (SPI 33 NOT enabled)\n");
}

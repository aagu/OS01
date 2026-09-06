/* Minimal QEMU virt GICv2: distributor setup is BSP-only; each CPU
 * configures its banked PPI registers and CPU interface with IRQ masked. */
#include <stdint.h>
#include <kernel/arch/aarch64/boot_log.h>
#include <kernel/arch/aarch64/dtb.h>
#include <kernel/arch/aarch64/smp.h>
#include "reg.h"

void gic_cpu_init(void)
{
    /* DTB validation precedes every call and fixes these mapped windows
     * and the physical-timer PPI (30). No AP writes GICD_CTLR. */
    volatile uint32_t *gicd = (volatile uint32_t *)dtb_gicd_base();
    volatile uint32_t *gicc = (volatile uint32_t *)dtb_gicc_base();
    uint32_t intid = dtb_cntp_ppi();
    gicd[GICD_IGROUPR / 4] &= ~(1U << intid);
    uint32_t prio_off = GICD_IPRIORITYR + (intid / 4) * 4;
    uint32_t prio_shift = (intid % 4) * 8;
    gicd[prio_off / 4] &= ~(0xffU << prio_shift);
    /* PPI routing is fixed to this CPU. Timer interrupt is level-high. */
    gicd[(GICD_ISENABLER + (intid / 32) * 4) / 4] = 1U << (intid % 32);
    gicc[GICC_PMR / 4] = 0xffU;
    gicc[GICC_CTLR / 4] = 1U;
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
}

void gic_init(void)
{
    volatile uint32_t *gicd = (volatile uint32_t *)dtb_gicd_base();
    if (gicd[GICD_IIDR / 4] == 0) {
        log_err("[gic] FATAL: GICD IIDR=0\n");
        for (;;) __asm__ __volatile__("wfi" ::: "memory");
    }
    gicd[GICD_CTLR / 4] = 1U;
    gic_cpu_init();
    log_info("[gic] distributor @ 0x");
    kputx(dtb_gicd_base());
    log_info(", CPU interface @ 0x");
    kputx(dtb_gicc_base());
    log_info(", PPI ");
    kputu(dtb_cntp_ppi());
    log_info(" enabled (SPI 33 NOT enabled)\n");
}

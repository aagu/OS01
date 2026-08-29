/* aarch64 phase 1: GICv2 + Generic Timer register definitions.
 *
 * Only the minimum surface needed for Task 3 (PPI 30, CNTP) is declared.
 * All MMIO addresses follow the QEMU virt defaults (GICD 0x0800_0000,
 * GICC 0x0801_0000) which are also identity-mapped as Device-nGnRnE
 * by head.S.  DTB parsing may overwrite the GIC base; the BSP can
 * still boot with the QEMU default if the DTB is absent or unparsable.
 *
 * See ARM IHI 0048B (GICv2 architecture) and ARM ARM D17 (Generic
 * Timer) for the register layouts used here.
 */

#ifndef _ARCH_AARCH64_GIC_H
#define _ARCH_AARCH64_GIC_H

#include <stdint.h>

/* ── GICv2 MMIO bases (QEMU virt defaults; spec §2.2 / §2.3) ───── */
#ifndef GICD_BASE
#define GICD_BASE     0x08000000UL
#endif
#ifndef GICC_BASE
#define GICC_BASE     0x08010000UL
#endif

/* ── GIC Distributor (GICD) register offsets ─────────────────────── */
#define GICD_CTLR            0x000   /* RW: Distributor Control         */
#define GICD_TYPER           0x004   /* RO: Interrupt Controller Type   */
#define GICD_IIDR            0x008   /* RO: Implementer Identification  */
#define GICD_IGROUPR         0x080   /* RW: Interrupt Group (1 per reg) */
#define GICD_ISENABLER       0x100   /* RW: Interrupt Set-Enable        */
#define GICD_ICENABLER       0x180   /* RW: Interrupt Clear-Enable      */
#define GICD_ISPENDR         0x200   /* RW: Set Pending                 */
#define GICD_ICPENDR         0x280   /* RW: Clear Pending               */
#define GICD_IPRIORITYR      0x400   /* RW: Interrupt Priority (byte)   */
#define GICD_ITARGETSR       0x800   /* RW: Interrupt Target (byte)     */
#define GICD_ICFGR           0xC00   /* RW: Interrupt Configuration     */
#define GICD_NSACR           0xE00   /* RW: Non-Secure Access Control   */
#define GICD_SGIR            0xF00   /* WO: Software Generated Interrupt*/

/* Per-INTID stride for the byte-indexed registers (IPRIORITYR,
 * ITARGETSR).  Each register holds 4 INTIDs. */
#define GICD_PER_REG_STRIDE  4

/* ── GIC CPU Interface (GICC) register offsets ───────────────────── */
#define GICC_CTLR            0x000   /* RW: CPU Interface Control       */
#define GICC_PMR             0x004   /* RW: Priority Mask               */
#define GICC_BPR             0x008   /* RW: Binary Point                */
#define GICC_IAR             0x00C   /* RO: Interrupt Acknowledge       */
#define GICC_EOIR            0x010   /* WO: End Of Interrupt            */
#define GICC_RPR             0x014   /* RO: Running Priority            */
#define GICC_HPPIR           0x018   /* RO: Highest Priority Pending    */
#define GICC_ABPR            0x01C   /* RW: Aliased Binary Point        */
#define GICC_AIAR            0x020   /* RO: Aliased IAR                 */
#define GICC_AEOIR           0x024   /* WO: Aliased EOIR                */
#define GICC_AHPPIR          0x028   /* RO: Aliased HPPIR               */

/* GICC_IAR / GICC_EOIR fields: bits[9:0] = INTID, bits[12:10] = CPUID
 * (only meaningful for SGIs).  Value 1023 = spurious interrupt. */
#define GICC_INTID_SPURIOUS  1023U

/* CNTP physical timer PPI: spec §2.3.  GICv2 INTID numbering treats
 * 0..15 as SGIs, 16..31 as PPIs, 32..1019 as SPIs.  CNTP_NS (non-secure
 * physical timer) is PPI 30, which the QEMU virt DTB confirms. */
#define GIC_PPI_CNTP         30U

/* ── GICv2 register access helpers ─────────────────────────────────
 * Using volatile 32-bit loads/stores (Device-nGnRnE; no write buffering).
 * MMIO is already identity-mapped by head.S so physical == virtual. */
static inline void gicd_write32(uint64_t off, uint32_t v)
{
    *((volatile uint32_t *)(GICD_BASE + off)) = v;
}

static inline uint32_t gicd_read32(uint64_t off)
{
    return *((volatile uint32_t *)(GICD_BASE + off));
}

static inline void gicc_write32(uint64_t off, uint32_t v)
{
    *((volatile uint32_t *)(GICC_BASE + off)) = v;
}

static inline uint32_t gicc_read32(uint64_t off)
{
    return *((volatile uint32_t *)(GICC_BASE + off));
}

/* ── Generic Timer (CNTP) register access ─────────────────────────
 * Spec §2.3: EL1 can access CNTP_*_EL0 / CNTPCT_EL0 directly because
 * head.S set CNTHCTL_EL2 = {EL1PCTEN, EL1PCEN} before dropping to EL1.
 * We use the same approach as cpu.h: 'mrs'/'msr' inline asm. */
static inline uint64_t cntpct_el0(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntfrq_el0(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void cntp_tval_el0_write(uint64_t v)
{
    __asm__ __volatile__("msr cntp_tval_el0, %0" :: "r"(v) : "memory");
}

static inline uint64_t cntp_ctl_el0_read(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntp_ctl_el0" : "=r"(v));
    return v;
}

static inline void cntp_ctl_el0_write(uint64_t v)
{
    __asm__ __volatile__("msr cntp_ctl_el0, %0" :: "r"(v) : "memory");
}

#endif /* _ARCH_AARCH64_GIC_H */

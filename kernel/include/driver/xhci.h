/* kernel/include/driver/xhci.h — xHCI Host Controller Driver definitions */
#ifndef _DRIVER_XHCI_H
#define _DRIVER_XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

/* Ring and TRB constants */
#define XHCI_TRB_RING_SIZE          256
#define XHCI_ERDP_EHB               (1ULL << 3)
#define XHCI_TRB_TYPE_LINK          6
#define XHCI_TRB_LINK_TC            (1U << 1)
#define XHCI_TRB_CYCLE_BIT          (1U << 0)

/* Capability Register Offsets (relative to BAR0 MMIO base) */
#define XHCI_CAP_CAPLENGTH          0x00
#define XHCI_CAP_HCIVERSION         0x02
#define XHCI_CAP_HCSPARAMS1         0x04
#define XHCI_CAP_HCSPARAMS2         0x08
#define XHCI_CAP_HCSPARAMS3         0x0C
#define XHCI_CAP_HCCPARAMS1         0x10
#define XHCI_CAP_DBOFF              0x14
#define XHCI_CAP_RTSOFF             0x18

/* Operational Register Offsets (relative to op_regs = mmio_virt + cap_length) */
#define XHCI_OP_USBCMD              0x00
#define XHCI_OP_USBSTS              0x04
#define XHCI_OP_PAGESIZE            0x08
#define XHCI_OP_DNCTRL              0x14
#define XHCI_OP_CRCR                0x18
#define XHCI_OP_DCBAAP              0x30
#define XHCI_OP_CONFIG              0x38

/* USBCMD bits */
#define XHCI_CMD_RS                 (1U << 0)
#define XHCI_CMD_HCRST              (1U << 1)
#define XHCI_CMD_INTE               (1U << 2)
#define XHCI_CMD_HSEE               (1U << 3)

/* USBSTS bits */
#define XHCI_STS_HCH                (1U << 0)
#define XHCI_STS_HSE                (1U << 2)
#define XHCI_STS_CNR                (1U << 11)

/* Runtime & Interrupter 0 Register Offsets (relative to rt_regs + 0x20) */
#define XHCI_INTR_IMAN              0x00
#define XHCI_INTR_IMOD              0x04
#define XHCI_INTR_ERSTSZ            0x08
#define XHCI_INTR_ERSTBA            0x10
#define XHCI_INTR_ERDP              0x18

/* IMAN bits */
#define XHCI_IMAN_IP                (1U << 0)
#define XHCI_IMAN_IE                (1U << 1)

/* 16-byte Generic TRB (Transfer Request Block) */
struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

/* Event Ring Segment Table Entry (16 bytes) */
struct xhci_erst_entry {
    uint64_t ring_segment_base_address;
    uint16_t ring_segment_size;
    uint16_t rsvdz;
    uint32_t rsvdz2;
} __attribute__((packed));

/* xHCI Controller Instance State */
struct xhci_controller {
    struct pci_device *pdev;

    /* MMIO mappings */
    uint64_t mmio_phys;
    uint64_t mmio_len;
    volatile uint8_t *mmio_virt;
    volatile uint8_t *op_regs;   /* mmio_virt + cap_length */
    volatile uint8_t *rt_regs;   /* mmio_virt + rtsoff */
    volatile uint8_t *db_regs;   /* mmio_virt + dboff */

    /* Capability caches */
    uint32_t cap_length;
    uint32_t hci_version;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint32_t max_scratchpads;
    uint32_t dboff;
    uint32_t rtsoff;

    /* DCBAA */
    uint64_t dcbaa_phys;
    uint64_t *dcbaa_virt;

    /* Command Ring */
    uint64_t cmd_ring_phys;
    struct xhci_trb *cmd_ring_virt;
    uint32_t cmd_ring_enqueue;
    uint8_t cmd_ring_cycle;

    /* Event Ring */
    uint64_t event_ring_phys;
    struct xhci_trb *event_ring_virt;
    uint32_t event_ring_dequeue;
    uint8_t event_ring_cycle;

    /* ERST */
    uint64_t erst_phys;
    struct xhci_erst_entry *erst_virt;

    bool running;
};

/* xHCI MMIO read/write accessors */
#ifndef xhci_read32
static inline uint32_t xhci_read32(volatile const void *addr)
{
    return *(volatile const uint32_t *)addr;
}
#endif

#ifndef xhci_write32
static inline void xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}
#endif

#ifndef xhci_read64
static inline uint64_t xhci_read64(volatile const void *addr)
{
    return *(volatile const uint64_t *)addr;
}
#endif

#ifndef xhci_write64
static inline void xhci_write64(volatile void *addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}
#endif

/* PCI Driver Callbacks & Public APIs */
int xhci_probe(struct pci_device *pdev, const struct pci_device_id *id);
void xhci_remove(struct pci_device *pdev);
int xhci_parse_capabilities(struct xhci_controller *ctrl);
int xhci_reset(struct xhci_controller *ctrl);
int xhci_alloc_dma(struct xhci_controller *ctrl);
void xhci_free_dma(struct xhci_controller *ctrl);
void xhci_setup_interrupter0(struct xhci_controller *ctrl);
int xhci_start(struct xhci_controller *ctrl);
int xhci_cmd_ring_enqueue(struct xhci_controller *ctrl, const struct xhci_trb *trb);
extern const struct pci_driver xhci_pci_driver;

#endif /* _DRIVER_XHCI_H */

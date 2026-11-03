/* kernel/driver/xhci.c — xHCI Host Controller Driver (Phase 1) */
#include <driver/xhci.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <errno.h>
#include <string.h>

#ifndef OS01_HOST_TEST
#include <arch/x86_64/pte.h>
#include <memory/vmm.h>
#endif

#define XHCI_TIMEOUT_LOOPS 500000

int xhci_parse_capabilities(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->mmio_virt) return -EINVAL;

    ctrl->cap_length = ctrl->mmio_virt[XHCI_CAP_CAPLENGTH];
    ctrl->hci_version = *(volatile uint16_t *)(ctrl->mmio_virt + XHCI_CAP_HCIVERSION);

    uint32_t hcsparams1 = xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCSPARAMS1);
    ctrl->max_slots = hcsparams1 & 0xFF;
    ctrl->max_intrs = (hcsparams1 >> 8) & 0x7FF;
    ctrl->max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32_t hcsparams2 = xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCSPARAMS2);
    ctrl->max_scratchpads = (((hcsparams2 >> 21) & 0x1F) << 5) | ((hcsparams2 >> 27) & 0x1F);

    ctrl->dboff = xhci_read32(ctrl->mmio_virt + XHCI_CAP_DBOFF) & ~0x03U;
    ctrl->rtsoff = xhci_read32(ctrl->mmio_virt + XHCI_CAP_RTSOFF) & ~0x1FU;

    ctrl->op_regs = ctrl->mmio_virt + ctrl->cap_length;
    ctrl->rt_regs = ctrl->mmio_virt + ctrl->rtsoff;
    ctrl->db_regs = ctrl->mmio_virt + ctrl->dboff;

    if (ctrl->max_scratchpads > 0) {
        log_warn("[xhci] scratchpad buffers requested (%u), unsupported in Phase 1\n", ctrl->max_scratchpads);
        return -ENOTSUP;
    }

    return 0;
}

static int xhci_wait_bit(volatile void *reg, uint32_t mask, bool expected, volatile uint8_t *op_regs)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT_LOOPS; i++) {
        uint32_t val = xhci_read32(reg);
        if (op_regs) {
            uint32_t sts = xhci_read32(op_regs + XHCI_OP_USBSTS);
            if (sts & XHCI_STS_HSE) {
                log_err("[xhci] fatal: Host System Error (HSE) asserted\n");
                return -EIO;
            }
        }
        if (((val & mask) != 0) == expected) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

int xhci_reset(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return -EINVAL;

    volatile uint8_t *op = ctrl->op_regs;

    /* Step 1: Wait for CNR == 0 */
    int rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_CNR, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: CNR wait failed (%d)\n", rc);
        return rc;
    }

    /* Step 2: Ensure controller is halted (HCH == 1) before resetting */
    uint32_t cmd = xhci_read32(op + XHCI_OP_USBCMD);
    uint32_t sts = xhci_read32(op + XHCI_OP_USBSTS);
    if ((cmd & XHCI_CMD_RS) || !(sts & XHCI_STS_HCH)) {
        xhci_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_HCH, true, op);
        if (rc != 0) {
            log_err("[xhci] reset error: HCH halt wait failed (%d)\n", rc);
            return rc;
        }
    }

    /* Step 3: Write HCRST = 1 */
    cmd = xhci_read32(op + XHCI_OP_USBCMD);
    xhci_write32(op + XHCI_OP_USBCMD, cmd | XHCI_CMD_HCRST);

    /* Step 4: Wait for HCRST == 0 */
    rc = xhci_wait_bit(op + XHCI_OP_USBCMD, XHCI_CMD_HCRST, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: HCRST completion wait failed (%d)\n", rc);
        return rc;
    }

    /* Step 5: Wait for CNR == 0 */
    rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_CNR, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: post-reset CNR wait failed (%d)\n", rc);
        return rc;
    }

    return 0;
}

int xhci_alloc_dma(struct xhci_controller *ctrl)
{
    if (!ctrl) return -EINVAL;

    /* 1. DCBAA (Device Context Base Address Array) */
    ctrl->dcbaa_phys = alloc_4k_page();
    if (!ctrl->dcbaa_phys) return -ENOMEM;
    ctrl->dcbaa_virt = (uint64_t *)Phy_To_Virt(ctrl->dcbaa_phys);
    memset(ctrl->dcbaa_virt, 0, 4096);

    /* 2. Command Ring (256 TRBs) */
    ctrl->cmd_ring_phys = alloc_4k_page();
    if (!ctrl->cmd_ring_phys) return -ENOMEM;
    ctrl->cmd_ring_virt = (struct xhci_trb *)Phy_To_Virt(ctrl->cmd_ring_phys);
    memset(ctrl->cmd_ring_virt, 0, 4096);
    ctrl->cmd_ring_enqueue = 0;
    ctrl->cmd_ring_cycle = 1;

    /* Configure TRB 255 as Link TRB */
    struct xhci_trb *link = &ctrl->cmd_ring_virt[255];
    link->parameter = ctrl->cmd_ring_phys;
    link->status = 0;
    link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    /* 3. Event Ring (256 TRBs) */
    ctrl->event_ring_phys = alloc_4k_page();
    if (!ctrl->event_ring_phys) return -ENOMEM;
    ctrl->event_ring_virt = (struct xhci_trb *)Phy_To_Virt(ctrl->event_ring_phys);
    memset(ctrl->event_ring_virt, 0, 4096);
    ctrl->event_ring_dequeue = 0;
    ctrl->event_ring_cycle = 1;

    /* 4. Event Ring Segment Table (ERST) */
    ctrl->erst_phys = alloc_4k_page();
    if (!ctrl->erst_phys) return -ENOMEM;
    ctrl->erst_virt = (struct xhci_erst_entry *)Phy_To_Virt(ctrl->erst_phys);
    memset(ctrl->erst_virt, 0, 4096);
    ctrl->erst_virt[0].ring_segment_base_address = ctrl->event_ring_phys;
    ctrl->erst_virt[0].ring_segment_size = XHCI_TRB_RING_SIZE;

    return 0;
}

void xhci_setup_interrupter0(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->rt_regs) return;

    volatile uint8_t *ir0 = ctrl->rt_regs + 0x20;

    /* Clear pending interrupts (RW1C), keep IE=0 */
    xhci_write32(ir0 + XHCI_INTR_IMAN, XHCI_IMAN_IP);
    xhci_write32(ir0 + XHCI_INTR_IMOD, 0);

    /* Configure ERST */
    xhci_write32(ir0 + XHCI_INTR_ERSTSZ, 1);
    xhci_write64(ir0 + XHCI_INTR_ERSTBA, ctrl->erst_phys);

    /* Configure ERDP with EHB set to clear busy */
    xhci_write64(ir0 + XHCI_INTR_ERDP, ctrl->event_ring_phys | XHCI_ERDP_EHB);
}

int xhci_cmd_ring_enqueue(struct xhci_controller *ctrl, const struct xhci_trb *trb)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !trb) return -EINVAL;
    if (ctrl->cmd_ring_enqueue >= 255) return -EINVAL;

    struct xhci_trb *slot = &ctrl->cmd_ring_virt[ctrl->cmd_ring_enqueue];
    slot->parameter = trb->parameter;
    slot->status = trb->status;
    slot->control = (trb->control & ~XHCI_TRB_CYCLE_BIT) | (ctrl->cmd_ring_cycle ? XHCI_TRB_CYCLE_BIT : 0);

    ctrl->cmd_ring_enqueue++;
    if (ctrl->cmd_ring_enqueue == 255) {
        /* Update Link TRB cycle bit and wrap to 0 */
        struct xhci_trb *link = &ctrl->cmd_ring_virt[255];
        link->control = (link->control & ~XHCI_TRB_CYCLE_BIT) | (ctrl->cmd_ring_cycle ? XHCI_TRB_CYCLE_BIT : 0);
        ctrl->cmd_ring_cycle ^= 1;
        ctrl->cmd_ring_enqueue = 0;
    }

    return 0;
}

void xhci_free_dma(struct xhci_controller *ctrl)
{
    if (!ctrl) return;
    if (ctrl->erst_phys) {
        free_4k_page(ctrl->erst_phys);
        ctrl->erst_phys = 0;
        ctrl->erst_virt = NULL;
    }
    if (ctrl->event_ring_phys) {
        free_4k_page(ctrl->event_ring_phys);
        ctrl->event_ring_phys = 0;
        ctrl->event_ring_virt = NULL;
    }
    if (ctrl->cmd_ring_phys) {
        free_4k_page(ctrl->cmd_ring_phys);
        ctrl->cmd_ring_phys = 0;
        ctrl->cmd_ring_virt = NULL;
    }
    if (ctrl->dcbaa_phys) {
        free_4k_page(ctrl->dcbaa_phys);
        ctrl->dcbaa_phys = 0;
        ctrl->dcbaa_virt = NULL;
    }
}

int xhci_start(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return -EINVAL;

    volatile uint8_t *op = ctrl->op_regs;

    /* 1. Set MaxSlotsEn */
    uint32_t max_slots_en = ctrl->max_slots;
    if (max_slots_en > 16) max_slots_en = 16;
    xhci_write32(op + XHCI_OP_CONFIG, max_slots_en);

    /* 2. Program DCBAAP */
    xhci_write64(op + XHCI_OP_DCBAAP, ctrl->dcbaa_phys);

    /* 3. Program CRCR with initial RCS = 1 */
    xhci_write64(op + XHCI_OP_CRCR, ctrl->cmd_ring_phys | 1);

    /* 4. Start controller: write USBCMD.RS = 1 */
    uint32_t cmd = xhci_read32(op + XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RS;
    cmd &= ~XHCI_CMD_INTE; /* Ensure interrupts masked in polling mode */
    xhci_write32(op + XHCI_OP_USBCMD, cmd);

    /* 5. Wait for USBSTS.HCH == 0 */
    int rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_HCH, false, op);
    if (rc != 0) {
        log_err("[xhci] start error: controller did not run (%d)\n", rc);
        return rc;
    }

    ctrl->running = true;
    return 0;
}

int xhci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) return -EINVAL;

    /* 1. Enable Bus Master & MMIO decode */
    int rc = pci_set_bus_master(pdev, true);
    if (rc != 0) {
        log_err("[xhci] failed to enable PCI bus master (%d)\n", rc);
        return rc;
    }

    rc = pci_set_decode(pdev, false, true);
    if (rc != 0) {
        log_err("[xhci] failed to enable PCI memory decode (%d)\n", rc);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 2. Query BAR0 window (try MMIO32, fallback to MMIO64) */
    uint64_t mmio_phys = 0;
    rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO32, 0, 0x1000, &mmio_phys);
    if (rc != 0) {
        rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO64, 0, 0x1000, &mmio_phys);
    }
    if (rc != 0) {
        log_err("[xhci] failed to query BAR0 MMIO window (%d)\n", rc);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    struct xhci_controller *ctrl = (struct xhci_controller *)kmalloc(sizeof(struct xhci_controller));
    if (!ctrl) {
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return -ENOMEM;
    }
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->pdev = pdev;
    ctrl->mmio_phys = mmio_phys;
    ctrl->mmio_len = 0x1000;

#ifndef OS01_HOST_TEST
    uint64_t bar_page = mmio_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
    flush_tlb();
#endif

    ctrl->mmio_virt = (volatile uint8_t *)Phy_To_Virt(mmio_phys);

    /* 3. Parse capabilities */
    rc = xhci_parse_capabilities(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 4. Reset controller */
    rc = xhci_reset(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 5. Allocate DMA rings */
    rc = xhci_alloc_dma(ctrl);
    if (rc != 0) {
        xhci_free_dma(ctrl);
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 6. Setup Interrupter 0 */
    xhci_setup_interrupter0(ctrl);

    /* 7. Start controller */
    rc = xhci_start(ctrl);
    if (rc != 0) {
        xhci_free_dma(ctrl);
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    pdev->driver_data = ctrl;
    log_info("[xhci] controller initialized and running (slots=%u, ports=%u)\n",
             ctrl->max_slots, ctrl->max_ports);

    return 0;
}

void xhci_remove(struct pci_device *pdev)
{
    if (!pdev || !pdev->driver_data) return;

    struct xhci_controller *ctrl = (struct xhci_controller *)pdev->driver_data;

    /* Stop controller */
    if (ctrl->op_regs) {
        uint32_t cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
        xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        xhci_wait_bit(ctrl->op_regs + XHCI_OP_USBSTS, XHCI_STS_HCH, true, ctrl->op_regs);
    }
    ctrl->running = false;

    /* Free all DMA buffers */
    xhci_free_dma(ctrl);

    /* Restore PCI configuration */
    pci_set_decode(pdev, false, false);
    pci_set_bus_master(pdev, false);

    kfree(ctrl);
    pdev->driver_data = NULL;
}

static const struct pci_device_id xhci_ids[] = {
    {
        .vendor = PCI_ID_ANY,
        .device = PCI_ID_ANY,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (0x0C << 16) | (0x03 << 8) | 0x30,
        .class_mask  = 0xFFFFFF,
    },
};

const struct pci_driver xhci_pci_driver = {
    .name     = "xhci",
    .id_table = xhci_ids,
    .id_count = sizeof(xhci_ids) / sizeof(xhci_ids[0]),
    .probe    = xhci_probe,
    .remove   = xhci_remove,
};

PCI_DRIVER_DECLARE(xhci_pci_driver);

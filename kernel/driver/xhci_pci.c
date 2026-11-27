/* kernel/driver/xhci_pci.c — PCI frontend for xHCI Host Controller Driver */
#include <driver/xhci.h>
#include <arch/cpu.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <errno.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/slab.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) && !defined(OS01_HOST_TEST)
#include <arch/x86_64/vmm.h>
#include <arch/x86_64/pte.h>
#include <memory/vmm.h>
#endif

#ifdef OS01_HOST_TEST
__attribute__((weak)) void xhci_hid_detach_controller(struct xhci_controller *ctrl)
{
    (void)ctrl;
}
#endif

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

    mutex_init(&ctrl->io_lock);
    ctrl->io_lock_init = true;

    ctrl->pdev = pdev;
    ctrl->mmio_phys = mmio_phys;
    snprintf(ctrl->loc_desc, sizeof(ctrl->loc_desc), "bdf=%02x:%02x.%u",
             pdev->bus, pdev->slot, pdev->fn);

#if defined(__x86_64__) && !defined(OS01_HOST_TEST)
    uint64_t bar_page = mmio_phys & PAGE_2M_MASK;
    x86_vmm_map_2m(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
    flush_tlb();
    ctrl->mmio_len = (bar_page + 0x200000ULL) - mmio_phys;
#else
    ctrl->mmio_len = 0x10000;
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

    /* 3b. Claim BIOS/OS legacy ownership before HCRST */
    rc = xhci_claim_legacy(ctrl);
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

    /* 6-7. Program rings & Interrupter 0, then start controller */
    rc = xhci_start(ctrl);
    if (rc != 0) {
        if (!ctrl->dma_retained) {
            xhci_free_dma(ctrl);
            kfree(ctrl);
        }
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    pdev->driver_data = ctrl;
    if (xhci_publish_controller(ctrl) != 0) {
        log_warn("[xhci] controller limit reached\n");
    }
    log_info("[xhci] controller %s initialized and running (slots=%u, ports=%u, csz=%u)\n",
             xhci_ctrl_loc(ctrl), ctrl->max_slots, ctrl->max_ports, (unsigned)ctrl->csz);

    xhci_probe_ports(ctrl);

    return 0;
}

void xhci_remove(struct pci_device *pdev)
{
    if (!pdev || !pdev->driver_data) return;

    struct xhci_controller *ctrl = (struct xhci_controller *)pdev->driver_data;

#if defined(__x86_64__) || defined(OS01_HOST_TEST)
    /* Detach any bound USB HID service state before halting/freeing controller. */
    xhci_hid_detach_controller(ctrl);
#endif

    /* Safely stop controller and unregister from global tracking */
    xhci_unpublish_controller(ctrl);

    /* Tear down any enumerated device slots */
    for (uint8_t s = 1; s <= XHCI_MAX_MANAGED_SLOTS; s++) {
        xhci_free_slot(ctrl, s);
    }

    /* Free all DMA buffers (no-op if halt_ok is false and dma_retained is true) */
    xhci_free_dma(ctrl);

    /* Restore PCI configuration */
    pci_set_decode(pdev, false, false);
    pci_set_bus_master(pdev, false);

    if (ctrl->halted_confirmed) {
        kfree(ctrl);
    }
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

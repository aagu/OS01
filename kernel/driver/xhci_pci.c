/* kernel/driver/xhci_pci.c — PCI frontend for xHCI Host Controller Driver */
#include <driver/xhci.h>
#include <arch/cpu.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <device/device.h>
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

#if defined(__aarch64__) || defined(OS01_HOST_TEST)
#include <arch/aarch64/pcie.h>
#endif

#ifdef OS01_HOST_TEST
__attribute__((weak)) bool aarch64_pcie_xhci_bar0_ready(const struct pci_device *pdev)
{
    (void)pdev;
    return true;
}
__attribute__((weak)) void xhci_hid_detach_controller(struct xhci_controller *ctrl)
{
    (void)ctrl;
}
#endif

static void xhci_pci_write_cmd16(struct pci_device *pdev, uint16_t cmd16)
{
    /* Write 16-bit PCI Command with upper 16-bit Status W1C bits zeroed. */
    (void)pci_config_write32(pdev, 0x04, (uint32_t)cmd16 & 0xFFFFU);
}

static void xhci_pci_disable_bme_confirm(struct pci_device *pdev, uint16_t cmd_no_bme)
{
    uint32_t rb = 0;
    xhci_pci_write_cmd16(pdev, (uint16_t)(cmd_no_bme & ~0x0004U));
    (void)pci_config_read32(pdev, 0x04, &rb);
}

int xhci_pci_probe_aarch64(struct pci_device *pdev)
{
    if (!pdev)
        return -EINVAL;
    if (pdev->vendor != 0x1b36U || pdev->device != 0x000dU ||
        pdev->class_code != 0x0c0330U) {
        return -ENODEV;
    }

#if defined(__aarch64__) || defined(OS01_HOST_TEST)
    if (!aarch64_pcie_xhci_bar0_ready(pdev)) {
        log_warn("[xhci] aarch64 BAR0 not pre-authorized for %02x:%02x.%u\n",
                 pdev->bus, pdev->slot, pdev->fn);
        return -ENODEV;
    }
#endif

    uint32_t orig_cmd_sts = 0;
    int rc = pci_config_read32(pdev, 0x04, &orig_cmd_sts);
    if (rc != 0)
        return rc;
    uint16_t orig_cmd = (uint16_t)(orig_cmd_sts & 0xFFFFU);
    uint16_t cmd_mem = (uint16_t)((orig_cmd & ~0x0004U) | 0x0002U);
    uint16_t safe_restore = (uint16_t)(orig_cmd & ~0x0004U);

    /* 1. Disable Bus Master, enable Memory Space decode. */
    xhci_pci_write_cmd16(pdev, cmd_mem);

    /* 2. Map exact 16 KiB BAR0 window. */
    void *mmio_virt = NULL;
    rc = pci_iomap_range(pdev, 0, 0, 0x4000ULL, &mmio_virt);
    if (rc != 0 || !mmio_virt) {
        log_err("[xhci] aarch64 BAR0 iomap failed (%d)\n", rc);
        xhci_pci_write_cmd16(pdev, safe_restore);
        return rc != 0 ? rc : -EIO;
    }

    struct xhci_controller *ctrl = (struct xhci_controller *)kmalloc(sizeof(*ctrl));
    if (!ctrl) {
        xhci_pci_write_cmd16(pdev, safe_restore);
        return -ENOMEM;
    }
    memset(ctrl, 0, sizeof(*ctrl));
    mutex_init(&ctrl->io_lock);
    ctrl->io_lock_init = true;
    ctrl->pdev = pdev;
    ctrl->mmio_phys = pdev->bars[0].address;
    ctrl->mmio_len = 0x4000ULL;
    ctrl->mmio_virt = (volatile uint8_t *)mmio_virt;
    snprintf(ctrl->loc_desc, sizeof(ctrl->loc_desc), "bdf=%02x:%02x.%u",
             pdev->bus, pdev->slot, pdev->fn);

    /* 3. Parse and validate capabilities & aperture spans. */
    rc = xhci_parse_capabilities(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        xhci_pci_write_cmd16(pdev, safe_restore);
        return rc;
    }

    /* 4. Confirm firmware controller is halted before reset. */
    uint32_t usbcmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
    uint32_t usbsts = xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS);
    if ((usbcmd & XHCI_CMD_RS) != 0U || (usbsts & XHCI_STS_HCH) == 0U) {
        log_err("[xhci] %s firmware controller still running (cmd=0x%x sts=0x%x)\n",
                xhci_ctrl_loc(ctrl), usbcmd, usbsts);
        (void)xhci_stop(ctrl);
        xhci_pci_disable_bme_confirm(pdev, cmd_mem);
        if (!ctrl->halted_confirmed) {
            ctrl->poisoned = true;
            ctrl->dma_retained = true;
            return DEVICE_UNSAFE;
        }
        kfree(ctrl);
        return DEVICE_UNSAFE;
    }

    /* 5. Reset controller (checks CNR=0). */
    rc = xhci_reset(ctrl);
    if (rc != 0) {
        xhci_pci_disable_bme_confirm(pdev, cmd_mem);
        if (!ctrl->halted_confirmed) {
            ctrl->poisoned = true;
            ctrl->dma_retained = true;
            return DEVICE_UNSAFE;
        }
        kfree(ctrl);
        xhci_pci_write_cmd16(pdev, safe_restore);
        return rc;
    }

    /* 6. Allocate DMA rings, enable BME (RS is still 0), and program registers. */
    rc = xhci_alloc_dma(ctrl);
    if (rc != 0) {
        xhci_free_dma(ctrl);
        kfree(ctrl);
        xhci_pci_write_cmd16(pdev, safe_restore);
        return rc;
    }

    xhci_pci_write_cmd16(pdev, (uint16_t)(cmd_mem | 0x0004U));
    rc = xhci_program_rings(ctrl);
    if (rc != 0) {
        xhci_pci_disable_bme_confirm(pdev, cmd_mem);
        ctrl->halted_confirmed = true;
        xhci_free_dma(ctrl);
        kfree(ctrl);
        xhci_pci_write_cmd16(pdev, safe_restore);
        return rc;
    }

    /* 7. Set USBCMD.RS=1. */
    rc = xhci_run(ctrl);
    if (rc != 0) {
        xhci_pci_disable_bme_confirm(pdev, cmd_mem);
        if (!ctrl->halted_confirmed || ctrl->dma_retained) {
            return DEVICE_UNSAFE;
        }
        xhci_free_dma(ctrl);
        kfree(ctrl);
        return rc;
    }

    pdev->driver_data = ctrl;
    if (xhci_publish_controller(ctrl) != 0) {
        log_warn("[xhci] controller limit reached\n");
    }
    log_info("[xhci] controller %s initialized and running (slots=%u, ports=%u, csz=%u)\n",
             xhci_ctrl_loc(ctrl), ctrl->max_slots, ctrl->max_ports, (unsigned)ctrl->csz);

    xhci_probe_ports(ctrl);
    if (ctrl->poisoned) {
        xhci_pci_disable_bme_confirm(pdev, cmd_mem);
        return DEVICE_UNSAFE;
    }

    return 0;
}

int xhci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) return -EINVAL;

#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    return xhci_pci_probe_aarch64(pdev);
#else
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
#endif
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
#if defined(__aarch64__) || defined(OS01_HOST_TEST)
    {
        uint32_t cmd_sts = 0;
        if (pci_config_read32(pdev, 0x04, &cmd_sts) == 0) {
            xhci_pci_write_cmd16(pdev, (uint16_t)(cmd_sts & ~0x0006U));
        }
    }
#else
    pci_set_decode(pdev, false, false);
    pci_set_bus_master(pdev, false);
#endif

    if (ctrl->halted_confirmed) {
        kfree(ctrl);
    }
    pdev->driver_data = NULL;
}

static const struct pci_device_id xhci_ids[] = {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    {
        .vendor = 0x1b36U,
        .device = 0x000dU,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (0x0C << 16) | (0x03 << 8) | 0x30,
        .class_mask  = 0xFFFFFF,
    },
#else
    {
        .vendor = PCI_ID_ANY,
        .device = PCI_ID_ANY,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (0x0C << 16) | (0x03 << 8) | 0x30,
        .class_mask  = 0xFFFFFF,
    },
#endif
};

const struct pci_driver xhci_pci_driver = {
    .name     = "xhci",
    .id_table = xhci_ids,
    .id_count = sizeof(xhci_ids) / sizeof(xhci_ids[0]),
    .probe    = xhci_probe,
    .remove   = xhci_remove,
};

PCI_DRIVER_DECLARE(xhci_pci_driver);

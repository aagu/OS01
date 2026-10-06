// kernel/driver/e1000.c — Intel 82540EM (e1000) NIC driver
#include <driver/e1000.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <net/device.h>
#include <device/test_fault.h>
#include <ipc/mbox.h>
#include <memory/vmm.h>       // vmm_map_page, kernel_map
#include <arch/x86_64/pte.h>  // PAGE_KERNEL_PMD_NOCACHE
#include <memory/pmm.h>       // PAGE_2M_MASK, alloc_pages, alloc_4k_page, free_pages, free_4k_page
#include <memory/memory.h>    // Phy_To_Virt
#include <intr/interrupt.h> // register_irq, unregister_irq
#include <arch/spinlock.h>
#include <arch/barrier.h>
#include <log/log.h>
#include <memory/slab.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef OS01_HOST_TEST
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#endif

// ── Static legacy instance pointer (for transitional compatibility) ────
static struct e1000_instance *s_legacy_instance = NULL;

// ── MMIO helpers ───────────────────────────────────────────────
static inline uint32_t e1000_read(struct e1000_instance *inst, uint32_t reg)
{
    return *(volatile uint32_t *)(inst->mmio + reg);
}

static inline void e1000_write(struct e1000_instance *inst, uint32_t reg, uint32_t val)
{
#ifdef OS01_HOST_TEST
    fake_mmio_write_count++;
    if (reg == E1000_REG_CTRL && (val & E1000_CTRL_RST)) {
        val &= ~E1000_CTRL_RST;
    }
    extern void test_e1000_mmio_write_hook(void *inst, uint32_t reg, uint32_t val);
    test_e1000_mmio_write_hook(inst, reg, val);
#endif
    *(volatile uint32_t *)(inst->mmio + reg) = val;
}

// ── EEPROM read (MAC address) ─────────────────────────────────
static int e1000_eeprom_read(struct e1000_instance *inst, uint8_t addr, uint16_t *out)
{
    e1000_write(inst, E1000_REG_EERD, ((uint32_t)addr << 8) | E1000_EERD_START);
    int max_loops = 100000;
#ifdef OS01_HOST_TEST
    max_loops = 10;
#endif
    for (int i = 0; i < max_loops; i++) {
        if (e1000_read(inst, E1000_REG_EERD) & E1000_EERD_DONE) {
            *out = (uint16_t)(e1000_read(inst, E1000_REG_EERD) >> E1000_EERD_DATA_SHIFT);
            return 0;
        }
    }
    return -1;
}

// ── Cleanup DMA buffers & descriptor rings ─────────────────────
static void e1000_free_dma(struct e1000_instance *inst)
{
    if (!inst) return;
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        if (inst->rx_buf_phys[i]) {
            free_4k_page(inst->rx_buf_phys[i]);
            inst->rx_buf_phys[i] = 0;
            inst->rx_bufs[i] = NULL;
        }
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        if (inst->tx_buf_phys[i]) {
            free_4k_page(inst->tx_buf_phys[i]);
            inst->tx_buf_phys[i] = 0;
            inst->tx_bufs[i] = NULL;
        }
    }
    if (inst->rx_page) {
        free_pages(inst->rx_page, 1);
        inst->rx_page = NULL;
        inst->rx_descs = NULL;
    }
    if (inst->tx_page) {
        free_pages(inst->tx_page, 1);
        inst->tx_page = NULL;
        inst->tx_descs = NULL;
    }
}

// ── Hardware Setup ─────────────────────────────────────────────
static int e1000_setup_hw(struct e1000_instance *inst)
{
    // 1. Reset device
    uint32_t ctrl = e1000_read(inst, E1000_REG_CTRL);
    e1000_write(inst, E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) {
        if (!(e1000_read(inst, E1000_REG_CTRL) & E1000_CTRL_RST))
            break;
    }

    // 2. Read MAC — try EEPROM first, fall back to RAL/RAH registers.
    int eep_ok = 1;
    for (int i = 0; i < 3; i++) {
        uint16_t eep_word;
        if (e1000_eeprom_read(inst, (uint8_t)i, &eep_word) != 0) {
            eep_ok = 0;
            break;
        }
        inst->mac[i * 2]     = (uint8_t)(eep_word & 0xFF);
        inst->mac[i * 2 + 1] = (uint8_t)(eep_word >> 8);
    }
    if (!eep_ok) {
        uint32_t ral = e1000_read(inst, E1000_REG_RAL0);
        uint32_t rah = e1000_read(inst, E1000_REG_RAH0);
        inst->mac[0] = (uint8_t)(ral & 0xFF);
        inst->mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        inst->mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        inst->mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        inst->mac[4] = (uint8_t)(rah & 0xFF);
        inst->mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    }

    // 3. Allocate descriptor rings (physically contiguous)
    struct Page *rx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    struct Page *tx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!rx_page || !tx_page) {
        if (rx_page) free_pages(rx_page, 1);
        if (tx_page) free_pages(tx_page, 1);
        return -ENOMEM;
    }

    inst->rx_page = rx_page;
    inst->tx_page = tx_page;
    inst->rx_phys = rx_page->phy_address;
    inst->tx_phys = tx_page->phy_address;
    inst->rx_descs = (e1000_rx_desc_t *)Phy_To_Virt(inst->rx_phys);
    inst->tx_descs = (e1000_tx_desc_t *)Phy_To_Virt(inst->tx_phys);
    memset(inst->rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    memset(inst->tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

    // 4. Allocate DMA buffers for RX/TX descriptors using 4k physical pages
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -ENOMEM;
        inst->rx_buf_phys[i] = buf_phys;
        inst->rx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        inst->rx_descs[i].addr = buf_phys;
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -ENOMEM;
        inst->tx_buf_phys[i] = buf_phys;
        inst->tx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        inst->tx_descs[i].addr = buf_phys;
    }

#ifndef OS01_HOST_TEST
    // 5. Trigger PHY auto-negotiation via MDIC — REQUIRED for QEMU RX.
    {
        uint32_t bmcr = (1u << 9) | (1u << 12) | (1u << 8) | (1u << 6);
        e1000_write(inst, 0x0020, bmcr | (1u << 21) | (0u << 16) | (0u << 26));
        for (int m = 0; m < 1000; m++) {
            if (e1000_read(inst, 0x0020) & (1u << 28))
                break;
            for (volatile int d = 0; d < 1000; d++)
                __asm__ volatile("pause");
        }
    }
#endif

    // 6. Configure RX
    e1000_write(inst, E1000_REG_RDBAL, (uint32_t)(inst->rx_phys & 0xFFFFFFFF));
    e1000_write(inst, E1000_REG_RDBAH, (uint32_t)(inst->rx_phys >> 32));
    e1000_write(inst, E1000_REG_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(inst, E1000_REG_RDH, 0);
    e1000_write(inst, E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    inst->rx_tail = 0;
    e1000_write(inst, E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM
        | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    // 7. Configure TX
    e1000_write(inst, E1000_REG_TDBAL, (uint32_t)(inst->tx_phys & 0xFFFFFFFF));
    e1000_write(inst, E1000_REG_TDBAH, (uint32_t)(inst->tx_phys >> 32));
    e1000_write(inst, E1000_REG_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write(inst, E1000_REG_TDH, 0);
    e1000_write(inst, E1000_REG_TDT, 0);
    inst->tx_head = 0;
    inst->tx_tail = 0;
    spin_init(&inst->tx_lock);
    e1000_write(inst, E1000_REG_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP
        | (0x10 << E1000_TCTL_CT_SHIFT)
        | (E1000_TCTL_COLD_FULLDUPLEX << E1000_TCTL_COLD_SHIFT));

    return 0;
}

// ── Transmit helper ────────────────────────────────────────────
static int e1000_instance_xmit(struct e1000_instance *inst, struct pbuf *p)
{
    if (!inst || !inst->initialized || inst->stopped || !p) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&inst->tx_lock);

    while (inst->tx_tail != inst->tx_head &&
           (inst->tx_descs[inst->tx_tail].status & E1000_TXD_STAT_DD)) {
        inst->tx_descs[inst->tx_tail].status = 0;
        inst->tx_tail = (inst->tx_tail + 1) % E1000_NUM_TX_DESC;
    }

    uint32_t next = (inst->tx_head + 1) % E1000_NUM_TX_DESC;
    if (next == inst->tx_tail) {
        spin_unlock_irqrestore(&inst->tx_lock, flags);
        return -EBUSY;
    }

    uint8_t *dst = inst->tx_bufs[inst->tx_head];
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(dst + total, q->payload, q->len);
        total += (uint16_t)q->len;
    }

    inst->tx_descs[inst->tx_head].length = total;
    inst->tx_descs[inst->tx_head].cmd = E1000_TXD_CMD_EOP
                                      | E1000_TXD_CMD_IFCS
                                      | E1000_TXD_CMD_RS;
    inst->tx_descs[inst->tx_head].status = 0;

    inst->tx_head = next;
    arch_wmb();
    e1000_write(inst, E1000_REG_TDT, inst->tx_head);

    spin_unlock_irqrestore(&inst->tx_lock, flags);
    return 0;
}

// ── Interrupt handler ──────────────────────────────────────────
void e1000_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)regs;
    struct e1000_instance *inst = (struct e1000_instance *)(uintptr_t)param;
    if (!inst || !inst->initialized || inst->stopped) return;

    uint32_t icr = e1000_read(inst, E1000_REG_ICR);
    if (!icr) return;

    // RX: descriptor done — ack and wake only, sole consumer is poll_rx
    if (icr & (E1000_ICR_RXQ0 | E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        sys_mbox_wake();
    }

    // TX: descriptor done
    if (icr & (E1000_ICR_TXQ0 | E1000_ICR_TXDW)) {
        uint64_t flags = spin_lock_irqsave(&inst->tx_lock);
        while (inst->tx_tail != inst->tx_head) {
            if (!(inst->tx_descs[inst->tx_tail].status & E1000_TXD_STAT_DD))
                break;
            inst->tx_descs[inst->tx_tail].status = 0;
            inst->tx_tail = (inst->tx_tail + 1) % E1000_NUM_TX_DESC;
        }
        spin_unlock_irqrestore(&inst->tx_lock, flags);
    }
}

// ── Net Device Operations (for new e1000_probe instances) ─────
static int e1000_ndev_xmit(struct net_device *dev, struct pbuf *p)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct e1000_instance *inst = (struct e1000_instance *)dev->priv;
    return e1000_instance_xmit(inst, p);
}

static unsigned e1000_ndev_poll_rx(struct net_device *dev, unsigned budget)
{
    if (!dev || !dev->priv) return 0;
    struct e1000_instance *inst = (struct e1000_instance *)dev->priv;
    if (!inst->initialized || inst->stopped) return 0;

    unsigned count = 0;
    while (count < budget && (inst->rx_descs[inst->rx_tail].status & E1000_RXD_STAT_DD)) {
        uint32_t i = inst->rx_tail;
        uint16_t len = inst->rx_descs[i].length;

        if (len > 0 && len < 1600) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
            if (p) {
                pbuf_take(p, inst->rx_bufs[i], len);
                if (net_receive(dev, p) != 0) {
                    pbuf_free(p);
                }
            }
        }

        inst->rx_descs[i].status = 0;
        arch_wmb();
        e1000_write(inst, E1000_REG_RDT, i);
        inst->rx_tail = (i + 1) % E1000_NUM_RX_DESC;
        count++;
    }
    return count;
}

static bool e1000_ndev_get_link(struct net_device *dev)
{
    if (!dev || !dev->priv) return false;
    struct e1000_instance *inst = (struct e1000_instance *)dev->priv;
    if (!inst->initialized || inst->stopped) return false;
    return (e1000_read(inst, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
}

static int e1000_ndev_stop(struct net_device *dev)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct e1000_instance *inst = (struct e1000_instance *)dev->priv;
    inst->stopped = true;
    e1000_write(inst, E1000_REG_IMC, 0xFFFFFFFF);
    e1000_write(inst, E1000_REG_RCTL, 0);
    e1000_write(inst, E1000_REG_TCTL, 0);
    return 0;
}

static const struct net_device_ops e1000_net_ops = {
    .xmit = e1000_ndev_xmit,
    .poll_rx = e1000_ndev_poll_rx,
    .get_link = e1000_ndev_get_link,
    .stop = e1000_ndev_stop,
};

// ── Driver probe ──────────────────────────────────────────────
int e1000_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    if (!pdev) return -EINVAL;

    uint16_t e1000_bdf = arch9_obs_bdf_encode(pdev->bus, pdev->slot, pdev->fn);

    if (pdev->vendor != 0x8086 || pdev->device != 0x100e) {
        arch9_fault_on_probe_unbound_after_id();
        return -ENODEV;
    }

    arch9_fault_on_probe_begin_bdf(e1000_bdf, "e1000");

    if (id) {
        if (id->vendor != 0x8086 || id->device != 0x100e) {
            arch9_fault_on_probe_unbound_after_id();
            return -ENODEV;
        }
    } else {
        if (pci_match_id(&e1000_pci_driver, pdev) == NULL) {
            arch9_fault_on_probe_unbound_after_id();
            return -ENODEV;
        }
    }

    /* bad-nic-bar fault: corrupt the first NIC's BAR window after
     * id-match but before any hardware modification. The probe
     * returns -ENODEV; the kernel's ahci driver still brings up the
     * root disk.  Counted as bar_writes because the write happens
     * via the same pci_config_write8 that real hw probing would
     * touch. */
    static unsigned s_e1000_probe_index = 0;
    unsigned my_index = s_e1000_probe_index++;
    if (arch9_fault_should_inject_bad_nic_bar(my_index)) {
        log_warn("e1000: bad-nic-bar fault: corrupting BAR0 "
                  "for probe #%u\n", my_index);
        /* A single byte-write to BAR0 registers as a "bar_write"
         * under the observation counter; the next probe() aborts
         * because the BAR window is now unusable. */
        uint32_t val = 0;
        if (pci_config_read32(pdev, 0x10, &val) == 0) {
            arch9_fault_on_bar_write_bdf(e1000_bdf);
            (void)pci_config_write32(pdev, 0x10, 0xFFFFFFFFu);
        }
        arch9_fault_on_probe_unbound_no_match_bdf(e1000_bdf);
        return -ENODEV;
    }

    int rc = pci_set_bus_master(pdev, true);
    if (rc != 0) return rc;

    rc = pci_set_decode(pdev, false, true);
    if (rc != 0) {
        pci_set_bus_master(pdev, false);
        return rc;
    }

    uint64_t mmio_phys = 0;
    rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO32, 0, 0x20000, &mmio_phys);
    if (rc != 0) {
        rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO64, 0, 0x20000, &mmio_phys);
    }
    if (rc != 0 && pdev->bars[0].valid) {
        mmio_phys = pdev->bars[0].address;
        rc = 0;
    }
    if (rc != 0 || !mmio_phys) {
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc != 0 ? rc : -ENODEV;
    }

    struct e1000_instance *inst = kmalloc(sizeof(struct e1000_instance));
    if (!inst) {
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return -ENOMEM;
    }
    memset(inst, 0, sizeof(*inst));

    inst->pdev = pdev;
    inst->legacy_mode = false;
    inst->mmio_phys = mmio_phys;

#ifndef OS01_HOST_TEST
    uint64_t bar_page = mmio_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
#endif
    inst->mmio = (volatile uint8_t *)Phy_To_Virt(mmio_phys);

    rc = e1000_setup_hw(inst);
    if (rc != 0) {
        e1000_free_dma(inst);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        kfree(inst);
        return rc;
    }

    pdev->driver_data = inst;

    // Interrupt mode selection:
    // 1. Try MSI-X on vector 0x30 / GSI 16
    bool msix_ok = (pci_msix_enable(pdev, 0x30) == 0);
    if (msix_ok) {
        uint32_t gsi_to_register = 16;
        /* ARCH-9 Task 11: irq-conflict fault.  When this is the
         * SECOND NIC probe, redirect its candidate GSI to the
         * first card's already-held slot so the natural
         * register_irq "already occupied" rejection path runs.
         * register_irq returns 0 in that case; the existing
         * failure path falls through to INTx / POLL. */
        if (arch9_fault_should_force_irq_conflict() &&
            arch9_fault_get_nic_probe_count() >= 1) {
            gsi_to_register = arch9_fault_get_first_card_gsi();
            log_info("e1000: irq-conflict fault: redirecting GSI %u to first-card slot %u\n",
                      16, gsi_to_register);
        }
        int irq_res = register_irq(gsi_to_register, NULL, &e1000_handler,
                                   (uint64_t)(uintptr_t)inst,
                                   IRQF_TRIGGER_LEVEL, "e1000");
        if (irq_res == 1) {
            inst->irq_mode = NIC_MSIX;
            inst->gsi = gsi_to_register;
            inst->irq_owned = true;
            arch9_fault_record_card_gsi(gsi_to_register);
        } else {
            // MSI-X enable succeeded on PCI device but GSI 16 is occupied;
            // disable MSI-X and restore INTx before falling back.
            pci_interrupts_disable(pdev);
            pci_set_intx(pdev, true);
        }
    }

    // 2. If GSI 16 occupied or MSI-X failed, try INTx
    if (!inst->irq_owned) {
        uint32_t intx_gsi = 0;
        if (pci_route_gsi(pdev, &intx_gsi) == 0) {
            uint32_t gsi_to_register = intx_gsi;
            /* IRQ-conflict fault: same redirect for the INTx
             * fallback.  The fault check runs unconditionally so
             * mixed-topology (e1000 + virtio-net) — where the
             * natural pci_route_gsi would return a *different*
             * GSI than the first card's — still triggers the
             * "already owned" rejection path that the brief
             * mandates ("走真实已占槽拒绝路径"). */
            if (arch9_fault_should_force_irq_conflict() &&
                arch9_fault_get_nic_probe_count() >= 1) {
                gsi_to_register = arch9_fault_get_first_card_gsi();
                log_info("e1000: irq-conflict fault: redirecting INTX GSI %u to first-card slot %u\n",
                          intx_gsi, gsi_to_register);
            }
            int irq_res = register_irq(gsi_to_register, NULL, &e1000_handler,
                                       (uint64_t)(uintptr_t)inst,
                                       IRQF_TRIGGER_LEVEL, "e1000");
            if (irq_res == 1) {
                inst->irq_mode = NIC_INTX;
                inst->gsi = gsi_to_register;
                inst->irq_owned = true;
                arch9_fault_record_card_gsi(gsi_to_register);
            }
        }
    }

    // 3. Fallback to POLL if both occupied / unavailable
    if (!inst->irq_owned) {
        inst->irq_mode = NIC_POLL;
        inst->gsi = 0;
        inst->irq_owned = false;
        e1000_write(inst, E1000_REG_IMC, 0xFFFFFFFF);
        pci_set_intx(pdev, false);
    } else {
        // Enable interrupt sources on device AFTER handler/driver_data are ready
        if (inst->irq_mode == NIC_MSIX) {
            e1000_write(inst, E1000_REG_IVAR,
                        (0x8 | 0) | ((0x8 | 0) << 4) | ((0x8 | 0) << 8) |
                        ((0x8 | 0) << 12) | ((0x8 | 0) << 16));
            e1000_write(inst, E1000_REG_IMS,
                        E1000_ICR_RXQ0 | E1000_ICR_TXQ0 | E1000_ICR_OTHER |
                        E1000_ICR_LSC | E1000_ICR_RXDMT0);
        } else {
            e1000_write(inst, E1000_REG_IMS,
                        E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_TXDW | E1000_ICR_LSC);
        }
    }

    // Register net_device
    struct net_device *ndev = kmalloc(sizeof(struct net_device));
    if (!ndev) {
        rc = -ENOMEM;
        goto err_unwind_irq;
    }
    memset(ndev, 0, sizeof(*ndev));
    snprintf(ndev->name, sizeof(ndev->name), "eth%u", net_device_count());
    memcpy(ndev->mac, inst->mac, 6);
    ndev->mtu = 1500;
    ndev->link_up = true;
    ndev->parent = &pdev->dev;
    ndev->ops = &e1000_net_ops;
    ndev->priv = inst;

    /* ARCH-9 Task 9: mark initialized BEFORE net_device_register so
     * that get_link() observes `initialized == true` during the
     * register-time link query.  Previously this flag was set after
     * register, which caused net_device_register's get_link() to
     * return false (link down) and lwIP's DHCP to never start. */
    inst->initialized = 1;

    /* ARCH-9 Task 11: per-BDF observation — attribute the
     * adapter_registrations counter to THIS device's BDF so the
     * matrix can distinguish matched vs unmatched devices.  The
     * aggregate adapter_register hook was removed from
     * net_device_register() because every modern call site has
     * BDF context available here. */
    arch9_fault_on_adapter_register_bdf(e1000_bdf);

#ifdef OS01_TEST_FAULT
    /* ARCH-9 whole-branch final review: matrix's irq-conflict case
     * must verify that eth1 fell back to POLL (and that eth0 did
     * NOT).  The grep target is the same shape as
     * arch9-fault-dev:, so the harness can run per-card regex
     * checks against `arch9-irq-mode: bdf=... nic=ethN mode=POLL`. */
    log_info("arch9-irq-mode: bdf=%04x:%02x:%02x.%d nic=%s mode=%s\n",
              (unsigned)pdev->domain, (unsigned)pdev->bus,
              (unsigned)pdev->slot, (unsigned)pdev->fn,
              ndev->name,
              inst->irq_mode == NIC_MSIX ? "MSIX" :
              inst->irq_mode == NIC_INTX ? "INTX" : "POLL");
#endif

    rc = net_device_register(ndev);
    if (rc != 0) {
        kfree(ndev);
        goto err_unwind_irq;
    }

    inst->ndev = ndev;
    return 0;

err_unwind_irq:
    e1000_write(inst, E1000_REG_IMC, 0xFFFFFFFF);
    e1000_write(inst, E1000_REG_RCTL, 0);
    e1000_write(inst, E1000_REG_TCTL, 0);
    pci_interrupts_disable(pdev);
    if (inst->irq_owned) {
        unregister_irq(inst->gsi);
        inst->irq_owned = false;
    }
    pci_set_bus_master(pdev, false);
    pci_set_decode(pdev, false, false);
    e1000_free_dma(inst);
    pdev->driver_data = NULL;
    kfree(inst);
    return rc;
}

// ── Driver remove ─────────────────────────────────────────────
void e1000_remove(struct pci_device *pdev)
{
    if (!pdev) return;
    struct e1000_instance *inst = (struct e1000_instance *)pdev->driver_data;
    if (!inst) return;

    if (inst == s_legacy_instance) {
        s_legacy_instance = NULL;
    }

    // 1. Stop device & disable interrupt sources
    inst->stopped = true;
    e1000_write(inst, E1000_REG_IMC, 0xFFFFFFFF);
    e1000_write(inst, E1000_REG_RCTL, 0);
    e1000_write(inst, E1000_REG_TCTL, 0);
    pci_interrupts_disable(pdev);

    // 2. Revoke own IRQ
    if (inst->irq_owned) {
        unregister_irq(inst->gsi);
        inst->irq_owned = false;
    }

    // 3. Revoke unconsumed ndev & drain rxq
    if (inst->ndev) {
        net_device_unregister_boot(inst->ndev);
        kfree(inst->ndev);
        inst->ndev = NULL;
    }
    for (int i = 0; i < E1000_RXQ_DEPTH; i++) {
        if (inst->rxq.buf[i]) {
            kfree(inst->rxq.buf[i]);
            inst->rxq.buf[i] = NULL;
        }
    }

    // 4. Disable bus master before freeing DMA memory
    pci_set_bus_master(pdev, false);

    // 5. Free DMA buffers and descriptor rings
    e1000_free_dma(inst);

    // 6. Free private instance if not quarantined
    if (!pdev->dev.quarantined) {
        pdev->driver_data = NULL;
        kfree(inst);
    }
}

// ── Driver declaration ────────────────────────────────────────
static const struct pci_device_id e1000_ids[] = {
    {
        .vendor = 0x8086,
        .device = 0x100e,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = 0,
        .class_mask = 0,
    },
};

const struct pci_driver e1000_pci_driver = {
    .name = "e1000",
    .id_table = e1000_ids,
    .id_count = sizeof(e1000_ids) / sizeof(e1000_ids[0]),
    .probe = e1000_probe,
    .remove = e1000_remove,
};

PCI_DRIVER_DECLARE(e1000_pci_driver);

// ── Legacy transitional APIs ──────────────────────────────────
int e1000_legacy_init(struct pci_device *pdev, uint64_t bar, uint8_t gsi, int use_msi)
{
    if (s_legacy_instance && s_legacy_instance->initialized) return 0;

    struct e1000_instance *inst = kmalloc(sizeof(struct e1000_instance));
    if (!inst) return -ENOMEM;
    memset(inst, 0, sizeof(*inst));

    inst->pdev = pdev;
    inst->legacy_mode = true;
    inst->mmio_phys = bar;

#ifndef OS01_HOST_TEST
    uint64_t bar_page = bar & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
#endif
    inst->mmio = (volatile uint8_t *)Phy_To_Virt(bar);

    int rc = e1000_setup_hw(inst);
    if (rc != 0) {
        e1000_free_dma(inst);
        kfree(inst);
        return rc;
    }

    if (pdev) {
        pdev->driver_data = inst;
    }

    uint8_t reg_gsi = use_msi ? 16 : gsi;
    inst->gsi = reg_gsi;
    inst->irq_mode = use_msi ? NIC_MSIX : NIC_INTX;

    register_irq(reg_gsi, NULL, &e1000_handler, (uint64_t)(uintptr_t)inst,
                 IRQF_TRIGGER_LEVEL, "e1000");
    inst->irq_owned = true;

    if (use_msi) {
        e1000_write(inst, E1000_REG_IVAR,
                    (0x8 | 0) | ((0x8 | 0) << 4) | ((0x8 | 0) << 8) |
                    ((0x8 | 0) << 12) | ((0x8 | 0) << 16));
        e1000_write(inst, E1000_REG_IMS,
            E1000_ICR_RXQ0 | E1000_ICR_TXQ0 | E1000_ICR_OTHER |
            E1000_ICR_LSC | E1000_ICR_RXDMT0);
    } else {
        e1000_write(inst, E1000_REG_IMS,
            E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_TXDW | E1000_ICR_LSC);
    }

    inst->initialized = 1;
    s_legacy_instance = inst;

    log_info("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ=%u%s\n",
                inst->mac[0], inst->mac[1], inst->mac[2],
                inst->mac[3], inst->mac[4], inst->mac[5],
                reg_gsi, use_msi ? " (MSI-X v0x30)" : "");
    return 0;
}

int e1000_init(uint64_t bar_phys, uint8_t gsi, int use_msi)
{
    return e1000_legacy_init(NULL, bar_phys, gsi, use_msi);
}

int e1000_link_up(void)
{
    if (!s_legacy_instance || !s_legacy_instance->initialized) return 0;
    return (e1000_read(s_legacy_instance, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
}

void e1000_poll_rx(void)
{
    if (!s_legacy_instance || !s_legacy_instance->initialized) return;
    struct e1000_instance *inst = s_legacy_instance;

    while (inst->rx_descs[inst->rx_tail].status & E1000_RXD_STAT_DD) {
        uint32_t i = inst->rx_tail;
        uint16_t len = inst->rx_descs[i].length;

        if (len > 0 && len < 1600) {
            int next = (inst->rxq.head + 1) % E1000_RXQ_DEPTH;
            if (next != inst->rxq.tail) {
                uint8_t *buf = (uint8_t *)kmalloc(len);
                if (buf) {
                    memcpy(buf, inst->rx_bufs[i], len);
                    inst->rxq.buf[inst->rxq.head] = buf;
                    inst->rxq.len[inst->rxq.head] = len;
                    inst->rxq.head = next;
                }
            }
        }

        inst->rx_descs[i].status = 0;
        arch_wmb();
        e1000_write(inst, E1000_REG_RDT, i);
        inst->rx_tail = (i + 1) % E1000_NUM_RX_DESC;
    }
}

void e1000_process_rx(void)
{
    if (!s_legacy_instance || !s_legacy_instance->initialized) return;
    struct e1000_instance *inst = s_legacy_instance;

    while (inst->rxq.tail != inst->rxq.head) {
        uint8_t *buf = inst->rxq.buf[inst->rxq.tail];
        uint16_t len = inst->rxq.len[inst->rxq.tail];
        inst->rxq.buf[inst->rxq.tail] = NULL;
        inst->rxq.tail = (inst->rxq.tail + 1) % E1000_RXQ_DEPTH;

        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (p) {
            if (pbuf_take(p, buf, len) != ERR_OK ||
                (inst->netif_ptr && inst->netif_ptr->input(p, inst->netif_ptr) != ERR_OK))
                pbuf_free(p);
        }
        kfree(buf);
    }
}

err_t e1000_xmit(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (!s_legacy_instance) return ERR_IF;
    int rc = e1000_instance_xmit(s_legacy_instance, p);
    return rc == 0 ? ERR_OK : ERR_MEM;
}

static err_t e1000_netif_input(struct pbuf *p, struct netif *n)
{
#ifndef OS01_HOST_TEST
    return ethernet_input(p, n);
#else
    (void)p; (void)n;
    return ERR_OK;
#endif
}

err_t e1000_netif_init(struct netif *netif)
{
    if (!s_legacy_instance) return ERR_IF;
    s_legacy_instance->netif_ptr = netif;
    netif->input = e1000_netif_input;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, s_legacy_instance->mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = e1000_xmit;
#ifndef OS01_HOST_TEST
    netif->output = etharp_output;
#endif
    return ERR_OK;
}

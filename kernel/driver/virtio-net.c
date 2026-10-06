// kernel/driver/virtio-net.c — VirtIO-net driver for OS01
// Uses LEGACY transport via BAR0 IO ports.
// Supports per-device instances, INTx with polling fallback, and safe quarantine.

#include <driver/virtio-net.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <net/device.h>
#include <device/test_fault.h>
#include <ipc/mbox.h>
#include <intr/interrupt.h>
#include <arch/spinlock.h>
#include <arch/barrier.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef OS01_HOST_TEST
#include <lwip/etharp.h>
#include <lwip/pbuf.h>
#endif

// ── Static legacy instance pointer (transitional compatibility) ────────
static struct virtio_net_instance *s_legacy_instance = NULL;

// ── IO port helpers ───────────────────────────────────────────────────
#ifndef OS01_HOST_TEST
static inline uint32_t vio_in32(uint16_t port) { uint32_t v; __asm__("inl %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out32(uint16_t port, uint32_t v) { __asm__("outl %0, %1"::"a"(v),"Nd"(port)); }
static inline uint16_t vio_in16(uint16_t port) { uint16_t v; __asm__("inw %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out16(uint16_t port, uint16_t v) { __asm__("outw %0, %1"::"a"(v),"Nd"(port)); }
static inline uint8_t  vio_in8(uint16_t port)  { uint8_t v; __asm__("inb %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out8(uint16_t port, uint8_t v)  { __asm__("outb %0, %1"::"a"(v),"Nd"(port)); }
#endif

// ── DMA and queue helpers ──────────────────────────────────────────────
static int virtq_init_instance(virtq_t *vq, struct Page **page_out, uint64_t *phys_out, uint16_t qsize)
{
    vq->size = qsize;
    struct Page *pages = alloc_pages(ZONE_NORMAL, 2, 0);
    if (!pages) return -1;

    uint64_t phys = pages->phy_address;
    uint8_t *base = (uint8_t *)Phy_To_Virt(phys);

    uint16_t desc_sz = qsize * 16;
    uint16_t used_off = (desc_sz + 4 + qsize * 2 + 4095) & ~4095;
    memset(base, 0, used_off + 4 + qsize * 8);

    vq->desc = (virtq_desc_t *)base;
    vq->avail = (virtq_avail_t *)(base + desc_sz);
    vq->used = (virtq_used_t *)(base + used_off);
    vq->last_used_idx = 0;

    *page_out = pages;
    *phys_out = phys;
    return 0;
}

static void virtio_net_free_dma(struct virtio_net_instance *inst)
{
    if (!inst) return;

    for (int i = 0; i < VIRTIO_NET_VQ_SIZE; i++) {
        if (inst->rx_buf_phys[i]) {
            free_4k_page(inst->rx_buf_phys[i]);
            inst->rx_buf_phys[i] = 0;
            inst->rx_bufs[i] = NULL;
        }
        if (inst->tx_buf_phys[i]) {
            free_4k_page(inst->tx_buf_phys[i]);
            inst->tx_buf_phys[i] = 0;
        }
    }

    if (inst->rx_page) {
        free_pages(inst->rx_page, 2);
        inst->rx_page = NULL;
        inst->rx_vq.desc = NULL;
    }
    if (inst->tx_page) {
        free_pages(inst->tx_page, 2);
        inst->tx_page = NULL;
        inst->tx_vq.desc = NULL;
    }
}

// ── Hardware setup ─────────────────────────────────────────────────────
static int virtio_net_setup_hw(struct virtio_net_instance *inst)
{
    uint16_t io = inst->io_base;

    // 1. Reset device
    vio_out8(io + VIRTIO_LEGACY_DEVICE_STATUS, 0);

    // 2. Set ACKNOWLEDGE and DRIVER
    vio_out8(io + VIRTIO_LEGACY_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vio_out8(io + VIRTIO_LEGACY_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // 3. Negotiate features
    uint32_t host_feat = vio_in32(io + VIRTIO_LEGACY_HOST_FEATURES);
    if (!(host_feat & VIRTIO_NET_F_MAC)) {
        return -EIO;
    }
    uint32_t guest_feat = host_feat & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
    vio_out32(io + VIRTIO_LEGACY_GUEST_FEATURES, guest_feat);
    vio_out8(io + VIRTIO_LEGACY_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(vio_in8(io + VIRTIO_LEGACY_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        return -EIO;
    }

    // 4. Setup RX queue (0)
    vio_out16(io + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_RX_QUEUE);
    uint16_t rx_qsize = vio_in16(io + VIRTIO_LEGACY_QUEUE_SIZE);
    if (rx_qsize == 0) return -EIO;
    if (rx_qsize > VIRTIO_NET_VQ_SIZE) rx_qsize = VIRTIO_NET_VQ_SIZE;
    inst->rx_qsize = rx_qsize;

    if (virtq_init_instance(&inst->rx_vq, &inst->rx_page, &inst->rx_phys, rx_qsize) != 0) {
        return -ENOMEM;
    }
    vio_out16(io + VIRTIO_LEGACY_QUEUE_SIZE, rx_qsize);
    vio_out32(io + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)(inst->rx_phys >> 12));

    // 5. Setup TX queue (1)
    vio_out16(io + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_TX_QUEUE);
    uint16_t tx_qsize = vio_in16(io + VIRTIO_LEGACY_QUEUE_SIZE);
    if (tx_qsize == 0) return -EIO;
    if (tx_qsize > VIRTIO_NET_VQ_SIZE) tx_qsize = VIRTIO_NET_VQ_SIZE;
    inst->tx_qsize = tx_qsize;

    if (virtq_init_instance(&inst->tx_vq, &inst->tx_page, &inst->tx_phys, tx_qsize) != 0) {
        return -ENOMEM;
    }
    vio_out16(io + VIRTIO_LEGACY_QUEUE_SIZE, tx_qsize);
    vio_out32(io + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)(inst->tx_phys >> 12));

    // 6. Pre-fill RX ring with buffers
    for (uint16_t i = 0; i < rx_qsize; i++) {
        uint64_t phys = alloc_4k_page();
        if (!phys) {
            return -ENOMEM;
        }
        inst->rx_buf_phys[i] = phys;
        inst->rx_bufs[i] = Phy_To_Virt(phys);
        inst->rx_vq.desc[i].addr = phys;
        inst->rx_vq.desc[i].len = VIRTIO_NET_RX_BUF_SIZE;
        inst->rx_vq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        inst->rx_vq.desc[i].next = 0;
        inst->rx_vq.avail->ring[i] = i;
    }
    arch_wmb();
    inst->rx_vq.avail->idx = rx_qsize;

    // 7. DRIVER_OK
    vio_out8(io + VIRTIO_LEGACY_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
             VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    // 8. Read MAC address
    for (int i = 0; i < 6; i++) {
        inst->mac[i] = vio_in8(io + 0x14 + i);
    }

    // 9. Notify RX queue now that DRIVER_OK is set
    vio_out16(io + VIRTIO_LEGACY_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);

    return 0;
}

// ── Transmit implementation ───────────────────────────────────────────
static void virtio_net_reclaim_tx_locked(struct virtio_net_instance *inst)
{
    virtq_t *tvq = &inst->tx_vq;
    while (inst->tx_desc_tail != tvq->used->idx) {
        uint16_t t_di = inst->tx_desc_tail % inst->tx_qsize;
        if (inst->tx_buf_phys[t_di]) {
            free_4k_page(inst->tx_buf_phys[t_di]);
            inst->tx_buf_phys[t_di] = 0;
        }
        inst->tx_desc_tail++;
    }
}

static int virtio_instance_xmit(struct virtio_net_instance *inst, struct pbuf *p)
{
    if (!inst || !p) return -EINVAL;

    virtq_t *vq = &inst->tx_vq;
    uint64_t flags = spin_lock_irqsave(&inst->tx_lock);

    // Reclaim used TX descriptors under tx_lock before checking queue capacity
    virtio_net_reclaim_tx_locked(inst);

    uint16_t next_head = (inst->tx_desc_head + 1) % inst->tx_qsize;
    if (next_head == inst->tx_desc_tail) {
        spin_unlock_irqrestore(&inst->tx_lock, flags);
        return -ENOMEM;
    }

    uint16_t total = VIRTIO_NET_HDR_SIZE + p->tot_len;
    uint64_t buf_phys = alloc_4k_page();
    if (!buf_phys) {
        spin_unlock_irqrestore(&inst->tx_lock, flags);
        return -ENOMEM;
    }
    uint8_t *buf = (uint8_t *)Phy_To_Virt(buf_phys);
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    pbuf_copy_partial(p, buf + VIRTIO_NET_HDR_SIZE, p->tot_len, 0);

    uint16_t di = inst->tx_desc_head;
    inst->tx_buf_phys[di] = buf_phys;
    vq->desc[di].addr = buf_phys;
    vq->desc[di].len = total;
    vq->desc[di].flags = 0;
    vq->desc[di].next = 0;

    vq->avail->ring[vq->avail->idx % inst->tx_qsize] = di;
    arch_wmb();
    vq->avail->idx++;
    inst->tx_desc_head = next_head;

    vio_out16(inst->io_base + VIRTIO_LEGACY_QUEUE_NOTIFY, VIRTIO_NET_TX_QUEUE);

    spin_unlock_irqrestore(&inst->tx_lock, flags);

    // If legacy mode or polling mode, process completions
    if (inst->legacy_mode) {
        virtio_net_poll_rx();
    } else if (inst->irq_mode == NIC_POLL && inst->ndev) {
        inst->ndev->ops->poll_rx(inst->ndev, 64);
    }
    return 0;
}

// ── RX Polling Implementation ─────────────────────────────────────────
static unsigned virtio_net_ndev_poll_rx(struct net_device *dev, unsigned budget)
{
    if (!dev || !dev->priv) return 0;
    struct virtio_net_instance *inst = (struct virtio_net_instance *)dev->priv;
    if (!inst->initialized || inst->stopped) return 0;

    if (budget > 64) budget = 64;
    unsigned count = 0;
    virtq_t *vq = &inst->rx_vq;

    while (vq->last_used_idx != vq->used->idx && count < budget) {
        virtq_used_elem_t *ue = &vq->used->ring[vq->last_used_idx % inst->rx_qsize];
        uint32_t di = ue->id;
        uint32_t len = ue->len;
        uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
        uint32_t data_len = (len > VIRTIO_NET_HDR_SIZE) ? (len - VIRTIO_NET_HDR_SIZE) : 0;

        if (data_len > 0 && data_len < 1600) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, data_len, PBUF_POOL);
            if (p) {
                pbuf_take(p, buf + VIRTIO_NET_HDR_SIZE, data_len);
                if (net_receive(dev, p) != 0) {
                    pbuf_free(p);
                }
            }
        }

        // Recycle descriptor
        vq->desc[di].len = VIRTIO_NET_RX_BUF_SIZE;
        vq->desc[di].flags = VIRTQ_DESC_F_WRITE;
        uint16_t ai = vq->avail->idx;
        vq->avail->ring[ai % inst->rx_qsize] = di;
        arch_wmb();
        vq->avail->idx = ai + 1;
        vq->last_used_idx++;
        count++;
    }

    if (count > 0) {
        vio_out16(inst->io_base + VIRTIO_LEGACY_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);
    }

    // Drain TX completions under tx_lock
    uint64_t tx_flags = spin_lock_irqsave(&inst->tx_lock);
    virtio_net_reclaim_tx_locked(inst);
    spin_unlock_irqrestore(&inst->tx_lock, tx_flags);

    return count;
}

static int virtio_net_ndev_xmit(struct net_device *dev, struct pbuf *p)
{
    if (!dev || !dev->priv || !p) return -EINVAL;
    struct virtio_net_instance *inst = (struct virtio_net_instance *)dev->priv;
    if (!inst->initialized || inst->stopped) return -EINVAL;

    return virtio_instance_xmit(inst, p);
}

static bool virtio_net_ndev_get_link(struct net_device *dev)
{
    if (!dev || !dev->priv) return false;
    struct virtio_net_instance *inst = (struct virtio_net_instance *)dev->priv;
    if (!inst->initialized || inst->stopped) return false;
    return true;
}

static int virtio_net_ndev_stop(struct net_device *dev)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct virtio_net_instance *inst = (struct virtio_net_instance *)dev->priv;
    inst->stopped = true;

    vio_out8(inst->io_base + VIRTIO_LEGACY_DEVICE_STATUS, 0);
    if (vio_in8(inst->io_base + VIRTIO_LEGACY_DEVICE_STATUS) != 0) {
        if (inst->pdev) {
            device_quarantine(&inst->pdev->dev, inst);
            inst->pdev->dev.quarantined = true;
        }
        return DEVICE_UNSAFE;
    }
    return 0;
}

static const struct net_device_ops virtio_net_ops = {
    .xmit = virtio_net_ndev_xmit,
    .poll_rx = virtio_net_ndev_poll_rx,
    .get_link = virtio_net_ndev_get_link,
    .stop = virtio_net_ndev_stop,
};

// ── IRQ handler ────────────────────────────────────────────────────────
void virtio_net_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)regs;
    struct virtio_net_instance *inst = param ? (struct virtio_net_instance *)(uintptr_t)param : s_legacy_instance;
    if (!inst || !inst->initialized || inst->stopped) return;

    uint8_t isr = vio_in8(inst->io_base + VIRTIO_LEGACY_ISR_STATUS);
    if (isr & VIRTIO_ISR_QUEUE_INTR) {
        sys_mbox_wake();
    }
}

// ── Modern Driver Model Probe ──────────────────────────────────────────
int virtio_net_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    if (!pdev) return -EINVAL;

    uint16_t vnet_bdf = arch9_obs_bdf_encode(pdev->bus, pdev->slot, pdev->fn);
    arch9_fault_on_probe_begin_bdf(vnet_bdf, "virtio-net");

    // Validate device identity: reject modern-only or different vendor/device
    if (pdev->vendor != 0x1af4 || pdev->device != 0x1000) {
        arch9_fault_on_probe_unbound_after_id();
        return -ENODEV;
    }

    if (id) {
        if (id->vendor != 0x1af4 || id->device != 0x1000) {
            arch9_fault_on_probe_unbound_after_id();
            return -ENODEV;
        }
    } else {
        if (pci_match_id(&virtio_net_pci_driver, pdev) == NULL) {
            arch9_fault_on_probe_unbound_after_id();
            return -ENODEV;
        }
    }

    // Validate BAR0 kind: must be PCI_BAR_IO
    if (pdev->bars[0].kind != PCI_BAR_IO || !pdev->bars[0].valid)
        return -EINVAL;

    uint16_t io_base = (uint16_t)(pdev->bars[0].address & 0xFFFF);
    if (io_base == 0) return -EINVAL;

    int rc = pci_set_bus_master(pdev, true);
    if (rc != 0) return rc;

    rc = pci_set_decode(pdev, true, false);
    if (rc != 0) {
        pci_set_bus_master(pdev, false);
        return rc;
    }

    struct virtio_net_instance *inst = kmalloc(sizeof(struct virtio_net_instance));
    if (!inst) {
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return -ENOMEM;
    }
    memset(inst, 0, sizeof(*inst));

    inst->pdev = pdev;
    inst->io_base = io_base;
    inst->legacy_mode = false;
    spin_init(&inst->tx_lock);

    rc = virtio_net_setup_hw(inst);
    if (rc != 0) {
        virtio_net_free_dma(inst);
        vio_out8(io_base + VIRTIO_LEGACY_DEVICE_STATUS, 0);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        kfree(inst);
        return rc;
    }

    pdev->driver_data = inst;

    // Interrupt mode: Route GSI and attempt INTx registration
    uint32_t gsi = 0;
    int route_rc = pci_route_gsi(pdev, &gsi);
    if (route_rc == 0) {
        uint32_t gsi_to_register = gsi;
        /* ARCH-9 Task 11: irq-conflict fault.  Redirect the second
         * NIC's candidate GSI to the first card's already-held slot
         * so the natural register_irq "already occupied" rejection
         * path runs.  register_irq returns 0 → the existing failure
         * path falls back to NIC_POLL.  Brief Step 4:
         * "走真实已占槽拒绝路径，然后POLL". */
        if (arch9_fault_should_force_irq_conflict() &&
            arch9_fault_get_nic_probe_count() >= 1) {
            gsi_to_register = arch9_fault_get_first_card_gsi();
            log_info("virtio-net: irq-conflict fault: redirecting GSI %u to first-card slot %u\n",
                      gsi, gsi_to_register);
        }
        int irq_res = register_irq(gsi_to_register, NULL, &virtio_net_handler,
                                   (uint64_t)(uintptr_t)inst,
                                   IRQF_TRIGGER_LEVEL, "virtio-net");
        if (irq_res == 1) {
            inst->irq_mode = NIC_INTX;
            inst->gsi = gsi_to_register;
            inst->irq_owned = true;
            arch9_fault_record_card_gsi(gsi_to_register);
        } else {
            // Conflict: fall back to NIC_POLL without stealing or modifying the GSI
            inst->irq_mode = NIC_POLL;
            inst->gsi = gsi;
            inst->irq_owned = false;
        }
    } else {
        inst->irq_mode = NIC_POLL;
        inst->irq_owned = false;
    }

    // Register net_device
    struct net_device *ndev = kmalloc(sizeof(struct net_device));
    if (!ndev) {
        rc = -ENOMEM;
        goto err_unwind;
    }
    memset(ndev, 0, sizeof(*ndev));
    snprintf(ndev->name, sizeof(ndev->name), "eth%u", net_device_count());
    memcpy(ndev->mac, inst->mac, 6);
    ndev->mtu = 1500;
    ndev->link_up = true;
    ndev->parent = &pdev->dev;
    ndev->ops = &virtio_net_ops;
    ndev->priv = inst;

    /* ARCH-9 Task 9: mark initialized BEFORE net_device_register so
     * that get_link() observes `initialized == true` during the
     * register-time link query.  Mirrors the e1000 change so DHCP
     * starts at boot. */
    inst->initialized = 1;

    /* ARCH-9 Task 11: per-BDF observation — see e1000.c. */
    arch9_fault_on_adapter_register_bdf(vnet_bdf);

    rc = net_device_register(ndev);
    if (rc != 0) {
        kfree(ndev);
        goto err_unwind;
    }

    inst->ndev = ndev;
    return 0;

err_unwind:
    if (inst->irq_owned) {
        unregister_irq(inst->gsi);
        inst->irq_owned = false;
    }
    vio_out8(io_base + VIRTIO_LEGACY_DEVICE_STATUS, 0);
    pci_set_decode(pdev, false, false);
    pci_set_bus_master(pdev, false);
    virtio_net_free_dma(inst);
    pdev->driver_data = NULL;
    kfree(inst);
    return rc;
}

// ── Controlled Teardown & Quarantine ──────────────────────────────────
void virtio_net_remove(struct pci_device *pdev)
{
    if (!pdev) return;
    struct virtio_net_instance *inst = (struct virtio_net_instance *)pdev->driver_data;
    if (!inst) return;

    if (inst == s_legacy_instance) {
        s_legacy_instance = NULL;
    }

    inst->stopped = true;

    // Issue reset and verify confirmation
    vio_out8(inst->io_base + VIRTIO_LEGACY_DEVICE_STATUS, 0);
    uint8_t status = vio_in8(inst->io_base + VIRTIO_LEGACY_DEVICE_STATUS);
    if (status != 0) {
        device_quarantine(&pdev->dev, inst);
        pdev->dev.quarantined = true;
    }

    if (inst->irq_owned) {
        unregister_irq(inst->gsi);
        inst->irq_owned = false;
    }

    if (inst->ndev) {
        net_device_unregister_boot(inst->ndev);
        kfree(inst->ndev);
        inst->ndev = NULL;
    }

    pci_set_bus_master(pdev, false);

    if (!pdev->dev.quarantined) {
        virtio_net_free_dma(inst);
        pdev->driver_data = NULL;
        kfree(inst);
    }
}

// ── Driver Descriptor ──────────────────────────────────────────────────
static const struct pci_device_id virtio_net_ids[] = {
    {
        .vendor = 0x1af4,
        .device = 0x1000,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = 0,
        .class_mask = 0,
    },
};

const struct pci_driver virtio_net_pci_driver = {
    .name = "virtio-net",
    .id_table = virtio_net_ids,
    .id_count = sizeof(virtio_net_ids) / sizeof(virtio_net_ids[0]),
    .probe = virtio_net_probe,
    .remove = virtio_net_remove,
};

PCI_DRIVER_DECLARE(virtio_net_pci_driver);

// ── Legacy Transitional APIs ───────────────────────────────────────────
int virtio_net_legacy_init(struct pci_device *pdev, uint64_t bar, uint8_t gsi)
{
    if (s_legacy_instance && s_legacy_instance->initialized) return 0;

    struct virtio_net_instance *inst = kmalloc(sizeof(struct virtio_net_instance));
    if (!inst) return -ENOMEM;
    memset(inst, 0, sizeof(*inst));

    inst->pdev = pdev;
    inst->io_base = (uint16_t)(bar & 0xFFFF);
    inst->legacy_mode = true;
    spin_init(&inst->tx_lock);

    if (pdev) {
        pci_set_bus_master(pdev, true);
        pci_set_decode(pdev, true, false);
    }

    int rc = virtio_net_setup_hw(inst);
    if (rc != 0) {
        virtio_net_free_dma(inst);
        kfree(inst);
        return rc;
    }

    if (pdev) {
        pdev->driver_data = inst;
    }

    inst->gsi = gsi;
    inst->irq_mode = NIC_INTX;
    register_irq(gsi, NULL, &virtio_net_handler,
                 (uint64_t)(uintptr_t)inst,
                 IRQF_TRIGGER_LEVEL, "virtio-net");
    inst->irq_owned = true;

    inst->initialized = 1;
    s_legacy_instance = inst;

    log_info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x GSI=%u (INTx)\n",
             inst->mac[0], inst->mac[1], inst->mac[2],
             inst->mac[3], inst->mac[4], inst->mac[5], gsi);
    return 0;
}

int virtio_net_init(uint64_t bar_phys, uint8_t bus, uint8_t dev, uint8_t func, uint8_t gsi)
{
    struct pci_device *pdev = pci_device_lookup(0, bus, dev, func);
    return virtio_net_legacy_init(pdev, bar_phys, gsi);
}

void virtio_net_poll_rx(void)
{
    if (!s_legacy_instance || !s_legacy_instance->initialized) return;
    struct virtio_net_instance *inst = s_legacy_instance;

    unsigned count = 0;
    virtq_t *vq = &inst->rx_vq;
    while (vq->last_used_idx != vq->used->idx) {
        virtq_used_elem_t *ue = &vq->used->ring[vq->last_used_idx % inst->rx_qsize];
        uint32_t di = ue->id, len = ue->len;
        uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
        uint32_t data_len = (len > VIRTIO_NET_HDR_SIZE) ? (len - VIRTIO_NET_HDR_SIZE) : 0;

        if (data_len > 0 && data_len < 1600 && inst->netif_ptr && inst->netif_ptr->input) {
            struct pbuf *pb = pbuf_alloc(PBUF_RAW, data_len, PBUF_POOL);
            if (pb) {
                pbuf_take(pb, buf + VIRTIO_NET_HDR_SIZE, data_len);
                inst->netif_ptr->input(pb, inst->netif_ptr);
            }
        }

        vq->desc[di].len = VIRTIO_NET_RX_BUF_SIZE;
        vq->desc[di].flags = VIRTQ_DESC_F_WRITE;
        uint16_t ai = vq->avail->idx;
        vq->avail->ring[ai % inst->rx_qsize] = di;
        arch_wmb();
        vq->avail->idx = ai + 1;
        vq->last_used_idx++;
        count++;
    }

    if (count > 0) {
        vio_out16(inst->io_base + VIRTIO_LEGACY_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);
    }

    uint64_t tx_flags = spin_lock_irqsave(&inst->tx_lock);
    virtio_net_reclaim_tx_locked(inst);
    spin_unlock_irqrestore(&inst->tx_lock, tx_flags);
}

static err_t virtio_legacy_xmit(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (!s_legacy_instance) return ERR_IF;
    int rc = virtio_instance_xmit(s_legacy_instance, p);
    return rc == 0 ? ERR_OK : ERR_MEM;
}

err_t virtio_netif_init(struct netif *netif)
{
    if (!s_legacy_instance) return ERR_IF;
    s_legacy_instance->netif_ptr = netif;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, s_legacy_instance->mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    netif->linkoutput = virtio_legacy_xmit;
#ifndef OS01_HOST_TEST
    netif->output = etharp_output;
#endif
    return ERR_OK;
}

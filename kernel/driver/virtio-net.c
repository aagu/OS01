// kernel/driver/virtio-net.c — VirtIO-net driver for OS01
// Uses LEGACY transport via BAR0 IO ports.
// QEMU: -device virtio-net-pci,netdev=net0

#include <driver/virtio-net.h>
#include <driver/pci.h>
#include <kernel/debug.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/interrupt.h>
#include <kernel/apic.h>
#include <string.h>

#define VQ_SIZE         64
#define RX_BUF_SIZE     2048

// Legacy virtio register offsets (from BAR0 IO base)
#define VIRTIO_LEGACY_HOST_FEATURES   0x00
#define VIRTIO_LEGACY_GUEST_FEATURES  0x04
#define VIRTIO_LEGACY_QUEUE_PFN       0x08
#define VIRTIO_LEGACY_QUEUE_SIZE      0x0C
#define VIRTIO_LEGACY_QUEUE_SELECT    0x0E
#define VIRTIO_LEGACY_QUEUE_NOTIFY    0x10
#define VIRTIO_LEGACY_DEVICE_STATUS   0x12
#define VIRTIO_LEGACY_ISR_STATUS      0x13

static struct virtio_net_dev vnet;
static uint16_t io_base;  // BAR0 IO port base

// IO port helpers
static inline uint32_t vio_in32(uint16_t port) { uint32_t v; __asm__("inl %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out32(uint16_t port, uint32_t v) { __asm__("outl %0, %1"::"a"(v),"Nd"(port)); }
static inline uint16_t vio_in16(uint16_t port) { uint16_t v; __asm__("inw %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out16(uint16_t port, uint16_t v) { __asm__("outw %0, %1"::"a"(v),"Nd"(port)); }
static inline uint8_t  vio_in8(uint16_t port)  { uint8_t v; __asm__("inb %1, %0":"=a"(v):"Nd"(port)); return v; }
static inline void vio_out8(uint16_t port, uint8_t v)  { __asm__("outb %0, %1"::"a"(v),"Nd"(port)); }

static void virtio_reset(void) {
    vio_out8(io_base + VIRTIO_LEGACY_DEVICE_STATUS, 0);
}

static void virtio_set_status(uint8_t s) {
    vio_out8(io_base + VIRTIO_LEGACY_DEVICE_STATUS, s);
}

static uint8_t virtio_get_isr(void) {
    return vio_in8(io_base + VIRTIO_LEGACY_ISR_STATUS);
}

// Legacy virtqueue: descriptors + avail in one page, used in next page
// Page layout: [desc 0..63] [avail ring] [padding to 4K] [used ring in next page]
static int virtq_init_legacy(virtq_t *vq, uint16_t qsize) {
    vq->size = qsize;
    // Allocate 2 contiguous pages (used ring must follow desc ring)
    struct Page *pages = alloc_pages(ZONE_NORMAL, 2, 0);
    if (!pages) return -1;

    uint64_t phys = pages->phy_address;
    uint8_t *base = (uint8_t*)Phy_To_Virt(phys);
    memset(base, 0, 8192);

    vq->desc  = (virtq_desc_t*)base;
    vq->avail = (virtq_avail_t*)(base + qsize * 16);
    vq->used  = (virtq_used_t*)(base + 4096);
    vq->last_used_idx = 0;
    return 0;
}

static void virtq_notify(virtq_t *vq, uint16_t qi) {
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_NOTIFY, qi);
    (void)vq;
}

// TX: netif->linkoutput
static err_t virtio_xmit(struct netif *netif, struct pbuf *p) {
    (void)netif;
    virtq_t *vq = &vnet.tx_vq;
    uint16_t total = sizeof(virtio_net_hdr_t) + p->tot_len;

    uint64_t buf_phys = alloc_4k_page();
    if (!buf_phys) return ERR_MEM;
    uint8_t *buf = (uint8_t*)Phy_To_Virt(buf_phys);

    virtio_net_hdr_t *hdr = (virtio_net_hdr_t*)buf;
    memset(hdr, 0, sizeof(*hdr));
    pbuf_copy_partial(p, buf + sizeof(*hdr), p->tot_len, 0);

    uint16_t di = 0;
    vq->desc[di].addr  = buf_phys;
    vq->desc[di].len   = total;
    vq->desc[di].flags = 0;
    vq->desc[di].next  = 0;

    vq->avail->ring[vq->avail->idx % vq->size] = di;
    __asm__ volatile("mfence":::"memory");
    vq->avail->idx++;
    virtq_notify(vq, VIRTIO_NET_TX_QUEUE);

    while (vq->used->idx == vq->last_used_idx)
        __asm__ volatile("pause");
    vq->last_used_idx = vq->used->idx;

    virtio_net_poll_rx();
    return ERR_OK;
}

// RX poll
void virtio_net_poll_rx(void) {
    virtq_t *vq = &vnet.rx_vq;
    while (vq->last_used_idx != vq->used->idx) {
        virtq_used_elem_t *ue = &vq->used->ring[vq->last_used_idx % vq->size];
        uint32_t di = ue->id, len = ue->len;
        uint8_t *buf = (uint8_t*)Phy_To_Virt(vq->desc[di].addr);
        uint32_t data_len = len - sizeof(virtio_net_hdr_t);
        if (data_len > 0 && data_len < 1600 && vnet.netif_ptr) {
            struct pbuf *pb = pbuf_alloc(PBUF_RAW, data_len, PBUF_POOL);
            if (pb) {
                pbuf_take(pb, buf + sizeof(virtio_net_hdr_t), data_len);
                vnet.netif_ptr->input(pb, vnet.netif_ptr);
            }
        }
        // Re-submit
        vq->desc[di].len = RX_BUF_SIZE;
        vq->desc[di].flags = VIRTQ_DESC_F_WRITE;
        uint16_t ai = vq->avail->idx;
        vq->avail->ring[ai % vq->size] = di;
        __asm__ volatile("mfence":::"memory");
        vq->avail->idx = ai + 1;
        vq->last_used_idx++;
    }
}

// IRQ handler
void virtio_net_handler(uint64_t nr, uint64_t param, pt_regs_t *regs) {
    (void)nr; (void)param; (void)regs;
    uint8_t isr = virtio_get_isr();
    if (isr & VIRTIO_ISR_QUEUE_INTR)
        virtio_net_poll_rx();
}

// Fill RX with empty buffers
static int virtio_fill_rx(void) {
    virtq_t *vq = &vnet.rx_vq;
    for (int i = 0; i < VQ_SIZE; i++) {
        uint64_t phys = alloc_4k_page();
        if (!phys) return -1;
        vq->desc[i].addr  = phys;
        vq->desc[i].len   = RX_BUF_SIZE;
        vq->desc[i].flags = VIRTQ_DESC_F_WRITE;
    }
    for (int i = 0; i < VQ_SIZE; i++)
        vq->avail->ring[i] = i;
    vq->avail->idx = VQ_SIZE;
    return 0;
}

int virtio_net_init(uint64_t bar_phys, uint8_t bus, uint8_t dev, uint8_t func) {
    // BAR0 is I/O port. bar_phys contains the base port in low 32 bits.
    io_base = (uint16_t)(bar_phys & 0xFFFF);
    log_info("virtio: IO port base=0x%x\n", io_base);

    // Reset
    virtio_reset();

    // ACKNOWLEDGE
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation (only 32-bit for legacy)
    uint32_t host_feat = vio_in32(io_base + VIRTIO_LEGACY_HOST_FEATURES);
    log_info("virtio: host features=0x%x\n", host_feat);

    uint32_t guest_feat = (host_feat & VIRTIO_NET_F_MAC) | VIRTIO_NET_F_STATUS;
    if (!(host_feat & VIRTIO_NET_F_MAC)) {
        log_info("virtio: MAC feature not available!\n");
        return -1;
    }
    vio_out32(io_base + VIRTIO_LEGACY_GUEST_FEATURES, guest_feat);

    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK);
    if (!(vio_in8(io_base + VIRTIO_LEGACY_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        log_info("virtio: features not accepted\n");
        return -1;
    }

    // Setup RX queue (0)
    if (virtq_init_legacy(&vnet.rx_vq, VQ_SIZE)) return -1;
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_RX_QUEUE);
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SIZE, VQ_SIZE);
    uint64_t rx_page = ((uint64_t)vnet.rx_vq.desc - PAGE_OFFSET) >> 12;
    vio_out32(io_base + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)rx_page);

    // Setup TX queue (1)
    if (virtq_init_legacy(&vnet.tx_vq, VQ_SIZE)) return -1;
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_TX_QUEUE);
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SIZE, VQ_SIZE);
    uint64_t tx_page = ((uint64_t)vnet.tx_vq.desc - PAGE_OFFSET) >> 12;
    vio_out32(io_base + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)tx_page);

    // DRIVER_OK
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    // Read MAC from device config (available via BAR0 after DRIVER_OK)
    // For legacy, device config is at BAR0 + 0x14 (after legacy registers)
    for (int i = 0; i < 6; i++)
        vnet.mac[i] = vio_in8(io_base + 0x14 + i);

    // Fill RX
    if (virtio_fill_rx()) { log_info("virtio: RX fill failed\n"); return -1; }

    // IRQ
    vnet.irq = pci_read_interrupt_line(bus, dev, func);
    register_irq(0x20 + vnet.irq, NULL, &virtio_net_handler, 0,
                 get_ioapic_controller(), "virtio-net");

    log_info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ=%d\n",
             vnet.mac[0],vnet.mac[1],vnet.mac[2],vnet.mac[3],vnet.mac[4],vnet.mac[5],vnet.irq);
    return 0;
}

err_t virtio_netif_init(struct netif *netif) {
    vnet.netif_ptr = netif;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, vnet.mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->linkoutput = virtio_xmit;
    return ERR_OK;
}

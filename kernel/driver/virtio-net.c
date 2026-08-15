// kernel/driver/virtio-net.c — VirtIO-net driver for OS01
// Uses LEGACY transport via BAR0 IO ports.
// QEMU: -device virtio-net-pci,netdev=net0

#include <lwip/etharp.h>
#include <driver/virtio-net.h>
#include <driver/pci.h>
#include <kernel/debug.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/interrupt.h>
#include <kernel/apic.h>
#include <kernel/vmm.h>
#include <string.h>

#define VQ_SIZE         64
#define RX_BUF_SIZE     2048
#define VIRTIO_NET_HDR_SIZE 10

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
static uint16_t io_base;

// TX descriptor ring tracking
static uint16_t tx_desc_head = 0;
static uint16_t tx_desc_tail = 0;

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

static int virtq_init_legacy(virtq_t *vq, uint16_t qsize) {
    vq->size = qsize;
    struct Page *pages = alloc_pages(ZONE_NORMAL, 2, 0);
    if (!pages) return -1;

    uint64_t phys = pages->phy_address;
    uint8_t *base = (uint8_t*)Phy_To_Virt(phys);
    // Zero at least enough for desc + avail + used (used aligned to next 4K)
    uint16_t desc_sz = qsize * 16;
    uint16_t used_off = (desc_sz + 4 + qsize * 2 + 4095) & ~4095;  // page_align after avail
    memset(base, 0, used_off + 4 + qsize * 8);

    vq->desc  = (virtq_desc_t*)base;
    vq->avail = (virtq_avail_t*)(base + desc_sz);
    vq->used  = (virtq_used_t*)(base + used_off);
    vq->last_used_idx = 0;
    log_info("virtio: vring qsize=%u desc_sz=%u used_off=%u\n", qsize, desc_sz, used_off);
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

    uint16_t next_head = (tx_desc_head + 1) % VQ_SIZE;
    if (next_head == tx_desc_tail) {
        log_info("virtio: TX ring full\n");
        return ERR_MEM;
    }

    uint16_t total = VIRTIO_NET_HDR_SIZE + p->tot_len;
    uint64_t buf_phys = alloc_4k_page();
    if (!buf_phys) return ERR_MEM;
    uint8_t *buf = (uint8_t*)Phy_To_Virt(buf_phys);

    virtio_net_hdr_t *hdr = (virtio_net_hdr_t*)buf;
    memset(hdr, 0, VIRTIO_NET_HDR_SIZE);
    pbuf_copy_partial(p, buf + VIRTIO_NET_HDR_SIZE, p->tot_len, 0);

    uint16_t di = tx_desc_head;
    vq->desc[di].addr  = buf_phys;
    vq->desc[di].len   = total;
    vq->desc[di].flags = 0;
    vq->desc[di].next  = 0;

    vq->avail->ring[vq->avail->idx % vq->size] = di;
    __asm__ volatile("mfence":::"memory");
    vq->avail->idx++;
    tx_desc_head = next_head;

    log_info("virtio: TX len=%d desc=%u\n", total, di);
    virtq_notify(vq, VIRTIO_NET_TX_QUEUE);
    virtio_net_poll_rx();
    return ERR_OK;
}

// RX poll + TX completion
void virtio_net_poll_rx(void) {
    {
        virtq_t *vq = &vnet.rx_vq;
        while (vq->last_used_idx != vq->used->idx) {
            virtq_used_elem_t *ue = &vq->used->ring[vq->last_used_idx % vq->size];
            uint32_t di = ue->id, len = ue->len;
            uint8_t *buf = (uint8_t*)Phy_To_Virt(vq->desc[di].addr);
            uint32_t data_len = len - VIRTIO_NET_HDR_SIZE;
            if (data_len > 0 && data_len < 1600 && vnet.netif_ptr) {
                struct pbuf *pb = pbuf_alloc(PBUF_RAW, data_len, PBUF_POOL);
                if (pb) {
                    pbuf_take(pb, buf + VIRTIO_NET_HDR_SIZE, data_len);
                    vnet.netif_ptr->input(pb, vnet.netif_ptr);
                }
            }
            vq->desc[di].len = RX_BUF_SIZE;
            vq->desc[di].flags = VIRTQ_DESC_F_WRITE;
            uint16_t ai = vq->avail->idx;
            vq->avail->ring[ai % vq->size] = di;
            __asm__ volatile("mfence":::"memory");
            vq->avail->idx = ai + 1;
            vq->last_used_idx++;
        }
    }
    {
        virtq_t *vq = &vnet.tx_vq;
        while (tx_desc_tail != vq->used->idx) {
            tx_desc_tail = (tx_desc_tail + 1) % VQ_SIZE;
        }
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
    virtq_notify(vq, VIRTIO_NET_RX_QUEUE);
    return 0;
}

int virtio_net_init(uint64_t bar_phys, uint8_t bus, uint8_t dev, uint8_t func, uint8_t gsi) {
    io_base = (uint16_t)(bar_phys & 0xFFFF);
    vnet.pci_bus = bus; vnet.pci_dev = dev; vnet.pci_func = func;
    vnet.gsi = gsi;
    log_info("virtio: IO port base=0x%x\n", io_base);

    virtio_reset();
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint32_t host_feat = vio_in32(io_base + VIRTIO_LEGACY_HOST_FEATURES);
    log_info("virtio: host features=0x%x\n", host_feat);

    // ToroKernel-style: disable everything except MAC and STATUS
    uint32_t guest_feat = host_feat & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
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
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_RX_QUEUE);
    uint16_t qsize = vio_in16(io_base + VIRTIO_LEGACY_QUEUE_SIZE);
    if (qsize < VQ_SIZE) qsize = VQ_SIZE;
    if (virtq_init_legacy(&vnet.rx_vq, qsize)) return -1;
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SIZE, qsize);
    uint64_t rx_page = ((uint64_t)vnet.rx_vq.desc - PAGE_OFFSET) >> 12;
    log_info("virtio: RX phys=%lx pfn=%u\n", (unsigned long)(rx_page << 12), (unsigned)rx_page);
    vio_out32(io_base + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)rx_page);
    
    log_info("virtio: RX queue size=%u\n", qsize);

    // Setup TX queue (1)
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SELECT, VIRTIO_NET_TX_QUEUE);
    qsize = vio_in16(io_base + VIRTIO_LEGACY_QUEUE_SIZE);
    if (qsize < VQ_SIZE) qsize = VQ_SIZE;
    if (virtq_init_legacy(&vnet.tx_vq, qsize)) return -1;
    vio_out16(io_base + VIRTIO_LEGACY_QUEUE_SIZE, qsize);
    uint64_t tx_page = ((uint64_t)vnet.tx_vq.desc - PAGE_OFFSET) >> 12;
    log_info("virtio: TX phys=%lx pfn=%u\n", (unsigned long)(tx_page << 12), (unsigned)tx_page);
    vio_out32(io_base + VIRTIO_LEGACY_QUEUE_PFN, (uint32_t)tx_page);
    log_info("virtio: TX queue size=%u\n", qsize);

    // DRIVER_OK
    virtio_set_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    for (int i = 0; i < 6; i++)
        vnet.mac[i] = vio_in8(io_base + 0x14 + i);

    // Register IRQ handler (INTx via IOAPIC, same pattern as torokernel)
    register_irq(gsi, NULL, &virtio_net_handler, 0,
                 IRQF_TRIGGER_LEVEL, "virtio-net");
    log_info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x GSI=%u (INTx)\n",
             vnet.mac[0],vnet.mac[1],vnet.mac[2],vnet.mac[3],vnet.mac[4],vnet.mac[5],
             gsi);
    return 0;
}


err_t virtio_netif_init(struct netif *netif) {
    vnet.netif_ptr = netif;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, vnet.mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    netif->linkoutput = virtio_xmit;
    netif->output = etharp_output;
    return ERR_OK;
}

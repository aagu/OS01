// kernel/driver/e1000.c — Intel 82540EM (e1000) NIC driver
#include <driver/e1000.h>
#include <driver/pci.h>
#include <kernel/vmm.h>       // vmm_map_page, kernel_map, PAGE_KERNEL_MMIO
#include <kernel/pmm.h>       // PAGE_2M_MASK, alloc_pages, alloc_4k_page
#include <kernel/memory.h>    // Phy_To_Virt
#include <kernel/interrupt.h> // register_irq
#include <kernel/apic.h>      // lapic_eoi, get_ioapic_controller
#include <kernel/log.h>
#include <kernel/slab.h>       // log_info
#include <string.h>
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"   // ethernet_input (strips ETH header, routes ARP/IP)
#include "lwip/tcpip.h"
#include "lwip/etharp.h"   // ethernet_input

// ── Per-device state ───────────────────────────────────────────
static struct {
    uint64_t           mmio_phys;
    volatile uint8_t  *mmio;           // kernel-virtual MMIO base
    uint8_t            mac[6];
    uint8_t            irq;
    struct netif      *netif_ptr;      // set by e1000_netif_init during netif_add

    // Descriptor rings (in RAM — Phy_To_Virt works)
    e1000_rx_desc_t   *rx_descs;
    e1000_tx_desc_t   *tx_descs;
    uint64_t            rx_phys;
    uint64_t            tx_phys;

    // Packet buffers — must be allocated from physical pages, NOT kmalloc.
    // kmalloc returns slab memory which is NOT identity-mapped, so
    // Virt_To_Phy() computes a garbage physical address → DMA corruption.
    // Use alloc_4k_page() which returns a physical address usable for DMA.
    uint64_t           rx_buf_phys[E1000_NUM_RX_DESC];
    uint64_t           tx_buf_phys[E1000_NUM_TX_DESC];
    uint8_t           *rx_bufs[E1000_NUM_RX_DESC];    // Phy_To_Virt(rx_buf_phys[i])
    uint8_t           *tx_bufs[E1000_NUM_TX_DESC];    // Phy_To_Virt(tx_buf_phys[i])

    uint32_t            tx_head;       // next descriptor to send
    uint32_t            tx_tail;       // next free slot (post-completion)
    uint32_t            rx_tail;       // next descriptor to check

    int                 initialized;
} e1000;

// ── MMIO helpers ───────────────────────────────────────────────
static inline uint32_t e1000_read(uint32_t reg)
{
    return *(volatile uint32_t *)(e1000.mmio + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(e1000.mmio + reg) = val;
}

// ── EEPROM read (MAC address) ─────────────────────────────────
static int e1000_eeprom_read(uint8_t addr, uint16_t *out)
{
    e1000_write(E1000_REG_EERD, ((uint32_t)addr << 8) | E1000_EERD_START);
    for (int i = 0; i < 100000; i++) {
        if (e1000_read(E1000_REG_EERD) & E1000_EERD_DONE) {
            *out = (uint16_t)(e1000_read(E1000_REG_EERD) >> E1000_EERD_DATA_SHIFT);
            return 0;
        }
    }
    return -1;
}

// ── Poll RX + buffered processing ──────────────────────────────
// e1000_poll_rx: called from IRQ context (PIT handler) — copies packet
//   into a software ring queue.  e1000_process_rx: called from
//   tcpip_thread context (via net_poll_rx) — drains the queue and
//   delivers to lwIP via etharp_input.
//
// Single-slot buffering was a drop-loss bug: if a second packet
// arrived while the first was still buffered (not yet processed by
// tcpip_thread), e1000_poll_rx returned early, RDT stalled, QEMU
// saw a full ring and dropped the packet (ARP replies lost → DHCP
// stuck, TCP connect never completes).  A ring queue keeps the
// hardware ring drained continuously.

#define E1000_RXQ_DEPTH  64

typedef struct {
    uint8_t  *buf[E1000_RXQ_DEPTH];
    uint16_t  len[E1000_RXQ_DEPTH];
    int       head;   // next slot to fill (IRQ context)
    int       tail;   // next slot to drain (tcpip_thread context)
} e1000_rxq_t;

static e1000_rxq_t e1000_rxq;

void e1000_poll_rx(void) {
    if (!e1000.initialized) return;
    // Drain as many completed descriptors as fit in the queue.
    while (e1000.rx_descs[e1000.rx_tail].status & E1000_RXD_STAT_DD) {
        uint16_t len = e1000.rx_descs[e1000.rx_tail].length;
        if (len > 0 && len < 1600) {
            int next = (e1000_rxq.head + 1) % E1000_RXQ_DEPTH;
            if (next == e1000_rxq.tail) {
                // Queue full — drop this packet, keep HW ring moving.
                e1000.rx_descs[e1000.rx_tail].status = 0;
                e1000.rx_tail = (e1000.rx_tail + 1) % E1000_NUM_RX_DESC;
                continue;
            }
            uint8_t *buf = (uint8_t *)kmalloc(len);
            if (buf) {
                memcpy(buf, e1000.rx_bufs[e1000.rx_tail], len);
                e1000_rxq.buf[e1000_rxq.head] = buf;
                e1000_rxq.len[e1000_rxq.head] = len;
                e1000_rxq.head = next;
            }
        }
        e1000.rx_descs[e1000.rx_tail].status = 0;
        e1000.rx_tail = (e1000.rx_tail + 1) % E1000_NUM_RX_DESC;
    }
    // Clear any pending RX interrupts.  QEMU's e1000e sets interrupt
    // bits in ICR when packets arrive and may defer writing further
    // descriptors until the interrupt is acknowledged (ICR read).  Our
    // MSI-X path is not wired up (no IRQ ever fires), so without this
    // read QEMU goes quiet after the first burst of ~10 packets and
    // the HTTP body is never delivered.
    e1000_read(E1000_REG_ICR);
    // RDT is the LAST descriptor the software has prepared.  QEMU's
    // e1000e only re-arms reception when the RDT VALUE CHANGES and it
    // stops when RDH >= RDT.  A constant RDT means no re-arm: the NIC
    // goes quiet after its first burst (~10 pkts) and the HTTP body is
    // never delivered.  Worse, if we only write RDT while draining, a
    // stopped NIC (no DD descriptors) never gets re-armed — deadlock.
    // Toggle 30→31 on EVERY call (even with nothing to drain) so the
    // value always changes and RDT stays > RDH: QEMU keeps delivering.
    e1000_write(E1000_REG_RDT, 30);
    e1000_write(E1000_REG_RDT, 31);
}

void e1000_process_rx(void) {
    if (!e1000.initialized) return;
    while (e1000_rxq.tail != e1000_rxq.head) {
        uint8_t *buf = e1000_rxq.buf[e1000_rxq.tail];
        uint16_t len = e1000_rxq.len[e1000_rxq.tail];
        e1000_rxq.buf[e1000_rxq.tail] = NULL;
        e1000_rxq.tail = (e1000_rxq.tail + 1) % E1000_RXQ_DEPTH;

        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (p) {
            memcpy(p->payload, buf, len);
            e1000.netif_ptr->input(p, e1000.netif_ptr);
        }
        kfree(buf);
    }
}

// ── Interrupt handler ──────────────────────────────────────────
// Uses register_irq() — the same pattern as keyboard, serial, PIT.
// The kernel has 16 pre-installed stubs (arch/x86_64/irq.c) for
// vectors 0x20–0x2f that dispatch through do_IRQ → irq_table[].
// No DEFINE_INTR_STUB/REGISTER_INTR_HANDLER needed.

static void e1000_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;
    uint32_t icr = e1000_read(E1000_REG_ICR);
    if (!icr) return;

    // ── RX: descriptor done ─────────────────────────────────
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        // Buffer only (IRQ-safe).  lwIP processing happens in
        // tcpip_thread context — e1000_process_rx() runs from
        // net_poll_rx() inside sys_arch_mbox_fetch.  Wake the
        // fetcher so buffered packets are drained promptly.
        // NEVER call tcpip_input/tcpip_inpkt from IRQ context
        // (lwIP asserts: "tcpip_thread: invalid message").
        e1000_poll_rx();
        extern void sys_mbox_wake(void);
        sys_mbox_wake();
    }

    // ── TX: descriptor done ─────────────────────────────────
    if (icr & E1000_ICR_TXDW) {
        // Walk from tx_tail to tx_head, freeing completed descriptors
        while (e1000.tx_tail != e1000.tx_head) {
            if (!(e1000.tx_descs[e1000.tx_tail].status & E1000_TXD_STAT_DD))
                break;
            e1000.tx_descs[e1000.tx_tail].status = 0;
            e1000.tx_tail = (e1000.tx_tail + 1) % E1000_NUM_TX_DESC;
        }
    }
}

// ── TX: netif->linkoutput ─────────────────────────────────────
err_t e1000_xmit(struct netif *netif, struct pbuf *p)
{
    (void)netif;

    uint32_t next = (e1000.tx_head + 1) % E1000_NUM_TX_DESC;
    if (next == e1000.tx_tail) {
        // Ring full — lwIP will retry
        return ERR_MEM;
    }

    // Copy pbuf chain into TX buffer
    uint8_t *dst = e1000.tx_bufs[e1000.tx_head];
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(dst + total, q->payload, q->len);
        total += (uint16_t)q->len;
    }

    e1000.tx_descs[e1000.tx_head].length = total;
    e1000.tx_descs[e1000.tx_head].cmd = E1000_TXD_CMD_EOP
                                       | E1000_TXD_CMD_IFCS
                                       | E1000_TXD_CMD_RS;
    e1000.tx_descs[e1000.tx_head].status = 0;

    e1000.tx_head = next;
    e1000_write(E1000_REG_TDT, e1000.tx_head);

    return ERR_OK;
}

// ── netif init callback — called by netif_add ──────────────────
// Sets hwaddr, hwaddr_len, mtu, flags, linkoutput.
// The e1000 state is global (single NIC), so no void *arg needed.

static err_t e1000_netif_input(struct pbuf *p, struct netif *n) {
    // MUST go through ethernet_input: it strips the 14-byte Ethernet
    // header and routes the frame to etharp_input (ARP) or ip_input
    // (IP).  Calling etharp_input directly passed the raw Ethernet
    // frame — ARP header fields were read from the wrong offset, the
    // hwtype/proto sanity check failed and every packet was dropped.
    return ethernet_input(p, n);
}

err_t e1000_netif_init(struct netif *netif)
{
    e1000.netif_ptr = netif;  // store for IRQ handler's tcpip_inpkt()
    netif->input = e1000_netif_input;  // inline etharp processing
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, e1000.mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = e1000_xmit;
    netif->output = etharp_output;  // standard Ethernet ARP output
    return ERR_OK;
}

// ── Initialization ─────────────────────────────────────────────
int e1000_init(uint64_t bar_phys, uint8_t irq, int use_msi)
{
    if (e1000.initialized) return 0;

    // 1. Map MMIO BAR (2MB page at PCI MMIO address)
    e1000.mmio_phys = bar_phys;
    uint64_t bar_page = bar_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page,
                 (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_MMIO);
    e1000.mmio = (volatile uint8_t *)Phy_To_Virt(bar_phys);
    e1000.irq = irq;

    // 2. Reset device
    uint32_t ctrl = e1000_read(E1000_REG_CTRL);
    e1000_write(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) {
        if (!(e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST))
            break;
    }

    // 3. Read MAC — try EEPROM first, fall back to RAL/RAH registers.
    //    QEMU e1000e (82574L) has NO working EEPROM: the EERD read
    //    times out, but the hardware auto-loads the MAC into RAL0/RAH0
    //    after reset (same behavior as Linux e1000e driver fallback).
    int eep_ok = 1;
    for (int i = 0; i < 3; i++) {
        uint16_t eep_word;
        if (e1000_eeprom_read((uint8_t)i, &eep_word) != 0) {
            eep_ok = 0;
            break;
        }
        e1000.mac[i * 2]     = (uint8_t)(eep_word & 0xFF);
        e1000.mac[i * 2 + 1] = (uint8_t)(eep_word >> 8);
    }
    if (!eep_ok) {
        uint32_t ral = e1000_read(E1000_REG_RAL0);
        uint32_t rah = e1000_read(E1000_REG_RAH0);
        e1000.mac[0] = (uint8_t)(ral & 0xFF);
        e1000.mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        e1000.mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        e1000.mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        e1000.mac[4] = (uint8_t)(rah & 0xFF);
        e1000.mac[5] = (uint8_t)((rah >> 8) & 0xFF);
        log_info("e1000: MAC read from RAL0/RAH0 (EEPROM unavailable)\n");
    }

    // 4. Allocate descriptor rings (physically contiguous, 16-byte aligned)
    struct Page *rx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    struct Page *tx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!rx_page || !tx_page) return -1;

    e1000.rx_phys = rx_page->phy_address;
    e1000.tx_phys = tx_page->phy_address;
    e1000.rx_descs = (e1000_rx_desc_t *)Phy_To_Virt(e1000.rx_phys);
    e1000.tx_descs = (e1000_tx_desc_t *)Phy_To_Virt(e1000.tx_phys);
    memset(e1000.rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    memset(e1000.tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

    // 5. Allocate DMA buffers for RX/TX descriptors using physical pages.
    //    alloc_4k_page() returns a physical address — Phy_To_Virt gives the
    //    kernel-virtual address for CPU access.  The physical address goes
    //    directly into the descriptor (no Virt_To_Phy needed).
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -1;
        e1000.rx_buf_phys[i] = buf_phys;
        e1000.rx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        e1000.rx_descs[i].addr = buf_phys;  // physical address for DMA
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -1;
        e1000.tx_buf_phys[i] = buf_phys;
        e1000.tx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        e1000.tx_descs[i].addr = buf_phys;  // physical address for DMA
    }

    // 5b. Trigger PHY auto-negotiation via MDIC — REQUIRED for QEMU RX.
    // QEMU's e1000 only sets STATUS.LU (link up) after the PHY
    // auto-neg timer fires (~500ms virtual time).  The timer only
    // starts if the guest writes BMCR via MDIC.  Without this,
    // STATUS.LU stays 0, e1000x_rx_ready() returns false and RX is
    // permanently disabled (TX works, OFFER/SYN-ACK never arrive).
    {
        // BMCR: ANRESTART(9) | AUTOEN(12) | FD(8) | SPEED1000(6)
        uint32_t bmcr = (1u << 9) | (1u << 12) | (1u << 8) | (1u << 6);
        // MDIC: data=bmcr | op=WRITE(01) | PHY addr=0 | reg=0 (BMCR)
        e1000_write(0x0020, bmcr | (1u << 21) | (0u << 16) | (0u << 26));
        for (int m = 0; m < 1000; m++) {
            if (e1000_read(0x0020) & (1u << 28))
                break;
            for (volatile int d = 0; d < 1000; d++)
                __asm__ volatile("pause");
        }
        log_info("e1000: PHY auto-neg triggered via MDIC (STATUS=0x%x)\n",
                 e1000_read(E1000_REG_STATUS));
    }

    // 6. Configure RX
    e1000_write(E1000_REG_RDBAL, (uint32_t)(e1000.rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_REG_RDBAH, (uint32_t)(e1000.rx_phys >> 32));
    e1000_write(E1000_REG_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000.rx_tail = 0;
    e1000_write(E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM
        | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    // 7. Configure TX
    e1000_write(E1000_REG_TDBAL, (uint32_t)(e1000.tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_REG_TDBAH, (uint32_t)(e1000.tx_phys >> 32));
    e1000_write(E1000_REG_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000.tx_head = 0;
    e1000.tx_tail = 0;
    e1000_write(E1000_REG_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP
        | (0x10 << E1000_TCTL_CT_SHIFT)
        | (E1000_TCTL_COLD_FULLDUPLEX << E1000_TCTL_COLD_SHIFT));

    // 8. Enable e1000 interrupt sources
    e1000_write(E1000_REG_IMS,
        E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_TXDW | E1000_ICR_LSC);

    // 9. Register interrupt handler.
    // MSI-X path (use_msi != 0): caller already ran pci_enable_msix()
    // with vector 0x30.  Register on GSI 16 (vector 0x30) as the
    // dispatch slot — MSI-X interrupts arrive directly on the LAPIC,
    // bypassing the IOAPIC (which never fires on Q35+TCG).
    // INTx fallback (use_msi == 0): register on the GSI from
    // PCI_INTERRUPT_LINE, routed through the IOAPIC.
    uint8_t reg_gsi = use_msi ? 16 : irq;
    register_irq(reg_gsi, NULL, &e1000_handler, 0,
                 IRQF_TRIGGER_LEVEL, "e1000");

    e1000.initialized = 1;

    log_info("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ=%u%s\n",
                e1000.mac[0], e1000.mac[1], e1000.mac[2],
                e1000.mac[3], e1000.mac[4], e1000.mac[5],
                reg_gsi, use_msi ? " (MSI-X v0x30)" : "");
    return 0;
}

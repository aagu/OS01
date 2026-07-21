// kernel/driver/e1000.c — Intel 82540EM (e1000) NIC driver
#include <driver/e1000.h>
#include <driver/pci.h>
#include <kernel/vmm.h>       // vmm_map_page, kernel_map, PAGE_KERNEL_MMIO
#include <kernel/pmm.h>       // PAGE_2M_MASK, alloc_pages, alloc_4k_page
#include <kernel/memory.h>    // Phy_To_Virt
#include <kernel/interrupt.h> // register_irq
#include <kernel/apic.h>      // lapic_eoi, LAPIC_* constants, IOAPIC_*
#include <kernel/log.h>       // log_info
#include <kernel/printk.h>   // serial_printk
#include <kernel/arch/io.h>  // arch_outb (VMEXIT for QEMU event loop)
#include <string.h>
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"   // tcpip_inpkt, tcpip_input
#include "lwip/etharp.h"  // etharp_output

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
// Returns 0 on success, -1 on timeout (e.g. e1000e has no EEPROM).
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

// ── Interrupt handler ──────────────────────────────────────────
// Uses register_irq() — the same pattern as keyboard, serial, PIT.
// The kernel has 24 pre-installed stubs (arch/x86_64/irq.c) for
// vectors 0x20–0x37 that dispatch through do_IRQ → irq_table[].
// No DEFINE_INTR_STUB/REGISTER_INTR_HANDLER needed.

static void e1000_poll_rx(void)
{
    while (e1000.rx_descs[e1000.rx_tail].status & E1000_RXD_STAT_DD) {
        uint16_t len = e1000.rx_descs[e1000.rx_tail].length;
        if (len > 0) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
            if (p) {
                memcpy(p->payload, e1000.rx_bufs[e1000.rx_tail], len);
                if (tcpip_inpkt(p, e1000.netif_ptr, tcpip_input) != ERR_OK)
                    pbuf_free(p);
            } else {
                e1000.rx_bufs[e1000.rx_tail][0] = 0xDB; // "dropped" marker
            }
        }
        e1000.rx_descs[e1000.rx_tail].status = 0;
        e1000.rx_tail = (e1000.rx_tail + 1) % E1000_NUM_RX_DESC;
        e1000_write(E1000_REG_RDT, e1000.rx_tail);
    }
}

static void e1000_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;

    // Loop: re-read ICR after processing to catch events that arrived
    // during the handler (e.g. response packets arriving while we were
    // in e1000_poll_rx → tcpip_inpkt → lwIP → e1000_xmit).
    // Without this re-read, those events are only discovered on the next
    // interrupt — which may be the same MSI that already fired and won't
    // fire again (MSI is edge-triggered, no level-based retry).
    for (int loop = 0; loop < 4; loop++) {
        uint32_t icr = e1000_read(E1000_REG_ICR);
        if (!icr) break;

        // ── RX: poll on every interrupt ──────────────────────────
        e1000_poll_rx();

        // ── TX: descriptor done ─────────────────────────────────
        if (icr & E1000_ICR_TXDW) {
            while (e1000.tx_tail != e1000.tx_head) {
                if (!(e1000.tx_descs[e1000.tx_tail].status & E1000_TXD_STAT_DD))
                    break;
                e1000.tx_descs[e1000.tx_tail].status = 0;
                e1000.tx_tail = (e1000.tx_tail + 1) % E1000_NUM_TX_DESC;
            }
        }

        // ── Re-poll RX after TX (SLIRP needs VMEXIT for responses) ─
        if (icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) {
            for (volatile int i = 0; i < 3; i++) {
                arch_outb(0x80, 0);
                e1000_poll_rx();
            }
        }

        // ── LSC: link status change ─────────────────────────────
        if (icr & E1000_ICR_LSC) {
            if (e1000.netif_ptr) {
                uint32_t st = e1000_read(E1000_REG_STATUS);
                if (st & E1000_STATUS_LU)
                    netif_set_link_up(e1000.netif_ptr);
                else
                    netif_set_link_down(e1000.netif_ptr);
            }
        }
    }
}

int e1000_link_up(void)
{
    if (!e1000.initialized) return 0;
    return (e1000_read(E1000_REG_STATUS) & E1000_STATUS_LU) ? 1 : 0;
}

// ── TX: netif->linkoutput ─────────────────────────────────────
err_t e1000_xmit(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    log_info("e1000: xmit len=%u\n", (unsigned)p->tot_len);

    // Debug: read IMS only (NOT ICR — ICR is Read-Clear, so reading it
    // here would consume interrupts that should be handled by the IRQ handler)
    uint32_t ims = e1000_read(E1000_REG_IMS);
    uint32_t st = e1000_read(E1000_REG_STATUS);
    if (ims != 0x95)
        log_info("e1000: pre-xmit IMS=%#x STATUS=%#x\n", ims, st);

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

    // Debug: check TX descriptor status and device state after submission
    // NOTE: reading ICR here would CLEAR pending interrupts, so we skip it.
    {
        int prev = (next == 0) ? (E1000_NUM_TX_DESC - 1) : (next - 1);
        uint8_t tx_st = e1000.tx_descs[prev].status;
        uint32_t ims_after = e1000_read(E1000_REG_IMS);
        uint32_t tdh = e1000_read(E1000_REG_TDH);
        uint32_t tdt = e1000_read(E1000_REG_TDT);
        log_info("e1000: post-xmit tx_desc[%u].status=%#x IMS=%#x TDH=%u TDT=%u\n",
                  prev, tx_st, ims_after, tdh, tdt);
    }

    // Poll RX — catch packets that arrived during TX
    e1000_poll_rx();

    return ERR_OK;
}

// ── netif init callback — called by netif_add ──────────────────
// Sets hwaddr, hwaddr_len, mtu, flags, linkoutput.
// The e1000 state is global (single NIC), so no void *arg needed.

err_t e1000_netif_init(struct netif *netif)
{
    e1000.netif_ptr = netif;  // store for IRQ handler's tcpip_inpkt()
    // netif->input is already set by netif_add(..., tcpip_input)
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, e1000.mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = e1000_xmit;
    netif->output = etharp_output;  // standard Ethernet ARP output
    return ERR_OK;
}

// ── MSI controller (just does lapic_eoi) ──────────────────────
static void       msi_ack(uint64_t nr) { (void)nr; lapic_eoi(); }
static void       msi_noop_enable(uint64_t nr) { (void)nr; }
static uint64_t   msi_noop_install(uint64_t nr, void *arg) { (void)nr; (void)arg; return 0; }
static void       msi_noop_disable(uint64_t nr) { (void)nr; }
static void       msi_noop_uninstall(uint64_t nr) { (void)nr; }

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

    // 3. Read MAC from EEPROM (or RA registers for e1000e which has no EEPROM)
    for (int i = 0; i < 3; i++) {
        uint16_t word;
        if (e1000_eeprom_read((uint8_t)i, &word) == 0) {
            e1000.mac[i * 2]     = (uint8_t)(word & 0xFF);
            e1000.mac[i * 2 + 1] = (uint8_t)(word >> 8);
        } else {
            // Fallback: read MAC from Receive Address register (RAL0/RAH0)
            // loaded from NVM by device reset on e1000e.
            uint32_t ral = e1000_read(E1000_REG_RA_BASE);
            uint32_t rah = e1000_read(E1000_REG_RA_BASE + 4);
            e1000.mac[0] = (uint8_t)(ral & 0xFF);
            e1000.mac[1] = (uint8_t)((ral >> 8) & 0xFF);
            e1000.mac[2] = (uint8_t)((ral >> 16) & 0xFF);
            e1000.mac[3] = (uint8_t)((ral >> 24) & 0xFF);
            e1000.mac[4] = (uint8_t)(rah & 0xFF);
            e1000.mac[5] = (uint8_t)((rah >> 8) & 0xFF);
            break;
        }
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

    // 6. Configure RX
    e1000_write(E1000_REG_RDBAL, (uint32_t)(e1000.rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_REG_RDBAH, (uint32_t)(e1000.rx_phys >> 32));
    e1000_write(E1000_REG_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000.rx_tail = 0;
    e1000_write(E1000_REG_RDTR, 100);   // RDTR = 100 μs RX delay
    e1000_write(E1000_REG_RADV, 1000);  // RADV = 1 ms absolute delay
    e1000_write(E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_BAM
        | E1000_RCTL_UPE | E1000_RCTL_MPE
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

    // 9. Set interrupt delivery mode.
    //    MSI: CTRL_EXT.INT_MODE=1 → device sends MSI directly to LAPIC,
    //    bypassing PIRQ/IOAPIC.  Handler registered with a minimal controller
    //    that just does lapic_eoi.
    //    INTx: CTRL_EXT.INT_MODE=0 → legacy INTx via PIRQ/IOAPIC (broken on
    //    Q35 QEMU 11.0.2 for GSI>=16, but works on real hardware / newer QEMU).
    {
        uint32_t ctrl_ext = e1000_read(E1000_REG_CTRL_EXT);
        if (use_msi) {
            ctrl_ext |= E1000_CTRL_EXT_INT_MODE;
            log_info("e1000: MSI mode\n");
        } else {
            ctrl_ext &= ~E1000_CTRL_EXT_INT_MODE;
            log_info("e1000: INTx mode\n");
        }
        e1000_write(E1000_REG_CTRL_EXT, ctrl_ext);

        if (use_msi) {
            static hw_int_controller_t msi_ctrl;
            msi_ctrl.ack      = msi_ack;
            msi_ctrl.enable   = msi_noop_enable;
            msi_ctrl.disable  = msi_noop_disable;
            msi_ctrl.install  = msi_noop_install;
            msi_ctrl.uninstall = msi_noop_uninstall;
            register_irq(0x20 + irq, NULL, e1000_handler, 0, &msi_ctrl, "e1000");
        } else {
            register_irq(0x20 + irq, NULL, &e1000_handler, 0,
                         get_ioapic_controller(), "e1000");
        }
    }

    e1000.initialized = 1;

    // ── Self-interrupt test ─────────────────────────────────
    // Write ICS to force a TXDW interrupt.  In MSI mode the device sends an
    // MSI message directly to the LAPIC.  In INTx mode the IOAPIC handles it.
    log_info("e1000: self-test (ICS=TXDW)...\n");
    e1000_read(E1000_REG_ICR);
    e1000_read(E1000_REG_ICR);
    e1000_write(E1000_REG_ICS, E1000_ICR_TXDW);
    {
        uint32_t vec = 0x20 + irq;
        // Short spin — let interrupt arrive
        for (volatile int i = 0; i < 10000; i++) arch_cpu_pause();
        // Check LAPIC ISR to see if interrupt was received
        uint32_t isr_reg = lapic_read(LAPIC_ISR_BASE + (vec / 32) * 0x10);
        int isr_set = (isr_reg >> (vec % 32)) & 1;
        log_info("e1000: post-ICS vec%u ISR=%u\n", vec, isr_set);
        // Self-IPI as a fallback to verify the IDT/handler path
        log_info("e1000: sending self-IPI vec=0x%x ...\n", vec);
        while (lapic_read(LAPIC_ICR_LOW) & ICR_STATUS_PENDING)
            arch_cpu_pause();
        lapic_write(LAPIC_ICR_HIGH, 0);
        lapic_write(LAPIC_ICR_LOW, (1 << 18) | ICR_DELIVERY_FIXED | vec);
        log_info("e1000: self-IPI sent\n");
    }

    log_info("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ=%u\n",
                e1000.mac[0], e1000.mac[1], e1000.mac[2],
                e1000.mac[3], e1000.mac[4], e1000.mac[5], irq);
    return 0;
}

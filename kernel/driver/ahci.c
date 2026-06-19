#include <driver/ahci.h>
#include <driver/pci.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/interrupt.h>
#include <kernel/apic.h>
#include <device/timer.h>
#include <block/blockdev.h>
#include <string.h>
#include <stdint.h>

// ── Constants ─────────────────────────────────────────────

#define AHCI_MAX_PORTS   32
#define CMD_LIST_ENTRIES 32           // command headers per port

// Offsets within the per-port 2MB DMA page
#define DMA_OFF_CMD_LIST    0x0000    // 1 KB (32 headers × 32 B)
#define DMA_OFF_FIS         0x0400    // 256 B
#define DMA_OFF_CMD_TABLES  0x0800    // 32 tables × 128 B = 4 KB
#define DMA_OFF_IDENTIFY    0x3000    // 512 B
#define DMA_OFF_DATA        0x4000    // data buffer for read/write (up to 1.75 MB)

// Timeout (in jiffies)
#define AHCI_TIMEOUT_JIFFIES  50      // 500 ms at 100 Hz

#define ROUND_UP(v, a)  (((v) + (a) - 1) & ~((a) - 1))

// ── Per-port driver state ─────────────────────────────────

typedef struct {
    int      present;                // 1 if a device was detected
    uint32_t port_num;
    uint64_t dma_phys;               // physical address of the 2MB DMA page
    void    *dma_virt;               // kernel virtual address of the page
    uint16_t identify[256];          // cached IDENTIFY data

    // Drive info parsed from IDENTIFY
    char     model[41];
    char     serial[21];
    uint64_t sector_count;           // in 512-byte sectors
    int      lba48;                  // non-zero if 48-bit LBA supported
} ahci_port_t;

static ahci_port_t ahci_ports[AHCI_MAX_PORTS];

// HBA virtual address — set once in ahci_init()
static HBA_MEM *g_hba = NULL;

// ── Forward declarations ──────────────────────────────────

static void ahci_port_init(HBA_MEM *hba, int port_num);
static int  ahci_identify(HBA_MEM *hba, int port_num, ahci_port_t *p);
static int  ahci_find_free_slot(HBA_PORT *port);

// ── Helper: port pointer ──────────────────────────────────

static inline HBA_PORT *port_regs(HBA_MEM *hba, int n)
{
    return (HBA_PORT *)((uint8_t *)hba + 0x100 + n * 0x80);
}

// ── Helper: busy-wait with jiffies timeout ───────────────
// Evaluates `expr` each iteration; loops while it's non-zero.
#define WAIT_WHILE(expr, timeout_jiffies)                                     \
    ({                                                                        \
        uint64_t _deadline = jiffies + (timeout_jiffies);                     \
        int _ret = 0;                                                         \
        while (expr) {                                                        \
            if (jiffies >= _deadline) { _ret = -1; break; }                   \
            nop();                                                            \
        }                                                                     \
        _ret;                                                                 \
    })

// ── AHCI initialization ───────────────────────────────────

void ahci_init(void)
{
    uint8_t bus, dev, func;
    int ret;

    // Try AHCI progIF first, fall back to any SATA controller
    ret = pci_find_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                          PCI_PROGIF_AHCI, &bus, &dev, &func);
    if (ret != 0) {
        serial_printk("AHCI: AHCI-mode SATA not found, trying any SATA...\n");
        ret = pci_find_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                              0xFF, &bus, &dev, &func);
    }
    if (ret != 0) {
        serial_printk("AHCI: no SATA controller found\n");
        return;
    }

    serial_printk("AHCI: found controller at PCI %d:%d.%d\n", bus, dev, func);

    // Read vendor/device ID
    uint32_t vendor_dev = pci_config_read(bus, dev, func, PCI_VENDOR_ID);
    serial_printk("AHCI: vendor=%#04x device=%#04x\n",
                  vendor_dev & 0xFFFF, vendor_dev >> 16);

    // Enable bus mastering and MMIO space
    pci_enable_bus_mastering(bus, dev, func);
    pci_enable_mmio(bus, dev, func);

    // Read ABAR (BAR5)
    int is_mmio, is_64bit;
    uint64_t abar_phys = pci_read_bar(bus, dev, func, 5, &is_mmio, &is_64bit);
    serial_printk("AHCI: ABAR = %#018lx (mmio=%d, 64bit=%d)\n",
                  abar_phys, is_mmio, is_64bit);

    if (!is_mmio) {
        serial_printk("AHCI: ABAR is not MMIO, aborting\n");
        return;
    }

    // Map the ABAR MMIO region (2MB-aligned)
    uint64_t abar_page = abar_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, abar_page,
                 (uintptr_t)Phy_To_Virt(abar_page), PAGE_KERNEL_MMIO);
    flush_tlb();

    g_hba = (HBA_MEM *)Phy_To_Virt(abar_phys);
    HBA_MEM *hba = g_hba;

    // Read version
    uint32_t vs = hba->vs;
    serial_printk("AHCI: version %u.%u.%u\n",
                  (vs >> 16) & 0xF, (vs >> 8) & 0xF, vs & 0xF);

    // Read capabilities
    uint32_t cap = hba->cap;
    uint32_t cap2 = hba->cap2;
    uint32_t n_ports = AHCI_CAP_NP(cap) + 1;
    uint32_t pi = hba->pi;
    serial_printk("AHCI: %u ports, ports implemented = %#x\n", n_ports, pi);

    // BIOS/OS handoff if supported
    if (cap2 & AHCI_CAP2_BOH(cap2)) {
        serial_printk("AHCI: BIOS/OS handoff supported\n");
        uint32_t bohc = hba->bohc;
        if (bohc & AHCI_BOHC_BOS) {
            // BIOS owns the HBA — request ownership
            hba->bohc = bohc | AHCI_BOHC_OOS;
            serial_printk("AHCI: requesting OS ownership...\n");
            // Spin waiting for BIOS to release
            int timeout = 100000;
            while (hba->bohc & AHCI_BOHC_BOS) {
                if (--timeout == 0) {
                    serial_printk("AHCI: BIOS handoff timeout\n");
                    break;
                }
                nop();
            }
        }
    }

    // Enable AHCI mode
    hba->ghc |= AHCI_GHC_AE;
    serial_printk("AHCI: AHCI mode enabled\n");

    // Enable interrupts at HBA level (if we plan to use them)
    // hba->ghc |= AHCI_GHC_IE;

    // Initialize each implemented port
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (pi & (1U << i)) {
            ahci_port_init(hba, i);
        }
    }

    serial_printk("AHCI: initialization complete\n");
}

// ── Port initialization ───────────────────────────────────

static void ahci_port_init(HBA_MEM *hba, int port_num)
{
    HBA_PORT *port = port_regs(hba, port_num);
    ahci_port_t *ap = &ahci_ports[port_num];
    uint32_t ssts;

    memset(ap, 0, sizeof(*ap));
    ap->port_num = port_num;

    // ── Check device presence ─────────────────────────
    ssts = port->ssts;
    uint32_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    uint32_t ipm = ssts & AHCI_PORT_SSTS_IPM_MASK;

    if (det != AHCI_PORT_SSTS_DET_PRES || ipm != AHCI_PORT_SSTS_IPM_ACTIVE) {
        serial_printk("AHCI: port %d: no device (ssts=%#x)\n", port_num, ssts);
        return;
    }
    serial_printk("AHCI: port %d: device detected (ssts=%#x)\n", port_num, ssts);

    // ── Stop port ─────────────────────────────────────
    port->cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);

    // Wait for FR and CR to clear
    if (WAIT_WHILE(port->cmd & (AHCI_PORT_CMD_FR | AHCI_PORT_CMD_CR), 10) != 0) {
        serial_printk("AHCI: port %d: timeout stopping port (cmd=%#x)\n",
                       port_num, port->cmd);
        // Continue anyway — a reset might help
    }

    // ── Clear SATA error register ─────────────────────
    port->serr = 0xFFFFFFFF;

    // ── Allocate DMA buffer page ──────────────────────
    struct Page *page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!page) {
        serial_printk("AHCI: port %d: failed to allocate DMA page\n", port_num);
        return;
    }
    ap->dma_phys = page->phy_address;
    ap->dma_virt = Phy_To_Virt(ap->dma_phys);

    // Zero out the entire page for clean state
    memset(ap->dma_virt, 0, PAGE_2M_SIZE);

    uint64_t phys = ap->dma_phys;

    // ── Set up Command List ───────────────────────────
    uint64_t clb_phys = phys + DMA_OFF_CMD_LIST;
    port->clb  = (uint32_t)(clb_phys & 0xFFFFFFFF);
    port->clbu = (uint32_t)(clb_phys >> 32);

    // ── Set up Received FIS area ──────────────────────
    uint64_t fb_phys = phys + DMA_OFF_FIS;
    port->fb  = (uint32_t)(fb_phys & 0xFFFFFFFF);
    port->fbu = (uint32_t)(fb_phys >> 32);

    // ── Set up Command Tables for each slot ───────────
    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);

    for (int slot = 0; slot < CMD_LIST_ENTRIES; slot++) {
        uint64_t ct_phys = phys + DMA_OFF_CMD_TABLES + slot * 128;
        cmd_list[slot].ctba  = (uint32_t)(ct_phys & 0xFFFFFFFF);
        cmd_list[slot].ctbau = (uint32_t)(ct_phys >> 32);
    }

    // ── Start port ───────────────────────────────────
    // FIS Receive Enable first, then Start
    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;

    serial_printk("AHCI: port %d: started (cmd=%#x, clb=%#x/%#x, fb=%#x/%#x)\n",
                   port_num, port->cmd,
                   port->clb, port->clbu, port->fb, port->fbu);

    // ── Send IDENTIFY DEVICE ─────────────────────────
    ap->present = 1;
    if (ahci_identify(hba, port_num, ap) == 0) {
        serial_printk("AHCI: port %d: MODEL=%s SERIAL=%s SECTORS=%lu%s\n",
                       port_num, ap->model, ap->serial,
                       ap->sector_count,
                       ap->lba48 ? " (LBA48)" : "");

        // Register as a block device (e.g., "hda", "hdb")
        char name[4] = { 'h', 'd', (char)('a' + port_num), '\0' };
        block_device_register(name, port_num, ap->sector_count);
    } else {
        serial_printk("AHCI: port %d: IDENTIFY failed\n", port_num);
    }
}

// ── IDENTIFY DEVICE command ──────────────────────────────

static int ahci_identify(HBA_MEM *hba, int port_num, ahci_port_t *ap)
{
    HBA_PORT *port = port_regs(hba, port_num);
    uint64_t phys = ap->dma_phys;

    // ── Find a free command slot ─────────────────────
    int slot = ahci_find_free_slot(port);
    if (slot < 0) {
        serial_printk("AHCI: port %d: no free command slot\n", port_num);
        return -1;
    }

    // ── Locate data structures ───────────────────────
    HBA_CMD_HEADER *cmd_header = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_header[slot];

    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)ap->dma_virt
                            + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t buf_phys = phys + DMA_OFF_IDENTIFY;
    uint16_t *buf = (uint16_t *)((uint8_t *)ap->dma_virt + DMA_OFF_IDENTIFY);
    memset(buf, 0, 512);

    // ── Build the Register H2D FIS ───────────────────
    cmd_tbl->cfis[0]  = FIS_TYPE_REG_H2D;    // fis_type
    cmd_tbl->cfis[1]  = 0x80;                 // c=1 (command), pmp=0
    cmd_tbl->cfis[2]  = ATA_CMD_IDENTIFY;     // command
    cmd_tbl->cfis[3]  = 0;                    // feature_low
    cmd_tbl->cfis[7]  = 0x40;                 // device (LBA mode)
    // All other cfis bytes are 0 (already zeroed by memset)

    // ── Build the PRDT entry ─────────────────────────
    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(buf_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = 512 - 1;     // 0-based byte count
    cmd_tbl->prdt_entry[0].i    = 0;            // no interrupt needed for polled

    // ── Build the command header ─────────────────────
    // cfl = Command FIS Length in DWORDS; Register H2D FIS is 5 DWORDS (20 bytes)
    header->cfl    = 5;
    header->w      = 0;          // Device-to-Host (read)
    header->a      = 0;          // Not ATAPI
    header->prdtl  = 1;          // 1 PRDT entry
    // ctba/ctbau were already set in port_init

    // ── Issue the command ────────────────────────────
    port->ci = (1U << slot);

    // ── Wait for completion ──────────────────────────
    uint64_t deadline = jiffies + AHCI_TIMEOUT_JIFFIES;
    while (port->ci & (1U << slot)) {
        nop();
        if (jiffies >= deadline) {
            serial_printk("AHCI: port %d: IDENTIFY timeout (ci=%#x, tfd=%#x, ssts=%#x)\n",
                           port_num, port->ci, port->tfd, port->ssts);
            return -1;
        }
    }

    // ── Check for errors ─────────────────────────────
    if (port->is & AHCI_PORT_IS_TFES) {
        serial_printk("AHCI: port %d: IDENTIFY task file error (tfd=%#x)\n",
                       port_num, port->tfd);
        return -1;
    }

    // ── Parse IDENTIFY data ──────────────────────────
    // Copy the data (it's in the DMA buffer which is in our mapped page)
    memcpy(ap->identify, buf, 512);

    // Model name: words 27-46, 20 characters in big-endian packed format
    for (int i = 0; i < 20; i += 2) {
        uint16_t w = ap->identify[ATA_IDENT_MODEL + i / 2];
        ap->model[i]     = w >> 8;       // first byte is bits 15-8
        ap->model[i + 1] = w & 0xFF;     // second byte is bits 7-0
    }
    ap->model[40] = '\0';

    // Serial number: words 10-19, 10 characters
    for (int i = 0; i < 10; i += 2) {
        uint16_t w = ap->identify[ATA_IDENT_SERIAL + i / 2];
        ap->serial[i]     = w >> 8;
        ap->serial[i + 1] = w & 0xFF;
    }
    ap->serial[20] = '\0';

    // Trim trailing spaces
    for (int i = 39; i >= 0; i--) {
        if (ap->model[i] == ' ')
            ap->model[i] = '\0';
        else if (ap->model[i] != '\0')
            break;
    }
    for (int i = 19; i >= 0; i--) {
        if (ap->serial[i] == ' ')
            ap->serial[i] = '\0';
        else if (ap->serial[i] != '\0')
            break;
    }

    // Sector count — prefer 48-bit LBA
    ap->lba48 = (ap->identify[83] & (1 << 10)) != 0;
    if (ap->lba48) {
        ap->sector_count = (uint64_t)ap->identify[100]
                         | ((uint64_t)ap->identify[101] << 16)
                         | ((uint64_t)ap->identify[102] << 32)
                         | ((uint64_t)ap->identify[103] << 48);
    } else {
        ap->sector_count = (uint64_t)ap->identify[60]
                         | ((uint64_t)ap->identify[61] << 16);
    }

    serial_printk("AHCI: port %d: IDENTIFY OK (%lu MB)\n",
                   port_num, ap->sector_count * 512 / 1024 / 1024);
    return 0;
}

// ── Find a free command slot ──────────────────────────────

static int ahci_find_free_slot(HBA_PORT *port)
{
    uint32_t slots = port->sact | port->ci;
    for (int i = 0; i < 32; i++) {
        if (!(slots & (1U << i)))
            return i;
    }
    return -1;
}

// ── Public API: read sectors ───────────────────────────────
// Uses READ DMA EXT (0x25) with polling.  Supports multi-sector reads
// up to the DMA page capacity.

int ahci_read_sectors(int port_num, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS || !ahci_ports[port_num].present) {
        serial_printk("AHCI: read_sectors: port %d not present\n", port_num);
        return -1;
    }
    if (count == 0) return 0;
    if (!g_hba) return -1;

    ahci_port_t *ap = &ahci_ports[port_num];
    HBA_PORT *port = port_regs(g_hba, port_num);

    // Compute total bytes and check against DMA buffer capacity
    uint32_t total_bytes = count * 512;
    uint32_t max_xfer = PAGE_2M_SIZE - DMA_OFF_DATA;
    if (total_bytes > max_xfer) {
        serial_printk("AHCI: read_sectors: transfer too large (%u > %u)\n",
                       total_bytes, max_xfer);
        return -1;
    }

    // Find a free command slot
    int slot = ahci_find_free_slot(port);
    if (slot < 0) {
        serial_printk("AHCI: read_sectors: port %d no free slot\n", port_num);
        return -1;
    }

    // Locate data structures in the DMA page
    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_list[slot];

    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)ap->dma_virt
                            + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t data_phys = ap->dma_phys + DMA_OFF_DATA;
    uint8_t *data_virt = (uint8_t *)ap->dma_virt + DMA_OFF_DATA;

    // ── Build the Register H2D FIS for READ DMA EXT ──
    cmd_tbl->cfis[0]  = FIS_TYPE_REG_H2D;    // fis_type
    cmd_tbl->cfis[1]  = 0x80;                 // c=1, pmp=0
    cmd_tbl->cfis[2]  = ATA_CMD_READ_DMA_EXT; // command 0x25
    cmd_tbl->cfis[3]  = 0;                    // feature_low
    // LBA [7:0]
    cmd_tbl->cfis[4]  = (uint8_t)(lba);
    cmd_tbl->cfis[5]  = (uint8_t)(lba >> 8);
    cmd_tbl->cfis[6]  = (uint8_t)(lba >> 16);
    cmd_tbl->cfis[7]  = 0x40;                 // device (LBA mode)
    cmd_tbl->cfis[8]  = (uint8_t)(lba >> 24);
    cmd_tbl->cfis[9]  = (uint8_t)(lba >> 32);
    cmd_tbl->cfis[10] = (uint8_t)(lba >> 40);
    cmd_tbl->cfis[11] = 0;                    // feature_high
    // Sector count
    cmd_tbl->cfis[12] = (uint8_t)(count);
    cmd_tbl->cfis[13] = (uint8_t)(count >> 8);
    // cfis[14-19] are control/icc/reserved — keep 0

    // ── Build the PRDT entry ─────────────────────────
    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(data_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = total_bytes - 1;   // 0-based
    cmd_tbl->prdt_entry[0].i    = 0;                  // no interrupt

    // ── Build the command header ─────────────────────
    header->cfl    = 5;            // FIS length: 5 DWORDS
    header->w      = 0;            // D2H (read)
    header->prdtl  = 1;            // 1 PRDT entry
    // ctba/ctbau already set in port_init

    // ── Issue the command ────────────────────────────
    port->ci = (1U << slot);

    // ── Wait for completion ──────────────────────────
    uint64_t deadline = jiffies + AHCI_TIMEOUT_JIFFIES;
    while (port->ci & (1U << slot)) {
        nop();
        if (jiffies >= deadline) {
            serial_printk("AHCI: read_sectors: port %d timeout (ci=%#x, tfd=%#x)\n",
                           port_num, port->ci, port->tfd);
            return -1;
        }
    }

    // ── Check for errors ─────────────────────────────
    if (port->is & AHCI_PORT_IS_TFES) {
        serial_printk("AHCI: read_sectors: port %d task file error (tfd=%#x)\n",
                       port_num, port->tfd);
        return -1;
    }

    // ── Copy data from DMA buffer to caller's buffer ──
    memcpy(buffer, data_virt, total_bytes);
    return 0;
}

// ── Public API: write sectors ──────────────────────────────
// Uses WRITE DMA EXT (0x35) with polling.  Same structure as read but
// copies data FROM caller INTO DMA buffer BEFORE issuing the command,
// and uses w=1 (Host-to-Device).

int ahci_write_sectors(int port_num, uint64_t lba, uint32_t count, const void *buffer)
{
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS || !ahci_ports[port_num].present) {
        serial_printk("AHCI: write_sectors: port %d not present\n", port_num);
        return -1;
    }
    if (count == 0) return 0;
    if (!g_hba) return -1;

    ahci_port_t *ap = &ahci_ports[port_num];
    HBA_PORT *port = port_regs(g_hba, port_num);

    uint32_t total_bytes = count * 512;
    uint32_t max_xfer = PAGE_2M_SIZE - DMA_OFF_DATA;
    if (total_bytes > max_xfer) {
        serial_printk("AHCI: write_sectors: transfer too large (%u > %u)\n",
                       total_bytes, max_xfer);
        return -1;
    }

    int slot = ahci_find_free_slot(port);
    if (slot < 0) {
        serial_printk("AHCI: write_sectors: port %d no free slot\n", port_num);
        return -1;
    }

    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_list[slot];

    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)ap->dma_virt
                            + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t data_phys = ap->dma_phys + DMA_OFF_DATA;
    uint8_t *data_virt = (uint8_t *)ap->dma_virt + DMA_OFF_DATA;

    // Copy caller data INTO DMA buffer BEFORE issuing write
    memcpy(data_virt, buffer, total_bytes);

    // ── Build the Register H2D FIS for WRITE DMA EXT ──
    cmd_tbl->cfis[0]  = FIS_TYPE_REG_H2D;      // fis_type
    cmd_tbl->cfis[1]  = 0x80;                   // c=1, pmp=0
    cmd_tbl->cfis[2]  = ATA_CMD_WRITE_DMA_EXT;  // command 0x35
    cmd_tbl->cfis[3]  = 0;                      // feature_low
    cmd_tbl->cfis[4]  = (uint8_t)(lba);
    cmd_tbl->cfis[5]  = (uint8_t)(lba >> 8);
    cmd_tbl->cfis[6]  = (uint8_t)(lba >> 16);
    cmd_tbl->cfis[7]  = 0x40;                   // device (LBA mode)
    cmd_tbl->cfis[8]  = (uint8_t)(lba >> 24);
    cmd_tbl->cfis[9]  = (uint8_t)(lba >> 32);
    cmd_tbl->cfis[10] = (uint8_t)(lba >> 40);
    cmd_tbl->cfis[11] = 0;                      // feature_high
    cmd_tbl->cfis[12] = (uint8_t)(count);
    cmd_tbl->cfis[13] = (uint8_t)(count >> 8);

    // ── Build the PRDT entry ─────────────────────────
    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(data_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = total_bytes - 1;
    cmd_tbl->prdt_entry[0].i    = 0;

    // ── Build the command header ─────────────────────
    header->cfl    = 5;            // FIS length: 5 DWORDS
    header->w      = 1;            // H2D (write) — key difference from read
    header->prdtl  = 1;

    // ── Issue the command ────────────────────────────
    port->ci = (1U << slot);

    // ── Wait for completion ──────────────────────────
    uint64_t deadline = jiffies + AHCI_TIMEOUT_JIFFIES;
    while (port->ci & (1U << slot)) {
        nop();
        if (jiffies >= deadline) {
            serial_printk("AHCI: write_sectors: port %d timeout (ci=%#x, tfd=%#x)\n",
                           port_num, port->ci, port->tfd);
            return -1;
        }
    }

    if (port->is & AHCI_PORT_IS_TFES) {
        serial_printk("AHCI: write_sectors: port %d task file error (tfd=%#x)\n",
                       port_num, port->tfd);
        return -1;
    }

    return 0;
}

// ── Port info accessors ────────────────────────────────────

int ahci_port_present(int port_num)
{
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return 0;
    return ahci_ports[port_num].present;
}

uint64_t ahci_port_sector_count(int port_num)
{
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return 0;
    return ahci_ports[port_num].sector_count;
}

#include <driver/ahci.h>
#include <driver/ahci_lifecycle.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/pci.h>
#include <block/blockdev.h>
#include <core/debug.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/slab.h>
#include <arch/io.h>
#include <arch/cpu.h>
#include <intr/interrupt.h>
#include <time/clocksource.h>
#ifndef OS01_HOST_TEST
#include <arch/x86_64/clocksource.h>
#endif
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <log/log.h>

// ── Offsets within per-port 2MB DMA page ──────────────────
#define DMA_OFF_CMD_LIST    0x0000    // 1 KB (32 headers × 32 B)
#define DMA_OFF_FIS         0x0400    // 256 B
#define DMA_OFF_CMD_TABLES  0x0800    // 32 tables × 128 B = 4 KB
#define DMA_OFF_IDENTIFY    0x3000    // 512 B
#define DMA_OFF_DATA        0x4000    // data buffer for read/write
#define CMD_LIST_ENTRIES    32

static struct ahci_controller *g_first_controller = NULL;

// ── Forward declarations ──────────────────────────────────
static void ahci_port_init(struct ahci_controller *ctrl, int port_num);
static int  ahci_identify(struct ahci_controller *ctrl, int port_num, struct ahci_port *ap);
static int  ahci_find_free_slot(HBA_PORT *port);
static int  ahci_alloc_disk_name(char *out_name, size_t max_len);
int         ahci_port_stop_engine(struct ahci_port *port, bool boot_phase);
void        ahci_port_teardown_dma(struct ahci_port *port, bool boot_phase);

static inline HBA_PORT *port_regs(HBA_MEM *hba, int n)
{
    return (HBA_PORT *)((uint8_t *)hba + 0x100 + n * 0x80);
}

// ── Block device operations for AHCI ──────────────────────
static int ahci_bdev_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *port = (struct ahci_port *)dev->private_data;
    if (!port) return -EINVAL;
    return ahci_port_read(port, lba, count, buf);
}

static int ahci_bdev_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *port = (struct ahci_port *)dev->private_data;
    if (!port) return -EINVAL;
    return ahci_port_write(port, lba, count, buf);
}

static int ahci_bdev_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

static const struct block_device_ops ahci_bdev_ops = {
    .read = ahci_bdev_read,
    .write = ahci_bdev_write,
    .flush = ahci_bdev_flush,
};

// ── Disk naming helper ────────────────────────────────────
static int ahci_alloc_disk_name(char *out_name, size_t max_len)
{
    for (char c = 'a'; c <= 'z'; c++) {
        char candidate[BLOCKDEV_NAME_MAX];
        candidate[0] = 'h';
        candidate[1] = 'd';
        candidate[2] = c;
        candidate[3] = '\0';

        bool exists = false;
        int count = block_device_count();
        for (int i = 0; i < count; i++) {
            block_device_t *bdev = block_device_get(i);
            if (bdev && strcmp(bdev->name, candidate) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            strncpy(out_name, candidate, max_len - 1);
            out_name[max_len - 1] = '\0';
            return 0;
        }
    }
    return -ENOSPC;
}

// ── Stop port engine with bounded timeout ─────────────────
int ahci_port_stop_engine(struct ahci_port *port, bool boot_phase)
{
    if (!port || !port->ctrl || !port->ctrl->hba) return -EINVAL;
    HBA_PORT *p = port_regs(port->ctrl->hba, port->port_num);

    ahci_write32(&p->cmd, p->cmd & ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE));

    struct ahci_deadline dl;
    int r = ahci_deadline_start(boot_phase, AHCI_BUDGET_STOP_MS, &dl);
    if (r != 0) {
        return r;
    }

    while (p->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) {
        if (ahci_deadline_expired(&dl)) {
            debug_block("AHCI: port %d engine stop timeout (cmd=%#x)\n",
                        port->port_num, p->cmd);
            return -ETIMEDOUT;
        }
        arch_nop();
    }
    return 0;
}

// ── Safe teardown & DMA quarantine ────────────────────────
void ahci_port_teardown_dma(struct ahci_port *port, bool boot_phase)
{
    if (!port) return;
    int err = ahci_port_stop_engine(port, boot_phase);
    if (err != 0) {
        // Safe DMA quarantine: do not free pages if engine cannot be proven stopped
        port->quarantined = true;
        if (port->ctrl && port->ctrl->pdev) {
            device_quarantine(&port->ctrl->pdev->dev, port);
        }
        log_warn("AHCI: port %d DMA quarantined: stop engine failed (%d)\n",
                 port->port_num, err);
    } else {
        if (port->dma_page) {
            free_pages(port->dma_page, 1);
            port->dma_page = NULL;
            port->dma_virt = NULL;
            port->dma_phys = 0;
        }
    }
}

// ── Public port read ──────────────────────────────────────
int ahci_port_read(struct ahci_port *port, uint64_t lba, uint32_t count, void *buffer)
{
    if (!port) return -EINVAL;
    if (count > AHCI_MAX_XFER_SECTORS) return -E2BIG;
    if (count == 0) return 0;
    if (!buffer) return -EINVAL;

    if (!port->ctrl || !port->ctrl->hba) return -EIO;
    if (port->state == AHCI_PORT_STATE_FAILED || port->quarantined || !port->present) {
        return -EIO;
    }

    if (!clocksource_active || clocksource_freq_hz() == 0) {
        return -ENOTSUP;
    }

    struct ahci_deadline gate_dl;
    int r = ahci_deadline_start(false, AHCI_BUDGET_GATE_MS, &gate_dl);
    if (r != 0) return r;

    r = ahci_port_claim(port, &gate_dl);
    if (r != 0) return r;

    uint32_t total_bytes = count * 512;
    HBA_MEM *hba = port->ctrl->hba;
    HBA_PORT *p = port_regs(hba, port->port_num);

    int slot = ahci_find_free_slot(p);
    if (slot < 0) {
        ahci_port_release(port);
        return -EBUSY;
    }

    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)port->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_list[slot];
    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)port->dma_virt + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t data_phys = port->dma_phys + DMA_OFF_DATA;
    uint8_t *data_virt = (uint8_t *)port->dma_virt + DMA_OFF_DATA;

    cmd_tbl->cfis[0]  = FIS_TYPE_REG_H2D;
    cmd_tbl->cfis[1]  = 0x80;
    cmd_tbl->cfis[2]  = ATA_CMD_READ_DMA_EXT;
    cmd_tbl->cfis[3]  = 0;
    cmd_tbl->cfis[4]  = (uint8_t)(lba);
    cmd_tbl->cfis[5]  = (uint8_t)(lba >> 8);
    cmd_tbl->cfis[6]  = (uint8_t)(lba >> 16);
    cmd_tbl->cfis[7]  = 0x40;
    cmd_tbl->cfis[8]  = (uint8_t)(lba >> 24);
    cmd_tbl->cfis[9]  = (uint8_t)(lba >> 32);
    cmd_tbl->cfis[10] = (uint8_t)(lba >> 40);
    cmd_tbl->cfis[11] = 0;
    cmd_tbl->cfis[12] = (uint8_t)(count);
    cmd_tbl->cfis[13] = (uint8_t)(count >> 8);

    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(data_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = total_bytes - 1;
    cmd_tbl->prdt_entry[0].i    = 0;

    header->cfl   = 5;
    header->w     = 0;
    header->prdtl = 1;

    ahci_record_dma_start();
    ahci_write32(&p->ci, (1U << slot));

    struct ahci_deadline cmd_dl;
    r = ahci_deadline_start(false, AHCI_BUDGET_CMD_MS, &cmd_dl);
    if (r != 0) {
        ahci_port_fail_escalate(port, r);
        ahci_port_release(port);
        return r;
    }

    bool timed_out = false;
    while (p->ci & (1U << slot)) {
        if (ahci_deadline_expired(&cmd_dl)) {
            timed_out = true;
            break;
        }
        arch_nop();
    }

    if (timed_out) {
        debug_block("AHCI: port %d read timeout\n", port->port_num);
        ahci_port_fail(port, -ETIMEDOUT);
        ahci_port_teardown_dma(port, false);
        ahci_port_release(port);
        return -ETIMEDOUT;
    }

    if (p->is & AHCI_PORT_IS_TFES) {
        debug_block("AHCI: port %d read TFES error\n", port->port_num);
        ahci_port_fail(port, -EIO);
        ahci_port_release(port);
        return -EIO;
    }

    memcpy(buffer, data_virt, total_bytes);
    ahci_port_release(port);
    return 0;
}

// ── Public port write ─────────────────────────────────────
int ahci_port_write(struct ahci_port *port, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!port) return -EINVAL;
    if (count > AHCI_MAX_XFER_SECTORS) return -E2BIG;
    if (count == 0) return 0;
    if (!buffer) return -EINVAL;

    if (!port->ctrl || !port->ctrl->hba) return -EIO;
    if (port->state == AHCI_PORT_STATE_FAILED || port->quarantined || !port->present) {
        return -EIO;
    }

    if (!clocksource_active || clocksource_freq_hz() == 0) {
        return -ENOTSUP;
    }

    struct ahci_deadline gate_dl;
    int r = ahci_deadline_start(false, AHCI_BUDGET_GATE_MS, &gate_dl);
    if (r != 0) return r;

    r = ahci_port_claim(port, &gate_dl);
    if (r != 0) return r;

    uint32_t total_bytes = count * 512;
    HBA_MEM *hba = port->ctrl->hba;
    HBA_PORT *p = port_regs(hba, port->port_num);

    int slot = ahci_find_free_slot(p);
    if (slot < 0) {
        ahci_port_release(port);
        return -EBUSY;
    }

    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)port->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_list[slot];
    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)port->dma_virt + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t data_phys = port->dma_phys + DMA_OFF_DATA;
    uint8_t *data_virt = (uint8_t *)port->dma_virt + DMA_OFF_DATA;

    ahci_record_bounce_write();
    memcpy(data_virt, buffer, total_bytes);

    cmd_tbl->cfis[0]  = FIS_TYPE_REG_H2D;
    cmd_tbl->cfis[1]  = 0x80;
    cmd_tbl->cfis[2]  = ATA_CMD_WRITE_DMA_EXT;
    cmd_tbl->cfis[3]  = 0;
    cmd_tbl->cfis[4]  = (uint8_t)(lba);
    cmd_tbl->cfis[5]  = (uint8_t)(lba >> 8);
    cmd_tbl->cfis[6]  = (uint8_t)(lba >> 16);
    cmd_tbl->cfis[7]  = 0x40;
    cmd_tbl->cfis[8]  = (uint8_t)(lba >> 24);
    cmd_tbl->cfis[9]  = (uint8_t)(lba >> 32);
    cmd_tbl->cfis[10] = (uint8_t)(lba >> 40);
    cmd_tbl->cfis[11] = 0;
    cmd_tbl->cfis[12] = (uint8_t)(count);
    cmd_tbl->cfis[13] = (uint8_t)(count >> 8);

    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(data_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = total_bytes - 1;
    cmd_tbl->prdt_entry[0].i    = 0;

    header->cfl   = 5;
    header->w     = 1;
    header->prdtl = 1;

    ahci_record_dma_start();
    ahci_write32(&p->ci, (1U << slot));

    struct ahci_deadline cmd_dl;
    r = ahci_deadline_start(false, AHCI_BUDGET_CMD_MS, &cmd_dl);
    if (r != 0) {
        ahci_port_fail_escalate(port, r);
        ahci_port_release(port);
        return r;
    }

    bool timed_out = false;
    while (p->ci & (1U << slot)) {
        if (ahci_deadline_expired(&cmd_dl)) {
            timed_out = true;
            break;
        }
        arch_nop();
    }

    if (timed_out) {
        debug_block("AHCI: port %d write timeout\n", port->port_num);
        ahci_port_fail(port, -ETIMEDOUT);
        ahci_port_teardown_dma(port, false);
        ahci_port_release(port);
        return -ETIMEDOUT;
    }

    if (p->is & AHCI_PORT_IS_TFES) {
        debug_block("AHCI: port %d write TFES error\n", port->port_num);
        ahci_port_fail(port, -EIO);
        ahci_port_release(port);
        return -EIO;
    }

    ahci_port_release(port);
    return 0;
}

// ── Port initialization ───────────────────────────────────
static void ahci_port_init(struct ahci_controller *ctrl, int port_num)
{
    HBA_PORT *port = port_regs(ctrl->hba, port_num);
    struct ahci_port *ap = &ctrl->ports[port_num];

    ap->ctrl = ctrl;
    ap->port_num = port_num;

    // Check device presence
    uint32_t ssts = port->ssts;
    uint32_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    uint32_t ipm = ssts & AHCI_PORT_SSTS_IPM_MASK;

    if (det != AHCI_PORT_SSTS_DET_PRES || ipm != AHCI_PORT_SSTS_IPM_ACTIVE) {
        debug_block("AHCI: port %d: no device (ssts=%#x)\n", port_num, ssts);
        ap->present = 0;
        return;
    }

    debug_block("AHCI: port %d: device detected (ssts=%#x)\n", port_num, ssts);

    // Stop port before setup
    if (ahci_port_stop_engine(ap, true) != 0) {
        debug_block("AHCI: port %d: stop before setup timed out\n", port_num);
    }

    // Clear SATA error register
    ahci_write32(&port->serr, 0xFFFFFFFF);

    // Allocate 2MB DMA buffer page
    struct Page *page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!page) {
        debug_block("AHCI: port %d: failed to allocate DMA page\n", port_num);
        return;
    }
    ap->dma_page = page;
    ap->dma_phys = page->phy_address;
    ap->dma_virt = Phy_To_Virt(ap->dma_phys);
    memset(ap->dma_virt, 0, PAGE_2M_SIZE);

    uint64_t phys = ap->dma_phys;

    // Set up Command List
    uint64_t clb_phys = phys + DMA_OFF_CMD_LIST;
    ahci_write32(&port->clb, (uint32_t)(clb_phys & 0xFFFFFFFF));
    ahci_write32(&port->clbu, (uint32_t)(clb_phys >> 32));

    // Set up Received FIS area
    uint64_t fb_phys = phys + DMA_OFF_FIS;
    ahci_write32(&port->fb, (uint32_t)(fb_phys & 0xFFFFFFFF));
    ahci_write32(&port->fbu, (uint32_t)(fb_phys >> 32));

    // Set up Command Tables for each slot
    HBA_CMD_HEADER *cmd_list = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);
    for (int slot = 0; slot < CMD_LIST_ENTRIES; slot++) {
        uint64_t ct_phys = phys + DMA_OFF_CMD_TABLES + slot * 128;
        cmd_list[slot].ctba  = (uint32_t)(ct_phys & 0xFFFFFFFF);
        cmd_list[slot].ctbau = (uint32_t)(ct_phys >> 32);
    }

    // Start port: FRE first, then ST
    ahci_write32(&port->cmd, port->cmd | AHCI_PORT_CMD_FRE);
    ahci_write32(&port->cmd, port->cmd | AHCI_PORT_CMD_ST);

    ap->present = 1;

    // Send IDENTIFY DEVICE
    if (ahci_identify(ctrl, port_num, ap) == 0) {
        debug_block("AHCI: port %d: MODEL=%s SERIAL=%s SECTORS=%lu%s\n",
                    port_num, ap->model, ap->serial,
                    ap->sector_count, ap->lba48 ? " (LBA48)" : "");

        char name[BLOCKDEV_NAME_MAX];
        int nret = ahci_alloc_disk_name(name, sizeof(name));
        if (nret != 0) {
            debug_block("AHCI: port %d: naming failed: %d\n", port_num, nret);
            ahci_port_teardown_dma(ap, true);
            return;
        }

        struct block_device_desc desc = {
            .name = name,
            .sector_count = ap->sector_count,
            .sector_size = 512,
            .ops = &ahci_bdev_ops,
            .private_data = ap,
            .parent = NULL,
            .kind = BLOCK_DISK,
        };

        int bret = block_device_register(&desc, &ap->bdev);
        if (bret != 0) {
            debug_block("AHCI: port %d: block_device_register failed: %d\n", port_num, bret);
            ap->bdev = NULL;
            ahci_port_teardown_dma(ap, true);
            return;
        }
        ap->state = AHCI_PORT_STATE_ACTIVE;
    } else {
        debug_block("AHCI: port %d: IDENTIFY failed, tearing down\n", port_num);
        ahci_port_teardown_dma(ap, true);
    }
}

// ── IDENTIFY DEVICE command ──────────────────────────────
static int ahci_identify(struct ahci_controller *ctrl, int port_num, struct ahci_port *ap)
{
    HBA_PORT *port = port_regs(ctrl->hba, port_num);
    uint64_t phys = ap->dma_phys;

    int slot = ahci_find_free_slot(port);
    if (slot < 0) {
        debug_block("AHCI: port %d: no free slot for IDENTIFY\n", port_num);
        return -1;
    }

    HBA_CMD_HEADER *cmd_header = (HBA_CMD_HEADER *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_LIST);
    HBA_CMD_HEADER *header = &cmd_header[slot];

    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL *)((uint8_t *)ap->dma_virt + DMA_OFF_CMD_TABLES + slot * 128);
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL));

    uint64_t buf_phys = phys + DMA_OFF_IDENTIFY;
    uint16_t *buf = (uint16_t *)((uint8_t *)ap->dma_virt + DMA_OFF_IDENTIFY);
    memset(buf, 0, 512);

    cmd_tbl->cfis[0] = FIS_TYPE_REG_H2D;
    cmd_tbl->cfis[1] = 0x80;
    cmd_tbl->cfis[2] = ATA_CMD_IDENTIFY;
    cmd_tbl->cfis[3] = 0;
    cmd_tbl->cfis[7] = 0x40;

    cmd_tbl->prdt_entry[0].dba  = (uint32_t)(buf_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc  = 512 - 1;
    cmd_tbl->prdt_entry[0].i    = 0;

    header->cfl   = 5;
    header->w     = 0;
    header->a     = 0;
    header->prdtl = 1;

    ahci_record_dma_start();
    ahci_write32(&port->ci, (1U << slot));

    struct ahci_deadline cmd_dl;
    int r = ahci_deadline_start(true, AHCI_BUDGET_CMD_MS, &cmd_dl);
    if (r != 0) return r;

    while (port->ci & (1U << slot)) {
        if (ahci_deadline_expired(&cmd_dl)) {
            debug_block("AHCI: port %d IDENTIFY timeout\n", port_num);
            return -ETIMEDOUT;
        }
        arch_nop();
    }

#ifdef OS01_HOST_TEST
    if (s_simulate_identify_fail) {
        port->is |= AHCI_PORT_IS_TFES;
    } else {
        buf[60] = 100000 & 0xFFFF;
        buf[61] = (100000 >> 16) & 0xFFFF;
        buf[ATA_IDENT_MODEL] = ('O' << 8) | 'S';
        buf[ATA_IDENT_MODEL + 1] = ('0' << 8) | '1';
    }
#endif

    if (port->is & AHCI_PORT_IS_TFES) {
        debug_block("AHCI: port %d IDENTIFY TFES error\n", port_num);
        return -EIO;
    }

    memcpy(ap->identify, buf, 512);

    for (int i = 0; i < 20; i += 2) {
        uint16_t w = ap->identify[ATA_IDENT_MODEL + i / 2];
        ap->model[i]     = w >> 8;
        ap->model[i + 1] = w & 0xFF;
    }
    ap->model[40] = '\0';

    for (int i = 0; i < 10; i += 2) {
        uint16_t w = ap->identify[ATA_IDENT_SERIAL + i / 2];
        ap->serial[i]     = w >> 8;
        ap->serial[i + 1] = w & 0xFF;
    }
    ap->serial[20] = '\0';

    for (int i = 39; i >= 0; i--) {
        if (ap->model[i] == ' ') ap->model[i] = '\0';
        else if (ap->model[i] != '\0') break;
    }
    for (int i = 19; i >= 0; i--) {
        if (ap->serial[i] == ' ') ap->serial[i] = '\0';
        else if (ap->serial[i] != '\0') break;
    }

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

    return 0;
}

// ── Find free command slot ────────────────────────────────
static int ahci_find_free_slot(HBA_PORT *port)
{
    uint32_t slots = port->sact | port->ci;
    for (int i = 0; i < 32; i++) {
        if (!(slots & (1U << i)))
            return i;
    }
    return -1;
}

// ── Driver probe ──────────────────────────────────────────
int ahci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) return -EINVAL;

    // Check clocksource validity BEFORE any hardware modification
    if (!clocksource_active || clocksource_freq_hz() == 0) {
        return -ENOTSUP;
    }

    struct ahci_controller *ctrl = kmalloc(sizeof(struct ahci_controller));
    if (!ctrl) return -ENOMEM;
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->pdev = pdev;
    pdev->driver_data = ctrl;

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        ctrl->ports[i].ctrl = ctrl;
        ctrl->ports[i].port_num = i;
        ctrl->ports[i].state = AHCI_PORT_STATE_INITIAL;
        spin_init(&ctrl->ports[i].lock);
    }

    int ret = pci_set_bus_master(pdev, true);
    if (ret != 0) {
        pdev->driver_data = NULL;
        kfree(ctrl);
        return ret;
    }

    ret = pci_set_decode(pdev, false, true);
    if (ret != 0) {
        pci_set_bus_master(pdev, false);
        pdev->driver_data = NULL;
        kfree(ctrl);
        return ret;
    }

    uint64_t abar_phys = 0;
    ret = pci_bar_window(pdev, 5, PCI_BAR_MMIO32, 0, sizeof(HBA_MEM), &abar_phys);
    if (ret != 0) {
        ret = pci_bar_window(pdev, 5, PCI_BAR_MMIO64, 0, sizeof(HBA_MEM), &abar_phys);
    }
    if (ret != 0 && pdev->bars[5].valid) {
        abar_phys = pdev->bars[5].address;
        ret = 0;
    }
    if (ret != 0) {
        pci_set_bus_master(pdev, false);
        pdev->driver_data = NULL;
        kfree(ctrl);
        return ret;
    }

    ctrl->abar_phys = abar_phys;

#ifndef OS01_HOST_TEST
    uint64_t abar_page = abar_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, abar_page, (uintptr_t)Phy_To_Virt(abar_page), PAGE_KERNEL_PMD_NOCACHE);
    flush_tlb();
#endif

    ctrl->hba = (HBA_MEM *)Phy_To_Virt(abar_phys);
    HBA_MEM *hba = ctrl->hba;

    // BIOS/OS handoff
    if (hba->cap2 & AHCI_CAP2_BOH(hba->cap2)) {
        if (hba->bohc & AHCI_BOHC_BOS) {
            ahci_write32(&hba->bohc, hba->bohc | AHCI_BOHC_OOS);
            struct ahci_deadline handoff_dl;
            if (ahci_deadline_start(true, AHCI_BUDGET_HANDOFF_MS, &handoff_dl) == 0) {
                while (hba->bohc & AHCI_BOHC_BOS) {
                    if (ahci_deadline_expired(&handoff_dl)) {
                        debug_block("AHCI: BIOS handoff timeout\n");
                        break;
                    }
                    arch_nop();
                }
            }
        }
    }

    // Enable AHCI mode
    ahci_write32(&hba->ghc, hba->ghc | AHCI_GHC_AE);

    // Route IRQ
    uint32_t gsi = 0;
    if (pci_route_gsi(pdev, &gsi) == 0) {
        ctrl->irq = gsi;
    }

    uint32_t pi = hba->pi;
    ctrl->pi = pi;
    ctrl->nports = AHCI_CAP_NP(hba->cap) + 1;

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (pi & (1U << i)) {
            ahci_port_init(ctrl, i);
        }
    }

    if (!g_first_controller) {
        g_first_controller = ctrl;
    }

    return 0;
}

// ── Driver remove ─────────────────────────────────────────
void ahci_remove(struct pci_device *pdev)
{
    if (!pdev) return;
    struct ahci_controller *ctrl = (struct ahci_controller *)pdev->driver_data;
    if (!ctrl) return;

    if (g_first_controller == ctrl) {
        g_first_controller = NULL;
    }

    // Reverse order teardown:
    // 1. Device stop & unregister block devices
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        struct ahci_port *port = &ctrl->ports[i];
        if (port->bdev) {
            block_device_unregister_boot(port->bdev);
            port->bdev = NULL;
        }
        if (port->present && ctrl->hba) {
            ahci_port_teardown_dma(port, true);
        }
    }

    // 2. 自有 IRQ 撤销
    pci_interrupts_disable(pdev);
    if (ctrl->irq_registered) {
        unregister_irq(ctrl->irq);
        ctrl->irq_registered = false;
    }

    // 3. Disable bus mastering
    pci_set_bus_master(pdev, false);

    // 4. Private free
    pdev->driver_data = NULL;
    kfree(ctrl);
}

// ── Transitional ahci_init() ──────────────────────────────
void ahci_init(void)
{
    device_core_init();
    int ret = pci_enumerate();
    if (ret != 0 && ret != BUS_UNAVAILABLE) {
        debug_block("AHCI: pci_enumerate failed: %d\n", ret);
        return;
    }

    static const struct pci_device_id ahci_ids[2] = {
        {
            .vendor = PCI_ID_ANY,
            .device = PCI_ID_ANY,
            .subvendor = PCI_ID_ANY,
            .subdevice = PCI_ID_ANY,
            .class_value = (PCI_CLASS_MASS_STORAGE << 16) | (PCI_SUBCLASS_SATA << 8) | PCI_PROGIF_AHCI,
            .class_mask = 0xFFFFFF,
        },
        {
            .vendor = PCI_ID_ANY,
            .device = PCI_ID_ANY,
            .subvendor = PCI_ID_ANY,
            .subdevice = PCI_ID_ANY,
            .class_value = (PCI_CLASS_MASS_STORAGE << 16) | (PCI_SUBCLASS_SATA << 8) | 0x00,
            .class_mask = 0xFFFF00,
        },
    };

    static const struct pci_driver ahci_driver = {
        .name = "ahci",
        .id_table = ahci_ids,
        .id_count = sizeof(ahci_ids) / sizeof(ahci_ids[0]),
        .probe = ahci_probe,
        .remove = ahci_remove,
    };

    unsigned count = pci_device_count();
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *pdev = pci_device_get(i);
        if (!pdev) continue;
        const struct pci_device_id *matched = pci_match_id(&ahci_driver, pdev);
        if (matched) {
            debug_block("AHCI: found device at %02x:%02x.%d, probing...\n",
                        pdev->bus, pdev->slot, pdev->fn);
            pdev->driver = &ahci_driver;
            int pr = ahci_probe(pdev, matched);
            if (pr == 0) {
                pdev->dev.state = DEV_BOUND;
                debug_block("AHCI: successfully bound controller\n");
            } else {
                pdev->dev.state = DEV_FAILED;
                pdev->dev.last_error = pr;
                pdev->driver = NULL;
                debug_block("AHCI: probe failed: %d\n", pr);
            }
            break;
        }
    }
}

// ── Legacy public APIs ────────────────────────────────────
int ahci_read_sectors(int port_num, uint64_t lba, uint32_t count, void *buffer)
{
    if (count == 0) return 0;
    if (!g_first_controller) return -EIO;
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return -EINVAL;
    return ahci_port_read(&g_first_controller->ports[port_num], lba, count, buffer);
}

int ahci_write_sectors(int port_num, uint64_t lba, uint32_t count, const void *buffer)
{
    if (count == 0) return 0;
    if (!g_first_controller) return -EIO;
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return -EINVAL;
    return ahci_port_write(&g_first_controller->ports[port_num], lba, count, buffer);
}

int ahci_port_present(int port_num)
{
    if (!g_first_controller) return 0;
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return 0;
    return g_first_controller->ports[port_num].present;
}

uint64_t ahci_port_sector_count(int port_num)
{
    if (!g_first_controller) return 0;
    if (port_num < 0 || port_num >= AHCI_MAX_PORTS) return 0;
    return g_first_controller->ports[port_num].sector_count;
}

#ifndef OS01_HOST_TEST
#include <subsys/subsys.h>
static int _ahci_init_wrapper(void)
{
    ahci_init();
    return 0;
}
static int _ahci_register(void)
{
    register_subsys("ahci", _ahci_init_wrapper,
                    SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
    return 0;
}
SUBSYS_INITCALL(_ahci_register);
#endif

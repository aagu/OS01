/*
 * kernel/driver/bga.c
 *
 * PCI Bochs Graphic Adapter (BGA) Backend Driver for QEMU Standard VGA.
 * (Spec §2.1, §2.2, §2.3, §4)
 */

#include <driver/bga.h>
#include <driver/fb_state.h>
#include <driver/fb_test.h>
#include <arch/x86_64/fb_map.h>
#include <core/printk.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <arch/io.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Standard candidate modes (Spec §2.3) */
static const struct fb_info s_candidate_modes[] = {
    {  640,  480,  640 * 4, 32, FB_FORMAT_RGB32 },
    {  800,  600,  800 * 4, 32, FB_FORMAT_RGB32 },
    { 1024,  768, 1024 * 4, 32, FB_FORMAT_RGB32 },
    { 1280,  720, 1280 * 4, 32, FB_FORMAT_RGB32 },
    { 1280,  800, 1280 * 4, 32, FB_FORMAT_RGB32 },
    { 1280, 1024, 1280 * 4, 32, FB_FORMAT_RGB32 },
    { 1440,  900, 1440 * 4, 32, FB_FORMAT_RGB32 },
    { 1600,  900, 1600 * 4, 32, FB_FORMAT_RGB32 },
    { 1920, 1080, 1920 * 4, 32, FB_FORMAT_RGB32 },
};

#define CANDIDATE_MODES_COUNT (sizeof(s_candidate_modes) / sizeof(s_candidate_modes[0]))

uint32_t bga_filter_modes(const bga_caps_t *caps, uint64_t mapped_size,
                          struct fb_info out[FB_MAX_MODES])
{
    if (!caps || !out || caps->max_bpp < 32) {
        return 0;
    }

    uint64_t effective_capacity = caps->vram_bytes < mapped_size ? caps->vram_bytes : mapped_size;
    uint32_t count = 0;

    for (size_t i = 0; i < CANDIDATE_MODES_COUNT; i++) {
        const struct fb_info *cand = &s_candidate_modes[i];
        if (cand->width > caps->max_width || cand->height > caps->max_height) {
            continue;
        }

        uint64_t req_bytes = (uint64_t)cand->width * 4ULL * (uint64_t)cand->height;
        if (req_bytes > effective_capacity) {
            continue;
        }

        if (count < FB_MAX_MODES) {
            out[count++] = *cand;
        }
    }

    return count;
}

#if !defined(__x86_64__) || defined(OS01_HOST_TEST_NON_X86)

/* ── Non-x86 Architecture Stub (Spec §1, §4) ─────────────────────── */
/* Non-x86 branches must return -ENODEV and contain no x86 port operations. */

int bga_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev;
    (void)id;
    return -ENODEV;
}

enum bga_result bga_apply_mode(const struct fb_info *target)
{
    (void)target;
    return BGA_FAILED;
}

static const struct pci_device_id bga_pci_ids[] = {
    {
        .vendor = BGA_PCI_VENDOR_ID,
        .device = BGA_PCI_DEVICE_ID,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (PCI_CLASS_DISPLAY << 16) | (PCI_SUBCLASS_VGA << 8),
        .class_mask = 0xFFFF00,
    },
};

const struct pci_driver bga_pci_driver = {
    .name = "bga",
    .id_table = bga_pci_ids,
    .id_count = sizeof(bga_pci_ids) / sizeof(bga_pci_ids[0]),
    .probe = bga_probe,
    .remove = NULL,
};

PCI_DRIVER_DECLARE(bga_pci_driver);

#ifdef OS01_HOST_TEST
void bga_set_transport_for_test(bga_io_read_fn r_io, bga_io_write_fn w_io,
                                bga_pci_read_fn r_pci, bga_pci_write_fn w_pci)
{
    (void)r_io; (void)w_io; (void)r_pci; (void)w_pci;
}

void bga_reset_for_test(void)
{
}
#endif

#else

/* ── x86_64 Production & x86 Host Test Implementation ───────────── */

static bool g_bga_bound = false;

#ifdef OS01_HOST_TEST
static uint16_t default_io_read16(uint16_t port) { (void)port; return 0; }
static void default_io_write16(uint16_t port, uint16_t val) { (void)port; (void)val; }
static int default_pci_read32(struct pci_device *pdev, uint16_t off, uint32_t *val)
{
    (void)pdev; (void)off; if (val) *val = 0; return 0;
}
static int default_pci_write32(struct pci_device *pdev, uint16_t off, uint32_t val)
{
    (void)pdev; (void)off; (void)val; return 0;
}

static bga_io_read_fn   s_io_read16 = default_io_read16;
static bga_io_write_fn  s_io_write16 = default_io_write16;
static bga_pci_read_fn  s_pci_read32 = default_pci_read32;
static bga_pci_write_fn s_pci_write32 = default_pci_write32;

void bga_set_transport_for_test(bga_io_read_fn r_io, bga_io_write_fn w_io,
                                bga_pci_read_fn r_pci, bga_pci_write_fn w_pci)
{
    s_io_read16 = r_io ? r_io : default_io_read16;
    s_io_write16 = w_io ? w_io : default_io_write16;
    s_pci_read32 = r_pci ? r_pci : default_pci_read32;
    s_pci_write32 = w_pci ? w_pci : default_pci_write32;
}

void bga_reset_for_test(void)
{
    g_bga_bound = false;
    s_io_read16 = default_io_read16;
    s_io_write16 = default_io_write16;
    s_pci_read32 = default_pci_read32;
    s_pci_write32 = default_pci_write32;
}

#define bga_inw(port)          s_io_read16(port)
#define bga_outw(port, val)    s_io_write16(port, val)
#define bga_pci_read(p, o, v)  s_pci_read32(p, o, v)
#define bga_pci_write(p, o, v) s_pci_write32(p, o, v)

#else

#define bga_inw(port)          arch_inw(port)
#define bga_outw(port, val)    arch_outw(port, val)
#define bga_pci_read(p, o, v)  pci_config_read32(p, o, v)
#define bga_pci_write(p, o, v) pci_config_write32(p, o, v)

#endif

int bga_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) {
        return -EINVAL;
    }

    /* Accept at most one matching backend */
    if (g_bga_bound) {
        return -ENODEV;
    }

    /* 1. Close writer admission and drain writers during boot probe */
    int tr_rc = fb_transition_begin(true);
    if (tr_rc != 0) {
        return -EBUSY;
    }

    /* 2. Read initial BAR0 and command */
    uint32_t orig_bar0 = 0;
    if (bga_pci_read(pdev, 0x10, &orig_bar0) != 0) {
        fb_transition_end();
        return -ENODEV;
    }

    /* Reject I/O BAR */
    if (orig_bar0 & 0x01) {
        fb_transition_end();
        return -ENODEV;
    }

    bool is_64 = ((orig_bar0 & 0x06) == 0x04);
    uint32_t orig_bar1 = 0;
    if (is_64) {
        if (bga_pci_read(pdev, 0x14, &orig_bar1) != 0) {
            fb_transition_end();
            return -ENODEV;
        }
    }

    uint64_t bar_base = (orig_bar0 & 0xFFFFFFF0ULL);
    if (is_64) {
        bar_base |= ((uint64_t)orig_bar1 << 32);
    }

    /* Check GOP base matching */
    if (bar_base != (uint64_t)Pos.Phy_addr) {
        fb_transition_end();
        return -ENODEV;
    }

    uint32_t cmd_status = 0;
    if (bga_pci_read(pdev, 0x04, &cmd_status) != 0) {
        fb_transition_end();
        return -ENODEV;
    }
    uint16_t orig_cmd = cmd_status & 0xFFFF;

    /* 3. Disable decode (clear bit 0 IO, bit 1 MMIO). Write low 16 only. */
    if (bga_pci_write(pdev, 0x04, (orig_cmd & ~0x03) & 0xFFFF) != 0) {
        fb_transition_end();
        return -ENODEV;
    }

    /* Sizing probe */
    uint32_t mask0 = 0;
    bga_pci_write(pdev, 0x10, 0xFFFFFFFFU);
    bga_pci_read(pdev, 0x10, &mask0);

    uint32_t mask1 = 0;
    if (is_64) {
        bga_pci_write(pdev, 0x14, 0xFFFFFFFFU);
        bga_pci_read(pdev, 0x14, &mask1);
    }

    /* Restore BAR(s) */
    bga_pci_write(pdev, 0x10, orig_bar0);
    if (is_64) {
        bga_pci_write(pdev, 0x14, orig_bar1);
    }

    /* Restore Command */
    bga_pci_write(pdev, 0x04, orig_cmd & 0xFFFF);

    /* Read back verification */
    uint32_t v_bar0 = 0, v_bar1 = 0, v_cmd = 0;
    bga_pci_read(pdev, 0x10, &v_bar0);
    if (is_64) {
        bga_pci_read(pdev, 0x14, &v_bar1);
    }
    bga_pci_read(pdev, 0x04, &v_cmd);

    if (v_bar0 != orig_bar0 || (is_64 && v_bar1 != orig_bar1) || (v_cmd & 0xFFFF) != orig_cmd) {
        fb_mark_failed();
        return -EIO;
    }

    /* Sizing math */
    uint64_t bar_size = 0;
    if (!is_64) {
        uint32_t m0 = mask0 & 0xFFFFFFF0U;
        if (m0 == 0) {
            fb_transition_end();
            return -ENODEV;
        }
        bar_size = (~m0) + 1;
        if ((m0 | (bar_size - 1)) != 0xFFFFFFFFU) {
            fb_transition_end();
            return -ENODEV;
        }
    } else {
        uint64_t m = ((uint64_t)mask1 << 32) | (mask0 & 0xFFFFFFF0ULL);
        if (m == 0) {
            fb_transition_end();
            return -ENODEV;
        }
        bar_size = (~m) + 1;
        if ((m | (bar_size - 1)) != 0xFFFFFFFFFFFFFFFFULL) {
            fb_transition_end();
            return -ENODEV;
        }
    }

    if (bar_base + bar_size < bar_base || bar_base + bar_size < bar_size) {
        fb_transition_end();
        return -ENODEV;
    }

    /* 4. DISPI handshake */
    uint16_t orig_index = bga_inw(VBE_DISPI_IOPORT_INDEX);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    uint16_t orig_id = bga_inw(VBE_DISPI_IOPORT_DATA);

    bga_outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_ID5);
    uint16_t id5_read = bga_inw(VBE_DISPI_IOPORT_DATA);
    if (id5_read != VBE_DISPI_ID5) {
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* Read VIDEO_MEMORY_64K */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    uint16_t mem_64k = bga_inw(VBE_DISPI_IOPORT_DATA);
    uint64_t bga_vram_bytes = ((uint64_t)mem_64k) * 64ULL * 1024ULL;
    if (bga_vram_bytes == 0) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* Read GETCAPS */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    uint16_t orig_enable = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_DATA, orig_enable | VBE_DISPI_GETCAPS);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    uint32_t max_w = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    uint32_t max_h = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    uint32_t max_bpp = bga_inw(VBE_DISPI_IOPORT_DATA);

    /* Restore ENABLE immediately */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    bga_outw(VBE_DISPI_IOPORT_DATA, orig_enable);

    if (max_w == 0 || max_h == 0 || max_bpp < 32) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* 5. Check initial layout */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    uint16_t cur_bpp = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    uint16_t cur_x_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t cur_y_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t cur_virt_w = bga_inw(VBE_DISPI_IOPORT_DATA);

    if (cur_bpp != 32 || cur_x_off != 0 || cur_y_off != 0 || cur_virt_w != (uint16_t)Pos.XResolution) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* 6. Verify capacity against initial framebuffer */
    uint64_t vram_capacity = bar_size < bga_vram_bytes ? bar_size : bga_vram_bytes;
    uint64_t initial_fb_size = (uint64_t)Pos.XResolution * (uint64_t)Pos.YResolution * 4ULL;
    if (vram_capacity < initial_fb_size) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* 7. Checked mapping of full VRAM */
    uint32_t *mapped_addr = NULL;
    int map_rc = fb_x86_map_checked(bar_base, vram_capacity, &mapped_addr);
    if (map_rc != 0) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        fb_transition_end();
        return -ENODEV;
    }

    /* 8. Install backend into coordinator */
    bga_caps_t caps = {
        .vram_bytes = vram_capacity,
        .max_width = max_w,
        .max_height = max_h,
        .max_bpp = max_bpp
    };
    int inst_rc = fb_install_backend(&caps, mapped_addr, vram_capacity);

    /* Restore original ID and index on all probe exits */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    bga_outw(VBE_DISPI_IOPORT_DATA, orig_id);
    bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);

    if (inst_rc != 0) {
        fb_transition_end();
        return inst_rc;
    }

    g_bga_bound = true;
    pdev->driver_data = (void *)1;
    return 0;
}

enum bga_result bga_apply_mode(const struct fb_info *target)
{
    if (!g_bga_bound || !target || target->bpp != 32) {
        return BGA_FAILED;
    }

    /* 1. Save full valid old layout */
    uint16_t orig_index = bga_inw(VBE_DISPI_IOPORT_INDEX);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    uint16_t old_enable = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    uint16_t old_xres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    uint16_t old_yres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    uint16_t old_bpp = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t old_virt_w = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t old_virt_h = bga_inw(VBE_DISPI_IOPORT_DATA);
    (void)old_virt_h;
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    uint16_t old_x_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t old_y_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BANK);
    uint16_t old_bank = bga_inw(VBE_DISPI_IOPORT_DATA);

    /* 2. Disable */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    bga_outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_DISABLED);

    /* 3. Program new mode */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    bga_outw(VBE_DISPI_IOPORT_DATA, (uint16_t)target->width);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    bga_outw(VBE_DISPI_IOPORT_DATA, (uint16_t)target->height);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    bga_outw(VBE_DISPI_IOPORT_DATA, 32);

    /* Enable with ENABLED | LFB_ENABLED | NOCLEARMEM */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    bga_outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BANK);
    bga_outw(VBE_DISPI_IOPORT_DATA, 0);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    bga_outw(VBE_DISPI_IOPORT_DATA, (uint16_t)target->width);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    bga_outw(VBE_DISPI_IOPORT_DATA, 0);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    bga_outw(VBE_DISPI_IOPORT_DATA, 0);

    /* 4. Read back verification */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    uint16_t rb_enable = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    uint16_t rb_xres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    uint16_t rb_yres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    uint16_t rb_bpp = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t rb_virt_w = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t rb_virt_h = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    uint16_t rb_x_off = fb_test_filter_readback(FB_TEST_STEP_APPLY,
                                                VBE_DISPI_INDEX_X_OFFSET,
                                                bga_inw(VBE_DISPI_IOPORT_DATA));
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t rb_y_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BANK);
    uint16_t rb_bank = bga_inw(VBE_DISPI_IOPORT_DATA);

    bool match = (rb_xres == (uint16_t)target->width &&
                  rb_yres == (uint16_t)target->height &&
                  rb_bpp == 32 &&
                  rb_virt_w == (uint16_t)target->width &&
                  rb_x_off == 0 &&
                  rb_y_off == 0 &&
                  rb_bank == 0 &&
                  rb_virt_h >= (uint16_t)target->height &&
                  (rb_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) ==
                  (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));

    if (match) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
        return BGA_APPLIED;
    }

    /* 5. Read back mismatch: Rollback to old layout with NOCLEARMEM */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    bga_outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_DISABLED);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_xres);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_yres);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_bpp);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_enable | VBE_DISPI_NOCLEARMEM);

    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BANK);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_bank);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_virt_w);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_x_off);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    bga_outw(VBE_DISPI_IOPORT_DATA, old_y_off);

    /* 6. Verify rollback read back */
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    uint16_t roll_enable = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    uint16_t roll_xres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    uint16_t roll_yres = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    uint16_t roll_bpp = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t roll_virt_w = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t roll_virt_h = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_X_OFFSET);
    uint16_t roll_x_off = fb_test_filter_readback(FB_TEST_STEP_ROLLBACK,
                                                  VBE_DISPI_INDEX_X_OFFSET,
                                                  bga_inw(VBE_DISPI_IOPORT_DATA));
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t roll_y_off = bga_inw(VBE_DISPI_IOPORT_DATA);
    bga_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BANK);
    uint16_t roll_bank = bga_inw(VBE_DISPI_IOPORT_DATA);

    bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);

    bool roll_match = (roll_xres == old_xres &&
                       roll_yres == old_yres &&
                       roll_bpp == old_bpp &&
                       roll_virt_w == old_virt_w &&
                       roll_x_off == old_x_off &&
                       roll_y_off == old_y_off &&
                       roll_bank == old_bank &&
                       roll_virt_h >= old_yres &&
                       (roll_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) ==
                       (old_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)));

    if (roll_match) {
        return BGA_ROLLED_BACK;
    }

    return BGA_FAILED;
}

#ifdef FB_RESOLUTION_TEST
/* Sample DISPI registers index 0..10 in order, restoring the caller's
 * index.  Only the test build links this; the caller (fb_test.c SNAPSHOT)
 * holds the display mutex. */
int bga_sample_regs(uint16_t out[11])
{
    if (!g_bga_bound || !out) {
        return -ENODEV;
    }
    uint16_t orig_index = bga_inw(VBE_DISPI_IOPORT_INDEX);
    for (uint16_t i = 0; i <= (uint16_t)VBE_DISPI_INDEX_VIDEO_MEMORY_64K; i++) {
        bga_outw(VBE_DISPI_IOPORT_INDEX, i);
        out[i] = bga_inw(VBE_DISPI_IOPORT_DATA);
    }
    bga_outw(VBE_DISPI_IOPORT_INDEX, orig_index);
    return 0;
}
#endif

static const struct pci_device_id bga_pci_ids[] = {
    {
        .vendor = BGA_PCI_VENDOR_ID,
        .device = BGA_PCI_DEVICE_ID,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (PCI_CLASS_DISPLAY << 16) | (PCI_SUBCLASS_VGA << 8),
        .class_mask = 0xFFFF00,
    },
};

const struct pci_driver bga_pci_driver = {
    .name = "bga",
    .id_table = bga_pci_ids,
    .id_count = sizeof(bga_pci_ids) / sizeof(bga_pci_ids[0]),
    .probe = bga_probe,
    .remove = NULL,
};

PCI_DRIVER_DECLARE(bga_pci_driver);

#endif

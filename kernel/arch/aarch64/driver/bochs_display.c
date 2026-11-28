/*
 * kernel/arch/aarch64/driver/bochs_display.c
 *
 * PCIe Bochs Display (QEMU -device bochs-display) MMIO backend for aarch64.
 * Provides bga_filter_modes() and bga_apply_mode() via BAR2 DISPI MMIO
 * registers (+0x500) so setres / FBIOGET_MODES / FBIOSET_MODE work on aarch64.
 */

#include <driver/bga.h>
#include <driver/fb_state.h>
#include <driver/fb_test.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <core/printk.h>
#include <log/log.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define BOCHS_MMIO_VBE_OFFSET 0x0500U
#define BOCHS_MMIO_BAR_INDEX  2U
#define BOCHS_VRAM_BAR_INDEX  0U
#define BOCHS_MMIO_MIN_SIZE   0x1000ULL

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

static bool s_bochs_bound = false;
static volatile uint8_t *s_mmio_base = NULL;

static inline uint16_t bochs_dispi_read(uint16_t index)
{
    volatile const uint16_t *reg = (volatile const uint16_t *)
        (s_mmio_base + BOCHS_MMIO_VBE_OFFSET + ((uint32_t)index << 1));
    return *reg;
}

static inline void bochs_dispi_write(uint16_t index, uint16_t val)
{
    volatile uint16_t *reg = (volatile uint16_t *)
        (s_mmio_base + BOCHS_MMIO_VBE_OFFSET + ((uint32_t)index << 1));
    *reg = val;
}

static void bochs_program_mode(uint16_t width, uint16_t height, uint16_t bpp,
                               uint16_t enable_flags)
{
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_dispi_write(VBE_DISPI_INDEX_XRES, width);
    bochs_dispi_write(VBE_DISPI_INDEX_YRES, height);
    bochs_dispi_write(VBE_DISPI_INDEX_BPP, bpp);
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, enable_flags);
    bochs_dispi_write(VBE_DISPI_INDEX_BANK, 0);
    bochs_dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    bochs_dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    bochs_dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
}

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

static uint64_t bochs_probe_bar_size(struct pci_device *pdev, uint8_t bar_idx, bool is_64)
{
    uint16_t off0 = (uint16_t)(0x10U + (uint16_t)bar_idx * 4U);
    uint16_t off1 = (uint16_t)(off0 + 4U);

    uint32_t orig_lo = 0U, orig_hi = 0U, cmd = 0U;
    if (pci_config_read32(pdev, off0, &orig_lo) != 0 ||
        (is_64 && pci_config_read32(pdev, off1, &orig_hi) != 0) ||
        pci_config_read32(pdev, 0x04U, &cmd) != 0) {
        return 0ULL;
    }

    uint16_t orig_cmd = (uint16_t)(cmd & 0xFFFFU);
    (void)pci_config_write32(pdev, 0x04U, (uint32_t)(orig_cmd & ~0x3U));
    (void)pci_config_write32(pdev, off0, 0xFFFFFFFFU);
    if (is_64) {
        (void)pci_config_write32(pdev, off1, 0xFFFFFFFFU);
    }

    uint32_t mask_lo = 0U, mask_hi = 0U;
    (void)pci_config_read32(pdev, off0, &mask_lo);
    if (is_64) {
        (void)pci_config_read32(pdev, off1, &mask_hi);
    }

    (void)pci_config_write32(pdev, off0, orig_lo);
    if (is_64) {
        (void)pci_config_write32(pdev, off1, orig_hi);
    }
    (void)pci_config_write32(pdev, 0x04U, orig_cmd);

    if (!is_64) {
        uint32_t m0 = mask_lo & 0xFFFFFFF0U;
        return m0 ? ((uint64_t)(~m0) + 1ULL) : 0ULL;
    }
    uint64_t m = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & 0xFFFFFFF0U);
    return m ? ((~m) + 1ULL) : 0ULL;
}

static int bochs_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) {
        return -EINVAL;
    }
    if (s_bochs_bound) {
        return -ENODEV;
    }

    const struct pci_bar *vram_bar = &pdev->bars[BOCHS_VRAM_BAR_INDEX];
    const struct pci_bar *mmio_bar = &pdev->bars[BOCHS_MMIO_BAR_INDEX];
    if (!vram_bar->valid ||
        (vram_bar->kind != PCI_BAR_MMIO32 && vram_bar->kind != PCI_BAR_MMIO64)) {
        log_warn("[bochs-display] invalid BAR0 kind=%d valid=%d\n",
                 (int)vram_bar->kind, (int)vram_bar->valid);
        return -ENODEV;
    }
    if (!mmio_bar->valid ||
        (mmio_bar->kind != PCI_BAR_MMIO32 && mmio_bar->kind != PCI_BAR_MMIO64)) {
        log_warn("[bochs-display] invalid BAR2 kind=%d valid=%d\n",
                 (int)mmio_bar->kind, (int)mmio_bar->valid);
        return -ENODEV;
    }

    uint64_t vram_bar_size = bochs_probe_bar_size(pdev, BOCHS_VRAM_BAR_INDEX,
                                                  vram_bar->kind == PCI_BAR_MMIO64);
    if (vram_bar_size == 0ULL) {
        log_warn("[bochs-display] BAR0 size=0\n");
        return -ENODEV;
    }

    /* If another framebuffer (e.g. ramfb) is already active at a different
     * physical address, do not hijack it. */
    bool has_gop = (Pos.FB_addr != NULL);
    if (has_gop && vram_bar->address != (uint64_t)(uintptr_t)Pos.Phy_addr) {
        return -ENODEV;
    }

    /* Ensure PCI memory space decoding is enabled (EDK2 may leave
     * bochs-display disabled if no GOP driver bound to it). */
    uint32_t cmd_status = 0;
    if (pci_config_read32(pdev, 0x04U, &cmd_status) != 0) {
        return -ENODEV;
    }
    uint16_t orig_cmd = (uint16_t)(cmd_status & 0xFFFFU);
    if ((orig_cmd & 0x02U) == 0U) {
        if (pci_config_write32(pdev, 0x04U, (uint32_t)(orig_cmd | 0x02U)) != 0) {
            return -ENODEV;
        }
    }

    void *mmio_va = NULL;
    int mmio_rc = pci_iomap_range(pdev, BOCHS_MMIO_BAR_INDEX, 0ULL, BOCHS_MMIO_MIN_SIZE, &mmio_va);
    if (mmio_rc != 0 || !mmio_va) {
        log_warn("[bochs-display] BAR2 iomap failed rc=%d addr=0x%llx\n",
                 mmio_rc, (unsigned long long)mmio_bar->address);
        return -ENODEV;
    }
    s_mmio_base = (volatile uint8_t *)mmio_va;

    /* DISPI handshake */
    uint16_t orig_id = bochs_dispi_read(VBE_DISPI_INDEX_ID);
    bochs_dispi_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    uint16_t read_id = bochs_dispi_read(VBE_DISPI_INDEX_ID);
    if (read_id != VBE_DISPI_ID5) {
        log_warn("[bochs-display] DISPI ID mismatch orig=0x%04x read=0x%04x\n",
                 orig_id, read_id);
        bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
        s_mmio_base = NULL;
        return -ENODEV;
    }

    uint16_t mem_64k = bochs_dispi_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    uint64_t bga_vram_bytes = ((uint64_t)mem_64k) * 64ULL * 1024ULL;
    if (bga_vram_bytes == 0ULL) {
        log_warn("[bochs-display] bga_vram_bytes=0\n");
        bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
        s_mmio_base = NULL;
        return -ENODEV;
    }

    uint16_t orig_enable = bochs_dispi_read(VBE_DISPI_INDEX_ENABLE);
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, orig_enable | VBE_DISPI_GETCAPS);
    uint32_t max_w = bochs_dispi_read(VBE_DISPI_INDEX_XRES);
    uint32_t max_h = bochs_dispi_read(VBE_DISPI_INDEX_YRES);
    uint32_t max_bpp = bochs_dispi_read(VBE_DISPI_INDEX_BPP);
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, orig_enable);

    /* QEMU -device bochs-display does not implement VBE_DISPI_GETCAPS and
     * returns 0 before a mode is programmed; fall back to VRAM-bounded caps. */
    if (max_w == 0U || max_h == 0U || max_bpp < 32U) {
        max_w = 1920U;
        max_h = 1200U;
        max_bpp = 32U;
    }

    uint64_t vram_capacity = vram_bar_size < bga_vram_bytes ? vram_bar_size : bga_vram_bytes;

    void *vram_va = NULL;
    int vram_rc = pci_iomap_range(pdev, BOCHS_VRAM_BAR_INDEX, 0ULL, vram_capacity, &vram_va);
    if (vram_rc != 0 || !vram_va) {
        log_warn("[bochs-display] BAR0 iomap failed rc=%d addr=0x%llx cap=0x%llx\n",
                 vram_rc, (unsigned long long)vram_bar->address,
                 (unsigned long long)vram_capacity);
        bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
        s_mmio_base = NULL;
        return -ENODEV;
    }

    bga_caps_t caps = {
        .vram_bytes = vram_capacity,
        .max_width = max_w,
        .max_height = max_h,
        .max_bpp = max_bpp,
    };

    if (!has_gop) {
        struct fb_info modes[FB_MAX_MODES];
        uint32_t nmodes = bga_filter_modes(&caps, vram_capacity, modes);
        if (nmodes == 0U) {
            bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
            s_mmio_base = NULL;
            return -ENODEV;
        }
        /* Pick 1024x768 if available, otherwise the first valid mode. */
        struct fb_info init_info = modes[0];
        for (uint32_t i = 0; i < nmodes; i++) {
            if (modes[i].width == 1024U && modes[i].height == 768U) {
                init_info = modes[i];
                break;
            }
        }

        bochs_program_mode((uint16_t)init_info.width, (uint16_t)init_info.height, 32U,
                           VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        memset(vram_va, 0, (size_t)init_info.width * (size_t)init_info.height * 4U);

        uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
        Pos.Phy_addr = (uint32_t *)(uintptr_t)vram_bar->address;
        Pos.FB_addr = (uint32_t *)vram_va;
        Pos.FB_length = vram_capacity;
        Pos.XResolution = init_info.width;
        Pos.YResolution = init_info.height;
        Pos.XPosition = 0;
        Pos.YPosition = 0;
        spin_unlock_irqrestore(&Pos.lock, pos_flags);

        fb_bootstrap_state(vram_bar->address, vram_capacity, &init_info);
        fb_publish_initial_mapping((uint32_t *)vram_va, vram_capacity);
    } else {
        uint64_t initial_fb_size = (uint64_t)Pos.XResolution * (uint64_t)Pos.YResolution * 4ULL;
        if (vram_capacity < initial_fb_size) {
            bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
            s_mmio_base = NULL;
            return -ENODEV;
        }
        if (fb_transition_begin(true) != 0) {
            bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
            s_mmio_base = NULL;
            return -EBUSY;
        }
    }

    int inst_rc = fb_install_backend(&caps, (uint32_t *)vram_va, vram_capacity);
    if (inst_rc != 0) {
        log_warn("[bochs-display] fb_install_backend failed rc=%d\n", inst_rc);
        if (has_gop) {
            fb_transition_end();
        }
        bochs_dispi_write(VBE_DISPI_INDEX_ID, orig_id);
        s_mmio_base = NULL;
        return inst_rc;
    }

    s_bochs_bound = true;
    pdev->driver_data = (void *)1;
    log_info("[bochs-display] bound %ux%u (vram=%u MiB, max=%ux%u)\n",
             (unsigned)Pos.XResolution, (unsigned)Pos.YResolution,
             (unsigned)(vram_capacity >> 20),
             (unsigned)max_w, (unsigned)max_h);
    return 0;
}

enum bga_result bga_apply_mode(const struct fb_info *target)
{
    if (!s_bochs_bound || !s_mmio_base || !target || target->bpp != 32U) {
        return BGA_FAILED;
    }

    uint16_t old_enable = bochs_dispi_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t old_xres   = bochs_dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t old_yres   = bochs_dispi_read(VBE_DISPI_INDEX_YRES);
    uint16_t old_bpp    = bochs_dispi_read(VBE_DISPI_INDEX_BPP);
    uint16_t old_virt_w = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t old_virt_h = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t old_x_off  = bochs_dispi_read(VBE_DISPI_INDEX_X_OFFSET);
    uint16_t old_y_off  = bochs_dispi_read(VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t old_bank   = bochs_dispi_read(VBE_DISPI_INDEX_BANK);

    bochs_program_mode((uint16_t)target->width, (uint16_t)target->height, 32U,
                       VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    uint16_t rb_enable = bochs_dispi_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t rb_xres   = bochs_dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t rb_yres   = bochs_dispi_read(VBE_DISPI_INDEX_YRES);
    uint16_t rb_bpp    = bochs_dispi_read(VBE_DISPI_INDEX_BPP);
    uint16_t rb_virt_w = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t rb_virt_h = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t rb_x_off  = fb_test_filter_readback(FB_TEST_STEP_APPLY,
                                                 VBE_DISPI_INDEX_X_OFFSET,
                                                 bochs_dispi_read(VBE_DISPI_INDEX_X_OFFSET));
    uint16_t rb_y_off  = bochs_dispi_read(VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t rb_bank   = bochs_dispi_read(VBE_DISPI_INDEX_BANK);

    bool match = (rb_xres == (uint16_t)target->width &&
                  rb_yres == (uint16_t)target->height &&
                  rb_bpp == 32U &&
                  rb_virt_w == (uint16_t)target->width &&
                  rb_x_off == 0U &&
                  rb_y_off == 0U &&
                  rb_bank == 0U &&
                  rb_virt_h >= (uint16_t)target->height &&
                  (rb_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) ==
                  (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));

    if (match) {
        return BGA_APPLIED;
    }

    /* Rollback to previous mode with NOCLEARMEM */
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_dispi_write(VBE_DISPI_INDEX_XRES, old_xres);
    bochs_dispi_write(VBE_DISPI_INDEX_YRES, old_yres);
    bochs_dispi_write(VBE_DISPI_INDEX_BPP, old_bpp);
    bochs_dispi_write(VBE_DISPI_INDEX_ENABLE, old_enable | VBE_DISPI_NOCLEARMEM);
    bochs_dispi_write(VBE_DISPI_INDEX_BANK, old_bank);
    bochs_dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, old_virt_w);
    bochs_dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, old_virt_h);
    bochs_dispi_write(VBE_DISPI_INDEX_X_OFFSET, old_x_off);
    bochs_dispi_write(VBE_DISPI_INDEX_Y_OFFSET, old_y_off);

    uint16_t roll_enable = bochs_dispi_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t roll_xres   = bochs_dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t roll_yres   = bochs_dispi_read(VBE_DISPI_INDEX_YRES);
    uint16_t roll_bpp    = bochs_dispi_read(VBE_DISPI_INDEX_BPP);
    uint16_t roll_virt_w = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t roll_virt_h = bochs_dispi_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t roll_x_off  = fb_test_filter_readback(FB_TEST_STEP_ROLLBACK,
                                                   VBE_DISPI_INDEX_X_OFFSET,
                                                   bochs_dispi_read(VBE_DISPI_INDEX_X_OFFSET));
    uint16_t roll_y_off  = bochs_dispi_read(VBE_DISPI_INDEX_Y_OFFSET);
    uint16_t roll_bank   = bochs_dispi_read(VBE_DISPI_INDEX_BANK);

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

    return roll_match ? BGA_ROLLED_BACK : BGA_FAILED;
}

static const struct pci_device_id bochs_display_pci_ids[] = {
    {
        .vendor = BGA_PCI_VENDOR_ID,
        .device = BGA_PCI_DEVICE_ID,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (PCI_CLASS_DISPLAY << 16) | (PCI_SUBCLASS_OTHER << 8),
        .class_mask = 0xFFFF00,
    },
    {
        .vendor = BGA_PCI_VENDOR_ID,
        .device = BGA_PCI_DEVICE_ID,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (PCI_CLASS_DISPLAY << 16) | (PCI_SUBCLASS_VGA << 8),
        .class_mask = 0xFFFF00,
    },
};

const struct pci_driver bochs_display_pci_driver = {
    .name = "bochs-display",
    .id_table = bochs_display_pci_ids,
    .id_count = sizeof(bochs_display_pci_ids) / sizeof(bochs_display_pci_ids[0]),
    .probe = bochs_probe,
    .remove = NULL,
};

PCI_DRIVER_DECLARE(bochs_display_pci_driver);

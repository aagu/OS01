/*
 * hosttests/cases/test_bga_resolution.c
 *
 * Host test suite for PCI BGA backend probe and hardware transaction
 * (QEMU Resolution Switcher Task 4).
 * (Spec §2.1, §2.2, §2.3, §4)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <driver/bga.h>
#include <driver/fb_state.h>
#include <core/printk.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

position Pos;

/* ── Hardware Simulation State ── */
#define FAKE_PCI_BAR0_BASE 0xE0000000ULL
#define FAKE_PCI_CMD_DEFAULT 0x0003U /* MMIO + IO decode enabled */
#define FAKE_PCI_STATUS_DEFAULT 0x02900000U /* Upper 16 bits */

static uint32_t s_pci_conf[16];
static uint32_t s_pci_cmd_writes[32];
static size_t   s_pci_cmd_writes_count = 0;

static uint16_t s_dispi_regs[16];
static uint16_t s_dispi_index = 0;
static uint16_t s_fake_max_w = 1920;
static uint16_t s_fake_max_h = 1080;
static uint16_t s_fake_max_bpp = 32;

/* Fault Injection Toggles */
static bool s_inject_bar_mask_zero = false;
static bool s_inject_bar_restore_fail = false;
static bool s_inject_cmd_restore_fail = false;
static bool s_inject_id_mismatch = false;
static bool s_inject_vram_zero = false;
static bool s_inject_getcaps_bpp16 = false;
static bool s_inject_initial_bpp_bad = false;
static bool s_inject_initial_offset_bad = false;
static bool s_inject_initial_virt_w_bad = false;
static bool s_inject_apply_readback_virt_w = false;
static bool s_inject_rollback_readback_fail = false;
static bool s_seen_apply_noclearmem = false;
static bool s_seen_rollback_noclearmem = false;
typedef enum {
    FAKE_BGA_IDLE = 0,
    FAKE_BGA_PROGRAMMING,
    FAKE_BGA_APPLY_VERIFY,
    FAKE_BGA_ROLLING_BACK,
} fake_bga_phase_t;

static fake_bga_phase_t s_phase = FAKE_BGA_IDLE;

static void fake_hw_reset(void)
{
    memset(s_pci_conf, 0, sizeof(s_pci_conf));
    s_pci_conf[0] = 0x11111234U; /* Vendor=0x1234, Device=0x1111 */
    s_pci_conf[1] = FAKE_PCI_STATUS_DEFAULT | FAKE_PCI_CMD_DEFAULT; /* Command (low 16) | Status (high 16) */
    s_pci_conf[2] = 0x03000000U; /* Class 0x03 (Display), Subclass 0x00 (VGA) */
    s_pci_conf[4] = (uint32_t)(FAKE_PCI_BAR0_BASE | 0x08U); /* 32-bit Prefetchable Memory BAR */
    s_pci_conf[5] = 0;

    s_pci_cmd_writes_count = 0;
    memset(s_pci_cmd_writes, 0, sizeof(s_pci_cmd_writes));

    memset(s_dispi_regs, 0, sizeof(s_dispi_regs));
    s_dispi_index = 0;
    s_dispi_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s_dispi_regs[VBE_DISPI_INDEX_XRES] = 800;
    s_dispi_regs[VBE_DISPI_INDEX_YRES] = 600;
    s_dispi_regs[VBE_DISPI_INDEX_BPP] = 32;
    s_dispi_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED;
    s_dispi_regs[VBE_DISPI_INDEX_BANK] = 0;
    s_dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = 800;
    s_dispi_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = 4096;
    s_dispi_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
    s_dispi_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
    s_dispi_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = 256; /* 256 * 64KB = 16MB */

    s_fake_max_w = 1920;
    s_fake_max_h = 1080;
    s_fake_max_bpp = 32;

    s_inject_bar_mask_zero = false;
    s_inject_bar_restore_fail = false;
    s_inject_cmd_restore_fail = false;
    s_inject_id_mismatch = false;
    s_inject_vram_zero = false;
    s_inject_getcaps_bpp16 = false;
    s_inject_initial_bpp_bad = false;
    s_inject_initial_offset_bad = false;
    s_inject_initial_virt_w_bad = false;
    s_inject_apply_readback_virt_w = false;
    s_inject_rollback_readback_fail = false;
    s_seen_apply_noclearmem = false;
    s_seen_rollback_noclearmem = false;
    s_phase = FAKE_BGA_IDLE;

    /* Pos setup */
    memset(&Pos, 0, sizeof(Pos));
    Pos.XResolution = 800;
    Pos.YResolution = 600;
    Pos.Phy_addr = (uint32_t *)FAKE_PCI_BAR0_BASE;
    Pos.FB_length = 800 * 600 * 4;
}

static uint16_t fake_io_read16(uint16_t port)
{
    if (port == VBE_DISPI_IOPORT_INDEX) {
        return s_dispi_index;
    }
    if (port == VBE_DISPI_IOPORT_DATA) {
        uint16_t idx = s_dispi_index;
        if (idx == VBE_DISPI_INDEX_VIDEO_MEMORY_64K && s_inject_vram_zero) {
            return 0;
        }
        if (idx == VBE_DISPI_INDEX_BPP && s_inject_initial_bpp_bad) {
            return 16;
        }
        if (idx == VBE_DISPI_INDEX_X_OFFSET && s_inject_initial_offset_bad) {
            return 8;
        }
        if (idx == VBE_DISPI_INDEX_VIRT_WIDTH && s_inject_initial_virt_w_bad) {
            return 1024;
        }

        /* Check GETCAPS */
        if (s_dispi_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            if (idx == VBE_DISPI_INDEX_XRES) return s_fake_max_w;
            if (idx == VBE_DISPI_INDEX_YRES) return s_fake_max_h;
            if (idx == VBE_DISPI_INDEX_BPP) {
                return s_inject_getcaps_bpp16 ? 16 : s_fake_max_bpp;
            }
        }

        if (idx == VBE_DISPI_INDEX_VIRT_WIDTH && s_inject_apply_readback_virt_w && s_phase == FAKE_BGA_APPLY_VERIFY) {
            return 640; /* Corrupt readback during apply */
        }
        if (idx == VBE_DISPI_INDEX_XRES && s_inject_rollback_readback_fail && s_phase == FAKE_BGA_ROLLING_BACK) {
            return 123; /* Corrupt rollback readback */
        }

        if (idx < 16) {
            return s_dispi_regs[idx];
        }
        return 0;
    }
    return 0;
}

static void fake_io_write16(uint16_t port, uint16_t val)
{
    if (port == VBE_DISPI_IOPORT_INDEX) {
        s_dispi_index = val;
    } else if (port == VBE_DISPI_IOPORT_DATA) {
        uint16_t idx = s_dispi_index;
        if (idx == VBE_DISPI_INDEX_ID) {
            if (val == VBE_DISPI_ID5 && s_inject_id_mismatch) {
                s_dispi_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID0;
                s_inject_id_mismatch = false; /* Inject only for initial handshake probe */
                return;
            }
        }
        if (idx == VBE_DISPI_INDEX_ENABLE) {
            if (val == VBE_DISPI_DISABLED) {
                if (s_phase == FAKE_BGA_APPLY_VERIFY) {
                    s_phase = FAKE_BGA_ROLLING_BACK;
                } else {
                    s_phase = FAKE_BGA_PROGRAMMING;
                }
            } else if (val & VBE_DISPI_NOCLEARMEM) {
                if (s_phase == FAKE_BGA_PROGRAMMING) {
                    s_phase = FAKE_BGA_APPLY_VERIFY;
                    s_seen_apply_noclearmem = true;
                } else if (s_phase == FAKE_BGA_ROLLING_BACK) {
                    s_seen_rollback_noclearmem = true;
                }
            }
        }
        if (idx == VBE_DISPI_INDEX_VIRT_WIDTH && val > 0) {
            s_dispi_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = (uint16_t)((16 * 1024 * 1024) / (val * 4));
        }
        if (idx < 16) {
            s_dispi_regs[idx] = val;
        }
    }
}

static int fake_pci_read32(struct pci_device *pdev, uint16_t offset, uint32_t *out)
{
    (void)pdev;
    if (!out) return -EINVAL;
    uint16_t dword_idx = offset / 4;
    if (dword_idx >= 16) return -EINVAL;

    if (dword_idx == 1 && s_inject_cmd_restore_fail) {
        *out = FAKE_PCI_STATUS_DEFAULT | 0x00FF; /* Mismatched command */
        return 0;
    }

    *out = s_pci_conf[dword_idx];
    return 0;
}

static int fake_pci_write32(struct pci_device *pdev, uint16_t offset, uint32_t val)
{
    (void)pdev;
    uint16_t dword_idx = offset / 4;
    if (dword_idx >= 16) return -EINVAL;

    if (dword_idx == 1) { /* Command/Status register */
        if (s_pci_cmd_writes_count < 32) {
            s_pci_cmd_writes[s_pci_cmd_writes_count++] = val;
        }
        /* Low 16 bits update command; upper 16 bits status: writes of 1 clear W1C */
        uint16_t new_cmd = val & 0xFFFF;
        s_pci_conf[1] = (s_pci_conf[1] & 0xFFFF0000U) | new_cmd;
        return 0;
    }

    if (dword_idx == 4) { /* BAR0 sizing probe */
        if (val == 0xFFFFFFFFU) {
            if (s_inject_bar_mask_zero) {
                s_pci_conf[4] = 0;
            } else {
                /* 16 MiB size mask: 0xFF000008 */
                s_pci_conf[4] = 0xFF000008U;
            }
        } else {
            if (s_inject_bar_restore_fail) {
                s_pci_conf[4] = 0xBAD00000U;
            } else {
                s_pci_conf[4] = val;
            }
        }
        return 0;
    }

    s_pci_conf[dword_idx] = val;
    return 0;
}

static struct pci_device make_fake_pdev(void)
{
    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.vendor = BGA_PCI_VENDOR_ID;
    pdev.device = BGA_PCI_DEVICE_ID;
    pdev.class_code = 0x030000;
    pdev.bars[0].kind = PCI_BAR_MMIO32;
    pdev.bars[0].address = FAKE_PCI_BAR0_BASE;
    pdev.bars[0].valid = true;
    pdev.bars[0].index = 0;
    return pdev;
}

static struct pci_device_id make_fake_pci_id(void)
{
    struct pci_device_id id;
    memset(&id, 0, sizeof(id));
    id.vendor = BGA_PCI_VENDOR_ID;
    id.device = BGA_PCI_DEVICE_ID;
    id.subvendor = PCI_ID_ANY;
    id.subdevice = PCI_ID_ANY;
    id.class_value = 0x030000;
    id.class_mask = 0xFFFF00;
    return id;
}

/* ── Test 1: test_probe_preserves_pci_and_dispi ────────────────── */
TEST_FUNC(test_probe_preserves_pci_and_dispi)
{
    struct pci_device pdev = make_fake_pdev();
    struct pci_device_id id = make_fake_pci_id();
    struct fb_info init_info = {
        .width = 800,
        .height = 600,
        .stride = 3200,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    /* Subtest A: BAR sizing failure (size mask 0) */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_bar_mask_zero = true;

    int rc = bga_probe(&pdev, &id);
    CHECK_TRUE(rc != 0);
    CHECK_EQ(FAKE_PCI_BAR0_BASE | 0x08U, s_pci_conf[4]); /* BAR0 restored */
    CHECK_EQ(FAKE_PCI_CMD_DEFAULT, s_pci_conf[1] & 0xFFFF); /* Command restored */
    CHECK_EQ(VBE_DISPI_ID5, s_dispi_regs[VBE_DISPI_INDEX_ID]); /* ID restored */
    CHECK_EQ(0, s_dispi_index); /* Index restored */

    /* Verify all Command/Status writes had high 16 bits = 0 */
    CHECK_TRUE(s_pci_cmd_writes_count > 0);
    for (size_t i = 0; i < s_pci_cmd_writes_count; i++) {
        CHECK_EQ(0, s_pci_cmd_writes[i] >> 16);
    }

    /* Subtest B: DISPI ID handshake failure */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_id_mismatch = true;

    rc = bga_probe(&pdev, &id);
    CHECK_TRUE(rc != 0);
    CHECK_EQ(FAKE_PCI_BAR0_BASE | 0x08U, s_pci_conf[4]);
    CHECK_EQ(FAKE_PCI_CMD_DEFAULT, s_pci_conf[1] & 0xFFFF);
    CHECK_EQ(VBE_DISPI_ID5, s_dispi_regs[VBE_DISPI_INDEX_ID]);
    CHECK_EQ(0, s_dispi_index);

    /* Subtest C: VIDEO_MEMORY_64K zero */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_vram_zero = true;

    rc = bga_probe(&pdev, &id);
    CHECK_TRUE(rc != 0);
    CHECK_EQ(FAKE_PCI_BAR0_BASE | 0x08U, s_pci_conf[4]);
    CHECK_EQ(FAKE_PCI_CMD_DEFAULT, s_pci_conf[1] & 0xFFFF);
    CHECK_EQ(VBE_DISPI_ID5, s_dispi_regs[VBE_DISPI_INDEX_ID]);

    /* Subtest D: GETCAPS reports max_bpp < 32 */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_getcaps_bpp16 = true;

    rc = bga_probe(&pdev, &id);
    CHECK_TRUE(rc != 0);
    CHECK_EQ(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED, s_dispi_regs[VBE_DISPI_INDEX_ENABLE]); /* ENABLE restored */

    /* Subtest E: Initial layout check failure (bpp != 32) */
    fake_hw_reset();
    s_dispi_regs[VBE_DISPI_INDEX_BPP] = 16;
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);

    rc = bga_probe(&pdev, &id);
    CHECK_TRUE(rc != 0);
    CHECK_EQ(16, s_dispi_regs[VBE_DISPI_INDEX_BPP]);

    /* Subtest F: BAR restore verification failure -> EIO and fb_mark_failed */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_bar_restore_fail = true;

    rc = bga_probe(&pdev, &id);
    CHECK_EQ(-EIO, rc);

    fb_snapshot_t snap;
    int s_rc = fb_snapshot_read(&snap);
    CHECK_EQ(-EIO, s_rc); /* System marked failed */
}

/* ── Test 2: test_probe_identity ───────────────────────────────── */
TEST_FUNC(test_probe_identity)
{
    struct pci_device pdev = make_fake_pdev();
    struct pci_device_id id = make_fake_pci_id();
    struct fb_info init_info = {
        .width = 800,
        .height = 600,
        .stride = 3200,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    /* Subtest A: Wrong class code (0x020000 Network) -> rejected */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    pdev.class_code = 0x020000;

    const struct pci_device_id *matched = pci_match_id(&bga_pci_driver, &pdev);
    CHECK_TRUE(matched == NULL);

    /* Subtest B: GOP base mismatch -> probe returns -ENODEV */
    fake_hw_reset();
    pdev.class_code = 0x030000;
    Pos.Phy_addr = (uint32_t *)0xC0000000ULL; /* Different from BAR0 0xE0000000 */
    fb_bootstrap_state(0xC0000000ULL, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);

    int rc = bga_probe(&pdev, &id);
    CHECK_EQ(-ENODEV, rc);
    fb_snapshot_t snap;
    CHECK_EQ(0, fb_snapshot_read(&snap)); /* GOP still valid, not failed */

    /* Subtest C: Sizing 0 capacity -> rejected */
    fake_hw_reset();
    Pos.Phy_addr = (uint32_t *)FAKE_PCI_BAR0_BASE;
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);
    s_inject_vram_zero = true;

    rc = bga_probe(&pdev, &id);
    CHECK_EQ(-ENODEV, rc);

    /* Subtest D: Valid candidate matching GOP base -> probe succeeds */
    fake_hw_reset();
    Pos.Phy_addr = (uint32_t *)FAKE_PCI_BAR0_BASE;
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);

    rc = bga_probe(&pdev, &id);
    CHECK_EQ(0, rc);
    CHECK_EQ(16 * 1024 * 1024, Pos.FB_length);

    /* Subtest E: Accept at most one backend -> second probe rejected */
    struct pci_device pdev2 = make_fake_pdev();
    pdev2.slot = 1;
    rc = bga_probe(&pdev2, &id);
    CHECK_TRUE(rc != 0);
}

/* ── Test 3: test_filter_capacity ──────────────────────────────── */
TEST_FUNC(test_filter_capacity)
{
    struct fb_info modes[FB_MAX_MODES];
    memset(modes, 0, sizeof(modes));

    bga_caps_t caps = {
        .vram_bytes = 16 * 1024 * 1024,
        .max_width = 1920,
        .max_height = 1080,
        .max_bpp = 32
    };

    /* Full 16 MiB: All 9 candidate modes must pass, including 1920x1080 */
    uint32_t count = bga_filter_modes(&caps, 16 * 1024 * 1024, modes);
    CHECK_EQ(9, count);
    CHECK_EQ(640, modes[0].width);
    CHECK_EQ(480, modes[0].height);
    CHECK_EQ(1920, modes[8].width);
    CHECK_EQ(1080, modes[8].height);
    for (uint32_t i = 0; i < count; i++) {
        CHECK_EQ(32, modes[i].bpp);
        CHECK_EQ(modes[i].width * 4, modes[i].stride);
        CHECK_EQ(FB_FORMAT_RGB32, modes[i].format);
    }

    /* 2 MiB (2,097,152 bytes): Only 640x480 (1.23MB) and 800x600 (1.92MB) fit */
    memset(modes, 0, sizeof(modes));
    count = bga_filter_modes(&caps, 2 * 1024 * 1024, modes);
    CHECK_EQ(2, count);
    CHECK_EQ(640, modes[0].width);
    CHECK_EQ(800, modes[1].width);

    /* 8 MiB (8,388,608 bytes): 1920x1080 requires 8,294,400 bytes, so it fits */
    memset(modes, 0, sizeof(modes));
    count = bga_filter_modes(&caps, 8 * 1024 * 1024, modes);
    CHECK_EQ(9, count);
    CHECK_EQ(1920, modes[8].width);

    /* Constrained max_width: max_width = 1024 */
    caps.max_width = 1024;
    memset(modes, 0, sizeof(modes));
    count = bga_filter_modes(&caps, 16 * 1024 * 1024, modes);
    CHECK_EQ(3, count); /* 640x480, 800x600, 1024x768 */
    CHECK_EQ(1024, modes[2].width);

    /* Constrained max_bpp: < 32 */
    caps.max_width = 1920;
    caps.max_bpp = 16;
    memset(modes, 0, sizeof(modes));
    count = bga_filter_modes(&caps, 16 * 1024 * 1024, modes);
    CHECK_EQ(0, count);
}

/* ── Test 4: test_apply_readback ───────────────────────────────── */
TEST_FUNC(test_apply_readback)
{
    struct pci_device pdev = make_fake_pdev();
    struct pci_device_id id = make_fake_pci_id();
    struct fb_info init_info = {
        .width = 800,
        .height = 600,
        .stride = 3200,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    /* Successfully probe first */
    fake_hw_reset();
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 800 * 600 * 4, &init_info);
    bga_reset_for_test();
    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);

    int rc = bga_probe(&pdev, &id);
    CHECK_EQ(0, rc);

    /* Subtest A: Successful apply */
    struct fb_info target = {
        .width = 1024,
        .height = 768,
        .stride = 4096,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    s_seen_apply_noclearmem = false;
    enum bga_result res = bga_apply_mode(&target);
    CHECK_EQ(BGA_APPLIED, res);
    CHECK_TRUE(s_seen_apply_noclearmem);
    CHECK_EQ(1024, s_dispi_regs[VBE_DISPI_INDEX_XRES]);
    CHECK_EQ(768, s_dispi_regs[VBE_DISPI_INDEX_YRES]);
    CHECK_EQ(1024, s_dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH]);
    CHECK_EQ(0, s_dispi_regs[VBE_DISPI_INDEX_X_OFFSET]);
    CHECK_EQ(0, s_dispi_regs[VBE_DISPI_INDEX_Y_OFFSET]);

    /* Subtest B: Readback mismatch triggers rollback */
    s_phase = FAKE_BGA_IDLE;
    target.width = 1280;
    target.height = 720;
    target.stride = 1280 * 4;
    s_inject_apply_readback_virt_w = true;
    s_seen_rollback_noclearmem = false;

    res = bga_apply_mode(&target);
    CHECK_EQ(BGA_ROLLED_BACK, res);
    CHECK_TRUE(s_seen_rollback_noclearmem);
    /* Restored to old layout (1024x768) */
    CHECK_EQ(1024, s_dispi_regs[VBE_DISPI_INDEX_XRES]);
    CHECK_EQ(768, s_dispi_regs[VBE_DISPI_INDEX_YRES]);
    CHECK_EQ(1024, s_dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH]);

    /* Subtest C: Rollback readback also fails -> BGA_FAILED */
    s_phase = FAKE_BGA_IDLE;
    s_seen_apply_noclearmem = false;
    s_seen_rollback_noclearmem = false;
    s_inject_apply_readback_virt_w = true;
    s_inject_rollback_readback_fail = true;

    res = bga_apply_mode(&target);
    CHECK_EQ(BGA_FAILED, res);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_probe_preserves_pci_and_dispi),
    TEST_ENTRY(test_probe_identity),
    TEST_ENTRY(test_filter_capacity),
    TEST_ENTRY(test_apply_readback),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    if (g_failed > 0) {
        printf("test_bga_resolution: %d failures\n", g_failed);
        return 1;
    }
    return 0;
}

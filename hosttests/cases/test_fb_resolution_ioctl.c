/*
 * hosttests/cases/test_fb_resolution_ioctl.c
 *
 * Safe framebuffer ioctl dispatch and SET state machine tests
 * (QEMU Resolution Switcher Task 5).
 * (Spec §4, §5)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <driver/fb.h>
#include <driver/fb_state.h>
#include <driver/bga.h>
#include <core/printk.h>
#include <tty/console.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <bus/pci/pci.h>

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

/* ── Framebuffer Backing Memory ── */
#define TEST_FB_BYTES (16 * 1024 * 1024) /* 16 MB */
static uint32_t *g_test_vram = NULL;

/* ── Hardware Simulation State ── */
#define FAKE_PCI_BAR0_BASE 0xE0000000ULL
#define FAKE_PCI_CMD_DEFAULT 0x0003U
#define FAKE_PCI_STATUS_DEFAULT 0x02900000U

static uint32_t s_pci_conf[16];
static uint16_t s_dispi_regs[16];
static uint16_t s_dispi_index = 0;
static uint32_t s_dispi_write_count = 0;

/* BGA Fault Injection Knobs */
static bool s_inject_apply_readback_virt_w = false;
static bool s_inject_rollback_readback_fail = false;

typedef enum {
    FAKE_BGA_IDLE = 0,
    FAKE_BGA_PROGRAMMING,
    FAKE_BGA_APPLY_VERIFY,
    FAKE_BGA_ROLLING_BACK,
} fake_bga_phase_t;

static fake_bga_phase_t s_phase = FAKE_BGA_IDLE;

static uint16_t fake_io_read16(uint16_t port)
{
    if (port == VBE_DISPI_IOPORT_INDEX) {
        return s_dispi_index;
    }
    if (port == VBE_DISPI_IOPORT_DATA) {
        uint16_t idx = s_dispi_index;
        /* GETCAPS check */
        if (s_dispi_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            if (idx == VBE_DISPI_INDEX_XRES) return 1920;
            if (idx == VBE_DISPI_INDEX_YRES) return 1080;
            if (idx == VBE_DISPI_INDEX_BPP)  return 32;
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
    s_dispi_write_count++;
    if (port == VBE_DISPI_IOPORT_INDEX) {
        s_dispi_index = val;
    } else if (port == VBE_DISPI_IOPORT_DATA) {
        uint16_t idx = s_dispi_index;
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
    *out = s_pci_conf[dword_idx];
    return 0;
}

static int fake_pci_write32(struct pci_device *pdev, uint16_t offset, uint32_t val)
{
    (void)pdev;
    uint16_t dword_idx = offset / 4;
    if (dword_idx >= 16) return -EINVAL;

    if (dword_idx == 1) {
        uint16_t new_cmd = val & 0xFFFF;
        s_pci_conf[1] = (s_pci_conf[1] & 0xFFFF0000U) | new_cmd;
        return 0;
    }

    if (dword_idx == 4) { /* BAR0 sizing probe */
        if (val == 0xFFFFFFFFU) {
            s_pci_conf[4] = 0xFF000008U; /* 16 MB mask */
        } else {
            s_pci_conf[4] = val;
        }
        return 0;
    }

    s_pci_conf[dword_idx] = val;
    return 0;
}

/* ── Mock User-Memory and FT-Copy Instrumentation ── */
static uint32_t g_copy_from_user_count = 0;
static size_t   g_copy_from_user_bytes = 0;
static uint32_t g_copy_to_user_count = 0;
static size_t   g_copy_to_user_bytes __attribute__((unused)) = 0;

static bool g_mock_user_range_valid = true;
static bool g_mock_user_readonly = false;
static bool g_inject_copy_from_fail = false;
static bool g_inject_copy_to_fail = false;

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    if (addr == 0) return false;
    if (!g_mock_user_range_valid) return false;
    if (writable && g_mock_user_readonly) return false;
    (void)len;
    return true;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_inject_copy_from_fail) {
        return -EFAULT;
    }
    g_copy_from_user_count++;
    g_copy_from_user_bytes += n;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_inject_copy_to_fail) {
        return -EFAULT;
    }
    g_copy_to_user_count++;
    g_copy_to_user_bytes += n;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* devfs, font and serial stubs */
#include <driver/font.h>

void write_serial_unlocked(unsigned char c) { (void)c; }
void write_serial(char c) { (void)c; }
void vfs_node_put(struct vfs_node *n) { (void)n; }
int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint64_t flags) { (void)pgdir; (void)phys; (void)virt; (void)flags; return 0; }
void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt) { (void)pgdir; (void)virt; }
int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out, uint32_t *vm_out) { (void)pgdir; (void)virt; (void)phys_out; (void)vm_out; return -ENOENT; }

void putchar_at_snap(const struct fb_snapshot *snap, int col, int row, unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    (void)snap; (void)col; (void)row; (void)FRcolor; (void)BKcolor; (void)c;
}

static psf2_t g_test_font = {
    .magic = 0x864ab572,
    .version = 0,
    .headersize = 32,
    .flags = 0,
    .numglyph = 256,
    .bytesperglyph = 16,
    .height = 16,
    .width = 8,
};
psf2_t *font = &g_test_font;

/* ── Environment Setup Helper ── */
static void setup_test_environment(void)
{
    fb_resolution_runtime_reset();
    bga_reset_for_test();

    if (!g_test_vram) {
        g_test_vram = (uint32_t *)malloc(TEST_FB_BYTES);
    }
    memset(g_test_vram, 0, TEST_FB_BYTES);

    g_copy_from_user_count = 0;
    g_copy_from_user_bytes = 0;
    g_copy_to_user_count = 0;
    g_copy_to_user_bytes = 0;
    g_mock_user_range_valid = true;
    g_mock_user_readonly = false;
    g_inject_copy_from_fail = false;
    g_inject_copy_to_fail = false;

    /* Initialize Fake Hardware */
    memset(s_pci_conf, 0, sizeof(s_pci_conf));
    s_pci_conf[0] = 0x11111234U; /* BGA */
    s_pci_conf[1] = FAKE_PCI_STATUS_DEFAULT | FAKE_PCI_CMD_DEFAULT;
    s_pci_conf[2] = 0x03000000U;
    s_pci_conf[4] = (uint32_t)(FAKE_PCI_BAR0_BASE | 0x08U);
    s_pci_conf[5] = 0;

    memset(s_dispi_regs, 0, sizeof(s_dispi_regs));
    s_dispi_index = 0;
    s_dispi_write_count = 0;
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
    s_dispi_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = 256; /* 16 MB */

    s_inject_apply_readback_virt_w = false;
    s_inject_rollback_readback_fail = false;
    s_phase = FAKE_BGA_IDLE;

    bga_set_transport_for_test(fake_io_read16, fake_io_write16, fake_pci_read32, fake_pci_write32);

    /* Setup Pos */
    memset(&Pos, 0, sizeof(Pos));
    Pos.XResolution = 800;
    Pos.YResolution = 600;
    Pos.Phy_addr = (uint32_t *)FAKE_PCI_BAR0_BASE;
    Pos.FB_addr = g_test_vram;
    Pos.FB_length = TEST_FB_BYTES;

    /* Bootstrap display coordinator */
    struct fb_info init_info = {
        .width = 800,
        .height = 600,
        .stride = 800 * 4,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, TEST_FB_BYTES, &init_info);
    fb_publish_initial_mapping(g_test_vram, TEST_FB_BYTES);
}

static void probe_test_bga(void)
{
    struct pci_device fake_pdev;
    memset(&fake_pdev, 0, sizeof(fake_pdev));
    fake_pdev.vendor = BGA_PCI_VENDOR_ID;
    fake_pdev.device = BGA_PCI_DEVICE_ID;
    fake_pdev.class_code = 0x030000;
    fake_pdev.bars[0].kind = PCI_BAR_MMIO32;
    fake_pdev.bars[0].address = FAKE_PCI_BAR0_BASE;
    fake_pdev.bars[0].valid = true;
    fake_pdev.bars[0].index = 0;

    struct pci_device_id match_id;
    memset(&match_id, 0, sizeof(match_id));
    match_id.vendor = BGA_PCI_VENDOR_ID;
    match_id.device = BGA_PCI_DEVICE_ID;

    int prc = bga_probe(&fake_pdev, &match_id);
    CHECK_EQ(0, prc);
    Pos.FB_addr = g_test_vram;
    s_dispi_write_count = 0;
}

/* ── Test 1: Query Pointer Directions and FBIOGET_MODES capacity handling ── */
static void test_query_pointer_directions(void)
{
    printf("\n--- test_query_pointer_directions ---\n");
    setup_test_environment();
    probe_test_bga();

    /* 1. FBIOGET_CURR_MODE: pure output, does NOT copy_from_user */
    struct fb_info curr_info;
    memset(&curr_info, 0xCC, sizeof(curr_info));
    g_copy_from_user_count = 0;
    g_copy_to_user_count = 0;

    int rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &curr_info);
    CHECK_EQ(0, rc);
    CHECK_EQ(0, g_copy_from_user_count);
    CHECK_EQ(1, g_copy_to_user_count);
    CHECK_EQ(800, curr_info.width);
    CHECK_EQ(600, curr_info.height);
    CHECK_EQ(3200, curr_info.stride);
    CHECK_EQ(32, curr_info.bpp);
    CHECK_EQ(FB_FORMAT_RGB32, curr_info.format);

    /* 2. FBIOGET_STATE: pure output, does NOT copy_from_user */
    struct fb_state curr_state;
    memset(&curr_state, 0xCC, sizeof(curr_state));
    g_copy_from_user_count = 0;
    g_copy_to_user_count = 0;

    rc = fb_ops.ioctl(NULL, FBIOGET_STATE, &curr_state);
    CHECK_EQ(0, rc);
    CHECK_EQ(0, g_copy_from_user_count);
    CHECK_EQ(1, g_copy_to_user_count);
    CHECK_EQ(800, curr_state.info.width);
    CHECK_EQ(600, curr_state.info.height);
    CHECK_EQ(0, curr_state.reserved);
    CHECK_TRUE(curr_state.generation >= 1);

    /* 3. FBIOGET_MODES capacity handling */
    /* Case A: capacity 17 -> EINVAL */
    struct fb_modes_req req;
    memset(&req, 0xAA, sizeof(req));
    req.capacity = 17;
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &req);
    CHECK_EQ(-EINVAL, rc);

    /* Case B: capacity 0 -> count 0, total 9, returns 0, modes zeroed */
    memset(&req, 0xAA, sizeof(req));
    req.capacity = 0;
    g_copy_from_user_count = 0;
    g_copy_from_user_bytes = 0;

    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &req);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_copy_from_user_count);
    CHECK_EQ(sizeof(uint32_t), g_copy_from_user_bytes); /* Read ONLY capacity! */
    CHECK_EQ(0, req.capacity);
    CHECK_EQ(0, req.count);
    CHECK_EQ(9, req.total);
    for (int i = 0; i < FB_MAX_MODES; i++) {
        CHECK_EQ(0, req.modes[i].width);
        CHECK_EQ(0, req.modes[i].height);
    }

    /* Case C: capacity 1 -> truncate to 1, modes[0] populated, rest zeroed */
    memset(&req, 0xAA, sizeof(req));
    req.capacity = 1;
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &req);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, req.capacity);
    CHECK_EQ(1, req.count);
    CHECK_EQ(9, req.total);
    CHECK_EQ(640, req.modes[0].width);
    CHECK_EQ(480, req.modes[0].height);
    for (int i = 1; i < FB_MAX_MODES; i++) {
        CHECK_EQ(0, req.modes[i].width);
        CHECK_EQ(0, req.modes[i].height);
    }

    /* Case D: capacity 16 -> returns all 9 supported modes, remainder zeroed */
    memset(&req, 0xAA, sizeof(req));
    req.capacity = 16;
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &req);
    CHECK_EQ(0, rc);
    CHECK_EQ(16, req.capacity);
    CHECK_EQ(9, req.count);
    CHECK_EQ(9, req.total);
    CHECK_EQ(640, req.modes[0].width);
    CHECK_EQ(800, req.modes[1].width);
    CHECK_EQ(1920, req.modes[8].width);
    for (int i = 9; i < FB_MAX_MODES; i++) {
        CHECK_EQ(0, req.modes[i].width);
        CHECK_EQ(0, req.modes[i].height);
    }
}

/* ── Test 2: Unknown cmd, FBIOSURRENDER, NULL pointers, range/copy faults ── */
static void test_ioctl_fault_cleanup(void)
{
    printf("\n--- test_ioctl_fault_cleanup ---\n");
    setup_test_environment();
    probe_test_bga();

    /* 1. Unknown cmd returns -ENOTTY immediately without inspecting arg */
    int rc = fb_ops.ioctl(NULL, 0x9999, NULL);
    CHECK_EQ(-ENOTTY, rc);
    rc = fb_ops.ioctl(NULL, 0x9999, (void *)0x12345678ULL);
    CHECK_EQ(-ENOTTY, rc);

    /* 2. FBIOSURRENDER takes no pointer arg, succeeds even if arg == NULL */
    rc = fb_ops.ioctl(NULL, FBIOSURRENDER, NULL);
    CHECK_EQ(0, rc);
    rc = fb_ops.ioctl(NULL, FBIOSURRENDER, (void *)0x12345678ULL);
    CHECK_EQ(0, rc);

    /* 3. Known ioctls with NULL pointer return -EFAULT */
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, NULL);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, NULL);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, NULL);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOGET_STATE, NULL);
    CHECK_EQ(-EFAULT, rc);

    /* 4. Invalid user range -> -EFAULT */
    struct fb_info dummy_info;
    struct fb_set_mode_req dummy_set = { .width = 800, .height = 600, .bpp = 32 };
    g_mock_user_range_valid = false;

    rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &dummy_info);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &dummy_set);
    CHECK_EQ(-EFAULT, rc);
    g_mock_user_range_valid = true;

    /* 5. Read-only user buffer passed to output commands -> -EFAULT */
    g_mock_user_readonly = true;
    rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &dummy_info);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOGET_STATE, &dummy_info);
    CHECK_EQ(-EFAULT, rc);
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &dummy_info);
    CHECK_EQ(-EFAULT, rc);
    g_mock_user_readonly = false;

    /* 6. Injected copy fault -> -EFAULT and no lock leak */
    g_inject_copy_to_fail = true;
    rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &dummy_info);
    CHECK_EQ(-EFAULT, rc);
    g_inject_copy_to_fail = false;

    g_inject_copy_from_fail = true;
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &dummy_set);
    CHECK_EQ(-EFAULT, rc);
    g_inject_copy_from_fail = false;

    /* Verify mutex was not leaked by checking next ioctl succeeds */
    rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &dummy_info);
    CHECK_EQ(0, rc);
}

/* ── Test 3: SET no-op (same mode) does not touch hardware or alter state ── */
static void test_set_noop(void)
{
    printf("\n--- test_set_noop ---\n");
    setup_test_environment();
    probe_test_bga();

    /* Put sentinel pattern into framebuffer */
    g_test_vram[0] = 0xDEADBEEFU;
    g_test_vram[100] = 0xCAFEBABEU;

    /* Set non-zero cursor */
    console__test_set_cursors(5, 10, 10, 5);

    /* Get initial state */
    struct fb_state init_st;
    fb_ops.ioctl(NULL, FBIOGET_STATE, &init_st);
    uint64_t init_gen = init_st.generation;

    s_dispi_write_count = 0;

    /* Call FBIOSET_MODE with 800x600 (same mode, bpp=32) */
    struct fb_set_mode_req req = { .width = 800, .height = 600, .bpp = 32 };
    int rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &req);
    CHECK_EQ(0, rc);

    /* Assert 0 hardware writes */
    CHECK_EQ(0, s_dispi_write_count);

    /* Assert generation unchanged */
    struct fb_state new_st;
    fb_ops.ioctl(NULL, FBIOGET_STATE, &new_st);
    CHECK_EQ(init_gen, new_st.generation);

    /* Assert framebuffer contents intact (no clearing) */
    CHECK_EQ(0xDEADBEEFU, g_test_vram[0]);
    CHECK_EQ(0xCAFEBABEU, g_test_vram[100]);

    /* Assert cursors unchanged */
    int row = -1, col = -1;
    int32_t px = -1, py = -1;
    console__test_get_cursors(&row, &col, &px, &py);
    CHECK_EQ(5, row);
    CHECK_EQ(10, col);
    CHECK_EQ(10, px);
    CHECK_EQ(5, py);

    /* Call with bpp=0 (default 32) -> also no-op */
    req.bpp = 0;
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &req);
    CHECK_EQ(0, rc);
    CHECK_EQ(0, s_dispi_write_count);
}

/* ── Test 4: SET rollback on apply failure ── */
static void test_set_rollback(void)
{
    printf("\n--- test_set_rollback ---\n");
    setup_test_environment();
    probe_test_bga();

    /* Fill framebuffer */
    memset(g_test_vram, 0x55, 800 * 600 * 4);

    /* Set non-zero cursor */
    console__test_set_cursors(3, 7, 7, 3);

    struct fb_state init_st;
    fb_ops.ioctl(NULL, FBIOGET_STATE, &init_st);
    uint64_t init_gen = init_st.generation;

    /* Inject apply readback failure -> BGA will rollback to 800x600 */
    s_inject_apply_readback_virt_w = true;
    s_inject_rollback_readback_fail = false;

    struct fb_set_mode_req req = { .width = 1024, .height = 768, .bpp = 32 };
    int rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &req);
    CHECK_EQ(-EIO, rc);

    /* Mode remains old size */
    struct fb_state cur_st;
    fb_ops.ioctl(NULL, FBIOGET_STATE, &cur_st);
    CHECK_EQ(800, cur_st.info.width);
    CHECK_EQ(600, cur_st.info.height);

    /* Generation is incremented to invalidate views */
    CHECK_EQ(init_gen + 1, cur_st.generation);

    /* Cursor is reset */
    int row = -1, col = -1;
    int32_t px = -1, py = -1;
    console__test_get_cursors(&row, &col, &px, &py);
    CHECK_EQ(0, row);
    CHECK_EQ(0, col);
    CHECK_EQ(0, px);
    CHECK_EQ(0, py);

    /* Old active area was cleared */
    CHECK_EQ(0u, g_test_vram[0]);
    CHECK_EQ(0u, g_test_vram[800 * 300]);

    s_inject_apply_readback_virt_w = false;
}

/* ── Test 5: SET permanent backend failure ── */
static void test_set_failed_backend(void)
{
    printf("\n--- test_set_failed_backend ---\n");
    setup_test_environment();
    probe_test_bga();

    /* Inject failure on apply AND rollback -> BGA_FAILED */
    s_inject_apply_readback_virt_w = true;
    s_inject_rollback_readback_fail = true;

    struct fb_set_mode_req req = { .width = 1024, .height = 768, .bpp = 32 };
    int rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &req);
    CHECK_EQ(-EIO, rc);

    /* Subsequent queries return -EIO */
    struct fb_state st;
    CHECK_EQ(-EIO, fb_ops.ioctl(NULL, FBIOGET_STATE, &st));

    struct fb_info info;
    CHECK_EQ(-EIO, fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &info));

    struct fb_modes_req mreq = { .capacity = 16 };
    CHECK_EQ(-EIO, fb_ops.ioctl(NULL, FBIOGET_MODES, &mreq));

    /* Subsequent SET returns -EIO and does not touch hardware */
    s_dispi_write_count = 0;
    req.width = 640;
    req.height = 480;
    CHECK_EQ(-EIO, fb_ops.ioctl(NULL, FBIOSET_MODE, &req));
    CHECK_EQ(0, s_dispi_write_count);

    /* FBIOSURRENDER still succeeds */
    CHECK_EQ(0, fb_ops.ioctl(NULL, FBIOSURRENDER, NULL));

    s_inject_apply_readback_virt_w = false;
    s_inject_rollback_readback_fail = false;
}

/* ── Test 6: raw sticky blocks changing layout (-EBUSY) but permits same mode (0) ── */
static void test_raw_sticky_behavior(void)
{
    printf("\n--- test_raw_sticky_behavior ---\n");
    setup_test_environment();
    probe_test_bga();

    /* Mark raw mmap seen */
    fb_mark_raw_mmap_seen();
    CHECK_TRUE(fb_has_raw_mmap_seen());

    /* Same mode succeeds with 0 */
    struct fb_set_mode_req same_req = { .width = 800, .height = 600, .bpp = 32 };
    int rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &same_req);
    CHECK_EQ(0, rc);

    /* Changing mode returns -EBUSY */
    struct fb_set_mode_req diff_req = { .width = 1024, .height = 768, .bpp = 32 };
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &diff_req);
    CHECK_EQ(-EBUSY, rc);
}

/* ── Test 7: Behavior when BGA backend is unprobed ── */
static void test_unprobed_bga(void)
{
    printf("\n--- test_unprobed_bga ---\n");
    setup_test_environment();
    /* Do NOT probe BGA */

    /* FBIOGET_CURR_MODE returns GOP initial mode */
    struct fb_info info;
    int rc = fb_ops.ioctl(NULL, FBIOGET_CURR_MODE, &info);
    CHECK_EQ(0, rc);
    CHECK_EQ(800, info.width);
    CHECK_EQ(600, info.height);

    /* FBIOGET_STATE returns GOP initial mode */
    struct fb_state st;
    rc = fb_ops.ioctl(NULL, FBIOGET_STATE, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(800, st.info.width);
    CHECK_EQ(1, st.generation);

    /* FBIOGET_MODES returns -ENODEV */
    struct fb_modes_req mreq = { .capacity = 16 };
    rc = fb_ops.ioctl(NULL, FBIOGET_MODES, &mreq);
    CHECK_EQ(-ENODEV, rc);

    /* FBIOSET_MODE returns -ENODEV */
    struct fb_set_mode_req sreq = { .width = 640, .height = 480, .bpp = 32 };
    rc = fb_ops.ioctl(NULL, FBIOSET_MODE, &sreq);
    CHECK_EQ(-ENODEV, rc);

    /* FBIOSURRENDER still succeeds */
    rc = fb_ops.ioctl(NULL, FBIOSURRENDER, NULL);
    CHECK_EQ(0, rc);
}

/* ── Main Test Runner ── */
int main(void)
{
    printf("=== Test Runner: Safe Framebuffer Ioctl & SET Coordination ===\n");

    test_query_pointer_directions();
    test_ioctl_fault_cleanup();
    test_set_noop();
    test_set_rollback();
    test_set_failed_backend();
    test_raw_sticky_behavior();
    test_unprobed_bga();

    printf("\n  ---\n  Total: %u | Passed: %u | Failed: %u\n",
           __test_stats.total, __test_stats.passed, g_failed);

    if (g_failed > 0) {
        printf("  >>> SOME TESTS FAILED <<<\n");
        return 1;
    }

    printf("  >>> ALL TESTS PASSED <<<\n");
    return 0;
}

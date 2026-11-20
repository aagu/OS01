/*
 * hosttests/cases/test_fb_writers.c
 *
 * Framebuffer writers admission, leases, and raw mmap rollback tests
 * (QEMU Resolution Switcher Task 3).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <setjmp.h>
#include <test_framework.h>

#include "fb_mode_fixture.h"
#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <uapi/gfx.h>
#include <driver/fb_state.h>
#include <driver/fb_cache.h>
#include <driver/fb.h>
#include <driver/gfx.h>
#include <core/printk.h>
#include <core/panic.h>
#include <tty/console.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <driver/font.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>
#include <arch/fb_copy.h>          /* fb_copy_rgb32 + the row geometry */

/* ── Test Assertions ── */
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

/* ── Serial output capture buffer ── */
#define SERIAL_BUF_SIZE 1024
static char g_serial_buf[SERIAL_BUF_SIZE];
static size_t g_serial_pos = 0;

void write_serial_unlocked(unsigned char c)
{
    if (g_serial_pos < SERIAL_BUF_SIZE - 1) {
        g_serial_buf[g_serial_pos++] = (char)c;
        g_serial_buf[g_serial_pos] = '\0';
    }
}

void write_serial(char c)
{
    write_serial_unlocked((unsigned char)c);
}

static void reset_serial_capture(void)
{
    memset(g_serial_buf, 0, sizeof(g_serial_buf));
    g_serial_pos = 0;
}

/* ── Hardware / Host Framebuffer Backing ── */
#define TEST_FB_WIDTH 64
#define TEST_FB_HEIGHT 48
#define TEST_FB_PAGES 16
#define TEST_FB_BYTES (TEST_FB_PAGES * 4096)

static uint32_t g_test_vram[TEST_FB_BYTES / 4] __attribute__((aligned(4096)));

/* ── Mock Page Table Tracking for mmap ── */
#define MAX_TRACKED_PAGES 64
static uint64_t g_installed_ptes[MAX_TRACKED_PAGES];
static uint64_t g_installed_flags[MAX_TRACKED_PAGES];
static size_t   g_installed_count = 0;
static int      g_inject_map_fail_page = -1;
static int      g_inject_unmap_fail_page = -1;
static int      g_current_map_page = 0;

int x86_vmm_map_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint64_t flags)
{
    (void)pgdir;
    if (g_current_map_page == g_inject_map_fail_page) {
        g_current_map_page++;
        return -ENOMEM;
    }
    g_current_map_page++;

    if (g_installed_count < MAX_TRACKED_PAGES) {
        g_installed_ptes[g_installed_count] = virt;
        g_installed_flags[g_installed_count] = flags;
        g_installed_count++;
    }
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    for (size_t i = 0; i < g_installed_count; i++) {
        if (g_installed_ptes[i] == virt) {
            if ((int)i == g_inject_unmap_fail_page) {
                return;
            }
            /* Remove by shifting */
            for (size_t j = i; j + 1 < g_installed_count; j++) {
                g_installed_ptes[j] = g_installed_ptes[j + 1];
                g_installed_flags[j] = g_installed_flags[j + 1];
            }
            g_installed_count--;
            return;
        }
    }
}

/* ── MM commit callback mock ─────────────────────────────────
 * fb_mmap validates and then hands the range to the MM transaction;
 * this callback records the device PA and the SEMANTIC cache request
 * and returns the configured result.  The driver test asserts the
 * request only — the leaf-bit contract lives in the production MM
 * commit test. */
static int      g_mock_commit_calls;
static int      g_mock_commit_rc;
static uint64_t g_mock_commit_first_pa;
static enum vm_device_cache g_mock_commit_cache;

static int mock_commit(void *ctx, uint64_t first_pa, enum vm_device_cache cache)
{
    (void)ctx;
    g_mock_commit_calls++;
    g_mock_commit_first_pa = first_pa;
    g_mock_commit_cache = cache;
    return g_mock_commit_rc;
}

/* ── Fault-Tolerant Copy / User Range Mocks ── */
static int g_inject_copy_fail_row = -1;
static int g_current_copy_row = 0;

int syscall_check_user_range(uint64_t addr, size_t size, bool write)
{
    (void)addr; (void)size; (void)write;
    return 1;
}

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 0;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_current_copy_row == g_inject_copy_fail_row) {
        g_current_copy_row++;
        return -EFAULT;
    }
    g_current_copy_row++;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ── Kernel Global Structures Mocking ── */
position Pos;
spinlock_T serial_lock;
static uint64_t g_mock_user_pgd[512] __attribute__((aligned(4096)));
static struct mm_struct g_mock_mm;

file_t *file_alloc(void)
{
    return (file_t *)calloc(1, sizeof(file_t));
}

void vfs_node_put(struct vfs_node *n)
{
    (void)n;
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

void putchar_at_snap(const struct fb_snapshot *snap, int col, int row, unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    (void)snap; (void)col; (void)row; (void)FRcolor; (void)BKcolor; (void)c;
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    putchar_at_snap(NULL, col, row, FRcolor, BKcolor, c);
}

int color_printk(unsigned int FRcolor, unsigned int BKcolor, const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor;
    char buf[256];
    va_list args;
    uint64_t flags = spin_lock_irqsave(&Pos.lock);

    va_start(args, fmt);
    int i = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    fb_lease_t lease;
    int lrc = fb_writer_begin(&lease, 0);
    if (lrc < 0) {
        // Drop Pos.lock BEFORE taking serial_lock to prevent AB-BA deadlock
        spin_unlock_irqrestore(&Pos.lock, flags);
        uint64_t sf = spin_lock_irqsave(&serial_lock);
        for (int count = 0; count < i; count++) {
            write_serial_unlocked((unsigned char)buf[count]);
        }
        spin_unlock_irqrestore(&serial_lock, sf);
        return i;
    }

    for (int count = 0; count < i; count++) {
        if (buf[count] == '\n') {
            Pos.YPosition++;
            Pos.XPosition = 0;
        } else {
            Pos.XPosition++;
        }
    }
    fb_writer_end(&lease);
    spin_unlock_irqrestore(&Pos.lock, flags);
    return i;
}

/* ── Console cursors access helper ── */
#ifdef OS01_HOST_TEST
void console__test_get_cursors(int *row, int *col, int32_t *pos_x, int32_t *pos_y);
void console__test_set_cursors(int row, int col, int32_t pos_x, int32_t pos_y);
#endif

static void setup_test_environment(void)
{
    fb_resolution_runtime_reset();
    spin_init(&Pos.lock);
    spin_init(&serial_lock);
    reset_serial_capture();

    memset(g_test_vram, 0x55, sizeof(g_test_vram));
    g_installed_count = 0;
    g_inject_map_fail_page = -1;
    g_inject_unmap_fail_page = -1;
    g_current_map_page = 0;
    g_inject_copy_fail_row = -1;
    g_current_copy_row = 0;
    g_mock_commit_calls = 0;
    g_mock_commit_rc = 0;
    g_mock_commit_first_pa = 0;
    g_mock_commit_cache = VM_DEVICE_CACHE_UC;

    Pos.XResolution = TEST_FB_WIDTH;
    Pos.YResolution = TEST_FB_HEIGHT;
    Pos.XPosition = 0;
    Pos.YPosition = 0;
    Pos.Phy_addr = (uint32_t *)(uintptr_t)0xE0000000ULL;
    Pos.FB_addr = g_test_vram;
    Pos.FB_length = TEST_FB_BYTES;

    struct fb_info info = {
        .width = TEST_FB_WIDTH,
        .height = TEST_FB_HEIGHT,
        .stride = TEST_FB_WIDTH * 4,
        .bpp = 32,
        .format = 0,
    };
    fb_bootstrap_state(0xE0000000ULL, TEST_FB_BYTES, &info);
    fb_publish_initial_mapping(g_test_vram, TEST_FB_BYTES);

    g_mock_mm.pgdir = g_mock_user_pgd;
    current->mm = (void *)&g_mock_mm;
}

/* ── Step 1 Test 1: test_present_fault_releases_lease ── */
static void test_present_fault_releases_lease(void)
{
    printf("\n--- test_present_fault_releases_lease ---\n");
    setup_test_environment();
    gfx_init();

    file_t *f = NULL;
    int rc = gfx_ops.open("gfx0", &f);
    CHECK_EQ(0, rc);

    gfx_view_desc_t desc = {
        .x = 0,
        .y = 0,
        .w = 32,
        .h = 4,
    };
    rc = gfx_ops.ioctl_file(f, GFX_CREATE_VIEW, &desc);
    CHECK_EQ(0, rc);

    rc = gfx_ops.ioctl_file(f, GFX_ACQUIRE, (void *)(uintptr_t)0);
    CHECK_EQ(0, rc);

    uint32_t fake_pixels[32 * 4];
    memset(fake_pixels, 0xAA, sizeof(fake_pixels));

    gfx_present_req_t req = {
        .pixels = (uint64_t)(uintptr_t)fake_pixels,
        .stride = 32 * 4,
        .reserved = 0,
    };

    /* Inject copy_from_user_ft failure on row 2 (0-indexed row 1) */
    /* Note: row 0 succeeds, row 1 fails */
    g_current_copy_row = 0;
    g_inject_copy_fail_row = 1;

    rc = gfx_ops.ioctl_file(f, GFX_PRESENT, &req);
    CHECK_EQ(-EFAULT, rc);

    /* Assert no leases leaked: active writers is 0 */
    CHECK_EQ(0, fb_active_writers_count());

    /* Subsequently transition must succeed immediately without timing out */
    rc = fb_transition_begin(false);
    CHECK_EQ(0, rc);
    fb_transition_end();

    gfx_ops.release_file(f);
}

/* ── Step 1 Test 2: test_view_aba_stale ── */
static void test_view_aba_stale(void)
{
    printf("\n--- test_view_aba_stale ---\n");
    setup_test_environment();
    const struct fb_set_mode_req modes[] = {
        { .width = TEST_FB_WIDTH / 2, .height = TEST_FB_HEIGHT / 2, .bpp = 32 },
        { .width = TEST_FB_WIDTH, .height = TEST_FB_HEIGHT, .bpp = 32 },
    };
    CHECK_EQ(0, fb_mode_fixture_install(g_test_vram, sizeof(g_test_vram), modes, 2));
    gfx_init();

    file_t *f = NULL;
    int rc = gfx_ops.open("gfx0", &f);
    CHECK_EQ(0, rc);

    gfx_view_desc_t desc = {
        .x = 0,
        .y = 0,
        .w = 32,
        .h = 4,
    };
    rc = gfx_ops.ioctl_file(f, GFX_CREATE_VIEW, &desc);
    CHECK_EQ(0, rc);

    rc = gfx_ops.ioctl_file(f, GFX_ACQUIRE, (void *)(uintptr_t)0);
    CHECK_EQ(0, rc);

    struct fb_state before = {0}, after = {0};
    CHECK_EQ(0, fb_get_state(&before));
    CHECK_EQ(0, fb_set_mode(&modes[0]));
    CHECK_EQ(0, fb_set_mode(&modes[1]));
    CHECK_EQ(0, fb_get_state(&after));
    CHECK_EQ(before.generation + 2, after.generation);
    CHECK_EQ(before.info.width, after.info.width);
    CHECK_EQ(before.info.height, after.info.height);
    memset(g_test_vram, 0x55, sizeof(g_test_vram));

    uint32_t pixel_val = 0x12345678;
    uint32_t fake_pixels[32 * 4];
    for (int i = 0; i < 32 * 4; i++) fake_pixels[i] = pixel_val;

    gfx_present_req_t req = {
        .pixels = (uint64_t)(uintptr_t)fake_pixels,
        .stride = 32 * 4,
        .reserved = 0,
    };

    /* Old view present must return -ESTALE */
    rc = gfx_ops.ioctl_file(f, GFX_PRESENT, &req);
    CHECK_EQ(-ESTALE, rc);

    /* Zero writes to VRAM: verify first pixels are still 0x55555555 */
    CHECK_EQ(0x55555555u, g_test_vram[0]);
    CHECK_EQ(0x55555555u, g_test_vram[1]);

    gfx_ops.release_file(f);
}

/* ── Step 1 Test 3: test_console_transition_serial_only ── */
static void test_console_transition_serial_only(void)
{
    printf("\n--- test_console_transition_serial_only ---\n");
    setup_test_environment();
    console_init();

    /* Set cursor to known location */
    console__test_set_cursors(5, 10, 10, 5);

    /* Put system into transitioning state */
    int rc = fb_transition_begin(true);
    CHECK_EQ(0, rc);

    reset_serial_capture();
    console_putchar('Z');

    /* Serial must still receive output */
    CHECK_EQ(1, g_serial_pos);
    CHECK_EQ('Z', g_serial_buf[0]);

    /* Also test color_printk fallback during transition */
    reset_serial_capture();
    int printed = color_printk(0x00ffffff, 0x00000000, "TransMsg\n");
    CHECK_TRUE(printed > 0);
    CHECK_TRUE(strstr(g_serial_buf, "TransMsg\n") != NULL);

    /* Cursors and Pos positions must STILL remain untouched */
    int row = -1, col = -1;
    int32_t pos_x = -1, pos_y = -1;
    console__test_get_cursors(&row, &col, &pos_x, &pos_y);
    CHECK_EQ(5, row);
    CHECK_EQ(10, col);
    CHECK_EQ(10, pos_x);
    CHECK_EQ(5, pos_y);

    fb_transition_end();

    /* After transition ends, color_printk succeeds and advances YPosition */
    reset_serial_capture();
    printed = color_printk(0x00ffffff, 0x00000000, "NormMsg\n");
    CHECK_TRUE(printed > 0);
    CHECK_EQ(6, Pos.YPosition);
}

/* ── Step 1 Test 4: test_mmap_partial_failure ── */
static void test_mmap_partial_failure(void)
{
    printf("\n--- test_mmap_partial_failure ---\n");
    setup_test_environment();

    vma_t vma;
    memset(&vma, 0, sizeof(vma));
    /* PTE permissions plan: fb_mmap accepts R or RW only — give the
     * harness VMA a real prot instead of the old bare-SHARED shape. */
    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000; /* 4 pages = 16KB */
    vma.vm_file  = NULL;

    /* Subcase 4a: the MM transaction reports an install failure.  It
     * rolls back its own leaves (the leaf-level rollback lives in the
     * production MM commit tests); fb_mmap relays the error and
     * publishes no raw state. */
    g_mock_commit_rc = -ENOMEM;

    int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-ENOMEM, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_UC, g_mock_commit_cache);
    CHECK_EQ(false, fb_has_raw_mmap_seen());

    /* Subcase 4b: a failed commit (device-side denial) publishes
     * neither leaves nor raw state. */
    setup_test_environment();
    g_installed_count = 0;
    g_mock_commit_rc = -EPERM;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EPERM, rc);
    CHECK_EQ(0, g_installed_count);
    CHECK_EQ(false, fb_has_raw_mmap_seen());

    /* Subcase 4c: successful commit sets raw state.  The driver never
     * installs leaves itself — the MM transaction owns that. */
    setup_test_environment();
    g_current_map_page = 0;
    g_inject_map_fail_page = -1;
    g_installed_count = 0;

    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_UC, g_mock_commit_cache);
    CHECK_EQ(true, fb_has_raw_mmap_seen());
}

/* A failed MAP_FIXED replacement must leave the old four-page mapping
 * byte-for-byte intact: permission denial happens before the commit,
 * and a transaction failure reports without touching the old leaves. */
static void test_fb_fixed_failure_keeps_old_mapping(void)
{
    printf("\n--- test_fb_fixed_failure_keeps_old_mapping ---\n");
    setup_test_environment();

    vma_t vma;
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000;
    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;

    /* Establish the old mapping in the mock page table. */
    for (int i = 0; i < 4; i++)
        x86_vmm_map_4k(NULL, 0x5000 + (uint64_t)i * 0x1000,
                        vma.vm_start + (uint64_t)i * 0x1000,
                        PAGE_USER_PTE_RO);
    uint64_t old_ptes[MAX_TRACKED_PAGES], old_flags[MAX_TRACKED_PAGES];
    size_t old_count = g_installed_count;
    memcpy(old_ptes, g_installed_ptes, sizeof(old_ptes));
    memcpy(old_flags, g_installed_flags, sizeof(old_flags));

    /* 1. Permission denial under the control lock: writable raw mmap
     *    is not allowed in normal mode. */
    vma.vm_flags = VMA_SHARED | VMA_PROT_WRITE;
    int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EPERM, rc);
    CHECK_EQ(0, g_mock_commit_calls);
    CHECK_EQ(old_count, g_installed_count);
    CHECK_EQ(0, memcmp(old_ptes, g_installed_ptes, sizeof(old_ptes)));
    CHECK_EQ(0, memcmp(old_flags, g_installed_flags, sizeof(old_flags)));
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());

    /* 2. Transaction OOM after validation: fb_mmap relays the error,
     *    publishes nothing and the old mapping is untouched. */
    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;
    g_mock_commit_rc = -ENOMEM;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-ENOMEM, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(old_count, g_installed_count);
    CHECK_EQ(0, memcmp(old_ptes, g_installed_ptes, sizeof(old_ptes)));
    CHECK_EQ(0, memcmp(old_flags, g_installed_flags, sizeof(old_flags)));
    CHECK_EQ(false, fb_has_raw_mmap_seen());

    /* 3. Transaction -EINVAL after validation: the error propagates
     *    verbatim and neither raw marker moves. */
    bool raw_before = fb_has_raw_mmap_seen();
    bool raw_w_before = fb_has_raw_writable_mmap_seen();
    g_mock_commit_rc = -EINVAL;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EINVAL, rc);
    CHECK_EQ(2, g_mock_commit_calls);
    CHECK_EQ(old_count, g_installed_count);
    CHECK_EQ(0, memcmp(old_ptes, g_installed_ptes, sizeof(old_ptes)));
    CHECK_EQ(0, memcmp(old_flags, g_installed_flags, sizeof(old_flags)));
    CHECK_EQ(raw_before, fb_has_raw_mmap_seen());
    CHECK_EQ(raw_w_before, fb_has_raw_writable_mmap_seen());

    /* 4. A successful commit hands the device PA and the semantic
     *    cache request of the published policy (default: uncacheable)
     *    to the MM transaction, and publishes the raw state. */
    g_mock_commit_rc = 0;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(0xE0000000ULL, g_mock_commit_first_pa);
    CHECK_EQ(VM_DEVICE_CACHE_UC, g_mock_commit_cache);
    CHECK_EQ(true, fb_has_raw_mmap_seen());
}

/* PTE permissions plan: the framebuffer accepts R or RW only.
 * PROT_NONE / PROT_EXEC / unknown prots are rejected in the prepare
 * phase with -EINVAL and the old mapping stays untouched. */
static void test_fb_mmap_prot_rejection(void)
{
    printf("\n--- test_fb_mmap_prot_rejection ---\n");
    setup_test_environment();

    vma_t vma;
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000;

    /* 1. EXEC (with or without R/W) → -EINVAL. */
    uint64_t exec_variants[] = {
        VMA_SHARED | VMA_PROT_EXEC,
        VMA_SHARED | VMA_PROT_READ | VMA_PROT_EXEC,
        VMA_SHARED | VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC,
    };
    for (size_t i = 0; i < sizeof(exec_variants) / sizeof(exec_variants[0]);
         i++) {
        vma.vm_flags = exec_variants[i];
        int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
        CHECK_EQ(-EINVAL, rc);
        CHECK_EQ(0, g_mock_commit_calls);
    }

    /* 2. PROT_NONE (no R/W/E at all) → -EINVAL. */
    vma.vm_flags = VMA_SHARED;
    int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EINVAL, rc);
    CHECK_EQ(0, g_mock_commit_calls);

    /* 3. The refusals published no PTEs and no raw-mmap state. */
    CHECK_EQ(0, g_installed_count);
    CHECK_EQ(false, fb_has_raw_mmap_seen());
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());
}

/* ── Extra tests: fb_write_row_leased & console_notify_resize_locked ── */
static void test_write_row_leased_and_resize(void)
{
    printf("\n--- test_write_row_leased_and_resize ---\n");
    setup_test_environment();

    fb_lease_t lease;
    int rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(lease.held);

    uint32_t row_data[16];
    for (int i = 0; i < 16; i++) row_data[i] = 0x42424242;

    rc = fb_write_row_leased(&lease, 0, 0, row_data, 16 * 4);
    CHECK_EQ(0, rc);
    CHECK_EQ(0x42424242u, g_test_vram[0]);

    /* Out of bounds write */
    rc = fb_write_row_leased(&lease, TEST_FB_WIDTH, 0, row_data, 16 * 4);
    CHECK_EQ(-EINVAL, rc);

    fb_writer_end(&lease);
    CHECK_TRUE(!lease.held);

    /* Write with unheld lease */
    rc = fb_write_row_leased(&lease, 0, 0, row_data, 16 * 4);
    CHECK_EQ(-EINVAL, rc);

    /* console_notify_resize_locked resets cursors */
    Pos.XPosition = 12;
    Pos.YPosition = 8;
    console__test_set_cursors(8, 12, 12, 8);
    console_notify_resize_locked();

    int row = -1, col = -1;
    int32_t pos_x = -1, pos_y = -1;
    console__test_get_cursors(&row, &col, &pos_x, &pos_y);
    CHECK_EQ(0, row);
    CHECK_EQ(0, col);
    CHECK_EQ(0, pos_x);
    CHECK_EQ(0, pos_y);
}

/* ── Panic Simulation / Test Harness (matches kernel/core/panic.c) ── */
static jmp_buf g_panic_jb;
static bool    g_panic_armed = false;
static char    g_last_panic_msg[256];

void panic_enable_fb_if_possible(void)
{
    fb_snapshot_t snap;
    if (fb_snapshot_read(&snap) == 0 && snap.addr != NULL && snap.mapped_size > 0) {
        if (spin_trylock(&Pos.lock)) {
            console_force_enable_locked();
            spin_unlock(&Pos.lock);
        }
    }
}

void kpanic(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vsnprintf(g_last_panic_msg, sizeof(g_last_panic_msg), msg, ap);
    va_end(ap);

    for (const char *p = g_last_panic_msg; *p; p++)
        write_serial_unlocked((unsigned char)*p);

    panic_enable_fb_if_possible();

    if (g_panic_armed) {
        longjmp(g_panic_jb, 1);
    }
}

static void test_kpanic_trylock_no_deadlock(void)
{
    printf("\n--- test_kpanic_trylock_no_deadlock ---\n");
    setup_test_environment();
    console_init();

    /* Subtest 1: Pos.lock is free -> kpanic forces console enable without hang */
    console_surrender_fb();
    CHECK_EQ(false, console__test_is_active());

    reset_serial_capture();
    g_panic_armed = true;
    if (setjmp(g_panic_jb) == 0) {
        kpanic("PANIC: unhandled exception\n");
        CHECK_TRUE(false); /* unreachable */
    }
    g_panic_armed = false;

    /* Assert serial output received panic string */
    CHECK_TRUE(strstr(g_serial_buf, "PANIC: unhandled exception\n") != NULL);
    /* Assert console fb was re-enabled by console_force_enable_locked */
    CHECK_EQ(true, console__test_is_active());
    /* Assert Pos.lock was released (not held) */
    CHECK_EQ(1, spin_trylock(&Pos.lock));
    spin_unlock(&Pos.lock);

    /* Subtest 2: Pos.lock is ALREADY HELD -> kpanic trylock fails gracefully, NO DEADLOCK */
    console_surrender_fb();
    CHECK_EQ(false, console__test_is_active());

    /* Simulate another CPU or interrupted context holding Pos.lock */
    uint64_t flags = spin_lock_irqsave(&Pos.lock);

    reset_serial_capture();
    g_panic_armed = true;
    if (setjmp(g_panic_jb) == 0) {
        kpanic("PANIC: recursive lock test\n");
        CHECK_TRUE(false); /* unreachable */
    }
    g_panic_armed = false;

    /* Serial output still emitted */
    CHECK_TRUE(strstr(g_serial_buf, "PANIC: recursive lock test\n") != NULL);
    /* Console fb active is STILL false because trylock failed and skipped double lock */
    CHECK_EQ(false, console__test_is_active());

    /* Release lock held before panic */
    spin_unlock_irqrestore(&Pos.lock, flags);
    /* Now lock is cleanly free */
    CHECK_EQ(1, spin_trylock(&Pos.lock));
    spin_unlock(&Pos.lock);
}

/* ── Test: Raw mmap boundary and bidirectional ownership EBUSY ── */
static void test_raw_mmap_ownership_boundaries(void)
{
    printf("\n--- test_raw_mmap_ownership_boundaries ---\n");
    setup_test_environment();

    vma_t vma;
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000; /* 4 pages = 16KB */
    vma.vm_file  = NULL;

    /* 1. Normal mode: writable mmap rejected with -EPERM */
    vma.vm_flags = VMA_SHARED | VMA_PROT_WRITE | VMA_PROT_READ;
    int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EPERM, rc);
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());
    CHECK_EQ(false, fb_has_raw_mmap_seen());
    CHECK_EQ(0, g_installed_count);

    /* 2. Readonly mmap succeeds: the commit request goes out once and
     *    only the read-only raw marker is published (the writable
     *    marker stays down — the RO/RW marker difference). */
    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;
    g_current_map_page = 0;
    g_inject_map_fail_page = -1;
    g_inject_unmap_fail_page = -1;
    g_installed_count = 0;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_UC, g_mock_commit_cache);
    CHECK_EQ(true, fb_has_raw_mmap_seen());
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());

    /* 3. Test mode writable mmap and ownership bidirectional EBUSY */
    setup_test_environment();
    fb_set_raw_writable_allowed_for_test(true);
    CHECK_EQ(true, fb_is_raw_writable_allowed());

    void *owner_cookie = (void *)0xCAFE;

    /* 3a: Foreground owner active -> writable mmap rejected with -EBUSY */
    rc = fb_owner_acquire(owner_cookie, 0);
    CHECK_EQ(0, rc);
    vma.vm_flags = VMA_SHARED | VMA_PROT_WRITE | VMA_PROT_READ;
    g_current_map_page = 0;
    g_installed_count = 0;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EBUSY, rc);
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());
    fb_owner_release(owner_cookie);

    /* 3b: Fallback owner active -> writable mmap rejected with -EBUSY */
    rc = fb_owner_register_fallback(owner_cookie, 0);
    CHECK_EQ(0, rc);
    g_current_map_page = 0;
    g_installed_count = 0;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-EBUSY, rc);
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());
    fb_owner_detach(owner_cookie);

    /* 3c: Writable mmap succeeds -> sets sticky g_raw_writable_mmap_seen,
     * causing subsequent acquire and register_fallback to return -EBUSY */
    g_current_map_page = 0;
    g_installed_count = 0;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(true, fb_has_raw_writable_mmap_seen());
    CHECK_EQ(-EBUSY, fb_owner_acquire(owner_cookie, 0));
    CHECK_EQ(-EBUSY, fb_owner_register_fallback(owner_cookie, 0));

    /* 4. A failed commit (device-side prepare OOM) publishes no raw
     *    state: a rejected replace leaves old owners admissible. */
    setup_test_environment();
    fb_set_raw_writable_allowed_for_test(true);
    g_mock_commit_rc = -ENOMEM;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(-ENOMEM, rc);
    CHECK_EQ(false, fb_has_raw_writable_mmap_seen());
    CHECK_EQ(0, fb_owner_acquire(owner_cookie, 0));
    fb_owner_release(owner_cookie);
    CHECK_EQ(0, fb_owner_register_fallback(owner_cookie, 0));
    fb_owner_detach(owner_cookie);
}

/* ── Test: the /dev/fb alias carries the framebuffer cache policy ─────
 *
 * The user alias maps the SAME physical pages as the kernel's
 * framebuffer mapping, so it must carry the same memory type: x86
 * forbids accessing one physical page under two different memory types
 * (IA32_PAT: avoid memory-type aliasing).  The driver asserts the
 * SEMANTIC cache request; the leaf-bit contract lives in the
 * production MM commit test.
 */
static void test_raw_mmap_cache_policy(void)
{
    printf("\n--- test_raw_mmap_cache_policy ---\n");

    vma_t vma;
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000; /* 4 pages = 16KB */
    vma.vm_file  = NULL;

    /* 1. Write-back policy (the QEMU VGA RAM backend publishes it):
     *    the alias request must be write-back. */
    setup_test_environment();
    fb_cache_policy_set(FB_CACHE_POLICY_WRITEBACK);

    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;
    int rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_WB, g_mock_commit_cache);
    CHECK_EQ(0xE0000000ULL, g_mock_commit_first_pa);

    /* 2. Same policy, writable alias (test build only). */
    setup_test_environment();
    fb_cache_policy_set(FB_CACHE_POLICY_WRITEBACK);
    fb_set_raw_writable_allowed_for_test(true);

    vma.vm_flags = VMA_SHARED | VMA_PROT_WRITE | VMA_PROT_READ;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_WB, g_mock_commit_cache);
    CHECK_EQ(true, fb_has_raw_writable_mmap_seen());

    /* 3. Uncacheable policy: the alias request must be uncacheable --
     *    the two mappings must never disagree about the memory type of
     *    the shared physical pages. */
    setup_test_environment();
    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);

    vma.vm_flags = VMA_SHARED | VMA_PROT_READ;
    rc = fb_ops.mmap(NULL, (struct vma *)&vma, mock_commit, &vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, g_mock_commit_calls);
    CHECK_EQ(VM_DEVICE_CACHE_UC, g_mock_commit_cache);

    /* Restore the default policy for the remaining tests. */
    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);
    fb_set_raw_writable_allowed_for_test(false);
}

/* ── Pitched framebuffer fixture (real-hardware geometry) ───────
 *
 * The real panel is 1024x768 with a 4096-byte pitch: the byte stride
 * is LARGER than width*4.  The fixture below reproduces that shape on
 * a RAM buffer — 8 visible pixels per row, a 48-byte pitch (4 padding
 * pixels per row) and 3 visible rows.
 *
 * Every writer must respect the real pitch: the visible pixels of a
 * row live at `y * 48`, and the padding between the end of the visible
 * area and the end of the pitch must never be written.
 */
#define TEST_PITCH_WIDTH   8
#define TEST_PITCH_HEIGHT  3
#define TEST_PITCH_STRIDE  48
#define TEST_PITCH_BYTES   (TEST_PITCH_STRIDE * TEST_PITCH_HEIGHT)
#define TEST_PITCH_PAD     (TEST_PITCH_STRIDE - TEST_PITCH_WIDTH * 4)

#define TEST_PITCH_FILL    0x5A5A5A5Au

static uint32_t g_test_pitch_vram[TEST_PITCH_BYTES / 4] __attribute__((aligned(4096)));

static void setup_pitched_environment(void)
{
    fb_resolution_runtime_reset();
    spin_init(&Pos.lock);
    spin_init(&serial_lock);
    reset_serial_capture();

    memset(g_test_pitch_vram, 0x5A, sizeof(g_test_pitch_vram));

    Pos.XResolution = TEST_PITCH_WIDTH;
    Pos.YResolution = TEST_PITCH_HEIGHT;
    Pos.XPosition = 0;
    Pos.YPosition = 0;
    Pos.Phy_addr = (uint32_t *)(uintptr_t)0xE0000000ULL;
    Pos.FB_addr = g_test_pitch_vram;
    Pos.FB_length = TEST_PITCH_BYTES;

    struct fb_info info = {
        .width = TEST_PITCH_WIDTH,
        .height = TEST_PITCH_HEIGHT,
        .stride = TEST_PITCH_STRIDE,
        .bpp = 32,
        .format = 0,
    };
    fb_bootstrap_state(0xE0000000ULL, TEST_PITCH_BYTES, &info);
    fb_publish_initial_mapping(g_test_pitch_vram, TEST_PITCH_BYTES);
}

/* Read the pixel at (x, y) of the pitched fixture. */
static uint32_t pitch_pixel(uint32_t x, uint32_t y)
{
    const uint8_t *row = (const uint8_t *)g_test_pitch_vram + y * TEST_PITCH_STRIDE;
    uint32_t v;
    memcpy(&v, row + x * 4, sizeof(v));
    return v;
}

/* Every padding byte of row `y` must still carry the fill pattern. */
static bool pitch_padding_intact(uint32_t y)
{
    const uint8_t *row = (const uint8_t *)g_test_pitch_vram + y * TEST_PITCH_STRIDE;
    for (int i = 0; i < TEST_PITCH_PAD; i++)
        if (row[TEST_PITCH_WIDTH * 4 + i] != 0x5A)
            return false;
    return true;
}

/* ── Test: the wide write honours the real pitch ───────────────
 *
 * A write that starts at a non-zero x/y and reaches the last pixel of
 * the row must land exactly on the visible pixels: every written
 * pixel carries its value, the pixels before it are untouched, the
 * neighbouring rows are untouched, and the row padding is untouched.
 */
static void test_write_row_leased_pitched(void)
{
    printf("\n--- test_write_row_leased_pitched ---\n");
    setup_pitched_environment();

    fb_lease_t lease;
    int rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(lease.held);

    /* From column 2 to the 8th (last) pixel of row 1: six pixels. */
    uint32_t pixels[6];
    for (int i = 0; i < 6; i++)
        pixels[i] = 0xC0DE0000u + (uint32_t)i;

    rc = fb_write_row_leased(&lease, 2, 1, pixels, sizeof(pixels));
    CHECK_EQ(0, rc);

    for (uint32_t x = 0; x < TEST_PITCH_WIDTH; x++) {
        uint32_t want = (x >= 2 && x < 8) ? pixels[x - 2] : TEST_PITCH_FILL;
        CHECK_EQ(want, pitch_pixel(x, 1));
    }
    /* The neighbouring rows are untouched. */
    for (uint32_t x = 0; x < TEST_PITCH_WIDTH; x++) {
        CHECK_EQ(TEST_PITCH_FILL, pitch_pixel(x, 0));
        CHECK_EQ(TEST_PITCH_FILL, pitch_pixel(x, 2));
    }
    /* The padding of every row is untouched. */
    for (uint32_t y = 0; y < TEST_PITCH_HEIGHT; y++)
        CHECK_TRUE(pitch_padding_intact(y));

    fb_writer_end(&lease);
}

/* ── Test: the illegal writer requests change nothing ──────────
 *
 * The same start point (x=2, y=1) is used for every rejection:
 * out-of-bounds geometry, a non-integral pixel length and a lease
 * that is not held.  Each one must return -EINVAL and leave the
 * framebuffer byte-for-byte unchanged.
 */
static void test_write_row_leased_rejects(void)
{
    printf("\n--- test_write_row_leased_rejects ---\n");
    setup_pitched_environment();

    uint8_t before[TEST_PITCH_BYTES];
    memcpy(before, g_test_pitch_vram, sizeof(before));

    uint32_t pixels[8];
    for (int i = 0; i < 8; i++)
        pixels[i] = 0xBADBAD00u + (uint32_t)i;

    fb_lease_t lease;
    int rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(0, rc);

    /* 1. Past the last column: the row would leave the visible area. */
    rc = fb_write_row_leased(&lease, TEST_PITCH_WIDTH, 1, pixels, 4);
    CHECK_EQ(-EINVAL, rc);

    /* 2. A row that starts inside the visible area but runs past it. */
    rc = fb_write_row_leased(&lease, 4, 1, pixels, 8 * 4);
    CHECK_EQ(-EINVAL, rc);

    /* 3. Past the last row. */
    rc = fb_write_row_leased(&lease, 2, TEST_PITCH_HEIGHT, pixels, 4 * 4);
    CHECK_EQ(-EINVAL, rc);

    /* 4. A non-integral pixel length: 6 bytes is 1.5 pixels.  Before
     *    the writer rejected this, the stray half pixel landed in the
     *    next pixel. */
    rc = fb_write_row_leased(&lease, 2, 1, pixels, 6);
    CHECK_EQ(-EINVAL, rc);

    /* 5. Past the end of the mapped range: the mapping is exactly three
     *    rows deep, so a row that starts inside the visible area but
     *    runs past the last mapped byte is refused. */
    rc = fb_write_row_leased(&lease, 4, 2, pixels, 8 * 4);
    CHECK_EQ(-EINVAL, rc);

    CHECK_EQ(0, memcmp(before, g_test_pitch_vram, sizeof(before)));

    fb_writer_end(&lease);

    /* 6. The same start point with a lease that is not held. */
    rc = fb_write_row_leased(&lease, 2, 1, pixels, 8 * 4);
    CHECK_EQ(-EINVAL, rc);
    CHECK_EQ(0, memcmp(before, g_test_pitch_vram, sizeof(before)));

    /* 7. A mapping that is too short for the row: the writer must
     *    refuse it instead of writing past the mapped range. */
    setup_pitched_environment();
    fb_publish_initial_mapping(g_test_pitch_vram, TEST_PITCH_BYTES - 16);
    rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(0, rc);
    rc = fb_write_row_leased(&lease, 4, 2, pixels, 8 * 4);
    CHECK_EQ(-EINVAL, rc);
    CHECK_EQ(0, memcmp(before, g_test_pitch_vram, sizeof(before)));
    fb_writer_end(&lease);
}

/* ── Test: the scroll geometry ─────────────────────────────────
 *
 * Three scanlines, two visible pixels each, a 12-byte pitch.  The
 * four padding bytes of every row carry a sentinel that must never
 * move.  A one-row scroll moves the visible pixels up, clears the
 * bottom visible row and leaves the padding alone.
 *
 * A framebuffer whose writable range is one byte short must not be
 * written at all.
 */
#define SCROLL_ROWS     3
#define SCROLL_WIDTH    2
#define SCROLL_STRIDE   12
#define SCROLL_FB_BYTES (SCROLL_STRIDE * SCROLL_ROWS)
#define SCROLL_PAD      (SCROLL_STRIDE - SCROLL_WIDTH * 4)

static uint8_t g_scroll_fb[SCROLL_FB_BYTES];

static uint32_t scroll_pixel(uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, g_scroll_fb + y * SCROLL_STRIDE + x * 4, sizeof(v));
    return v;
}

static void scroll_fill_pattern(void)
{
    memset(g_scroll_fb, 0, sizeof(g_scroll_fb));
    for (uint32_t y = 0; y < SCROLL_ROWS; y++) {
        for (uint32_t x = 0; x < SCROLL_WIDTH; x++) {
            uint32_t v = 0x0A0B0000u + y * 0x100u + x;
            memcpy(g_scroll_fb + y * SCROLL_STRIDE + x * 4, &v, sizeof(v));
        }
        memset(g_scroll_fb + y * SCROLL_STRIDE + SCROLL_WIDTH * 4, 0xEE,
               SCROLL_PAD);
    }
}

static void test_fb_scroll_geometry(void)
{
    printf("\n--- test_fb_scroll_geometry ---\n");
    scroll_fill_pattern();

    printk_fb_view_t view = {
        .fb = g_scroll_fb,
        .width = SCROLL_WIDTH,
        .height = SCROLL_ROWS,
        .stride = SCROLL_STRIDE,
        .fb_bytes = SCROLL_FB_BYTES,
    };

    CHECK_TRUE(fb_scroll_rows(&view, 1));

    /* The visible pixels moved up by one row. */
    for (uint32_t x = 0; x < SCROLL_WIDTH; x++) {
        CHECK_EQ(scroll_pixel(x, 0), 0x0A0B0000u + 1 * 0x100u + x);
        CHECK_EQ(scroll_pixel(x, 1), 0x0A0B0000u + 2 * 0x100u + x);
    }
    /* The bottom visible row was cleared. */
    for (uint32_t x = 0; x < SCROLL_WIDTH; x++)
        CHECK_EQ(0u, scroll_pixel(x, SCROLL_ROWS - 1));

    /* The padding never moved. */
    for (uint32_t y = 0; y < SCROLL_ROWS; y++)
        for (int i = 0; i < SCROLL_PAD; i++)
            CHECK_EQ(0xEEu, g_scroll_fb[y * SCROLL_STRIDE + SCROLL_WIDTH * 4 + i]);

    /* A framebuffer one byte short: nothing may be written. */
    uint8_t pristine[SCROLL_FB_BYTES];
    memset(pristine, 0x5A, sizeof(pristine));
    memcpy(g_scroll_fb, pristine, sizeof(g_scroll_fb));

    printk_fb_view_t short_view = {
        .fb = g_scroll_fb,
        .width = SCROLL_WIDTH,
        .height = SCROLL_ROWS,
        .stride = SCROLL_STRIDE,
        .fb_bytes = SCROLL_FB_BYTES - 1,
    };
    CHECK_TRUE(!fb_scroll_rows(&short_view, 1));
    CHECK_EQ(0, memcmp(pristine, g_scroll_fb, sizeof(pristine)));

    /* The same short range refuses the packed write-back too. */
    uint8_t packed[SCROLL_WIDTH * 4 * SCROLL_ROWS];
    memset(packed, 0x33, sizeof(packed));
    CHECK_TRUE(!fb_copy_rows(&short_view, packed, SCROLL_WIDTH * 4, SCROLL_ROWS));
    CHECK_EQ(0, memcmp(pristine, g_scroll_fb, sizeof(pristine)));

    /* A full write-back copies the visible pixels row by row and
     * leaves the padding alone. */
    scroll_fill_pattern();
    CHECK_TRUE(fb_copy_rows(&view, packed, SCROLL_WIDTH * 4, SCROLL_ROWS));
    for (uint32_t y = 0; y < SCROLL_ROWS; y++) {
        for (uint32_t x = 0; x < SCROLL_WIDTH; x++)
            CHECK_EQ(0x33333333u, scroll_pixel(x, y));
        for (int i = 0; i < SCROLL_PAD; i++)
            CHECK_EQ(0xEEu, g_scroll_fb[y * SCROLL_STRIDE + SCROLL_WIDTH * 4 + i]);
    }

    /* The packed row move: row `src_row` moves to row `dst_row`.  The
     * ranges must be disjoint — the shadow scroll moves one row at a
     * time. */
    uint8_t packed_move[SCROLL_WIDTH * 4 * SCROLL_ROWS];
    for (size_t i = 0; i < sizeof(packed_move); i++)
        packed_move[i] = (uint8_t)(i + 1u);

    CHECK_TRUE(fb_move_rows_packed(packed_move, SCROLL_WIDTH * 4, 0, 1, 1));
    CHECK_EQ(0x09u, packed_move[0]);                       /* row 0 <- old row 1 */
    CHECK_EQ(0x10u, packed_move[SCROLL_WIDTH * 4 - 1]);   /* row 0 last byte */
    CHECK_EQ(0x09u, packed_move[SCROLL_WIDTH * 4]);       /* row 1 untouched */
    CHECK_EQ(0x11u, packed_move[SCROLL_WIDTH * 4 * 2]);   /* row 2 untouched */

    /* Overlapping ranges are refused: a half-applied move would
     * corrupt the shadow. */
    CHECK_TRUE(!fb_move_rows_packed(packed_move, SCROLL_WIDTH * 4, 0, 1, 2));

    /* Scrolling must preserve each original source row until it is read. */
    uint32_t shadow[] = { 1, 2, 3, 4, 5, 6 };
    static const uint32_t expected[] = { 3, 4, 5, 6, 0, 0 };
    fb_scroll_packed((uint8_t *)shadow, 2 * sizeof(uint32_t), 3, 1);
    CHECK_EQ(0, memcmp(shadow, expected, sizeof(expected)));
}

int main(void)
{
    printf("=== Test Runner: Framebuffer Writers Admission ===\n");

    test_present_fault_releases_lease();
    test_view_aba_stale();
    test_console_transition_serial_only();
    test_mmap_partial_failure();
    test_fb_fixed_failure_keeps_old_mapping();
    test_write_row_leased_and_resize();
    test_kpanic_trylock_no_deadlock();
    test_raw_mmap_ownership_boundaries();
    test_raw_mmap_cache_policy();
    test_fb_mmap_prot_rejection();
    test_write_row_leased_pitched();
    test_write_row_leased_rejects();
    test_fb_scroll_geometry();

    printf("\n  ---\n  Total: %u | Passed: %u | Failed: %d\n",
           __test_stats.total, __test_stats.passed, g_failed);

    if (g_failed != 0) {
        printf("  >>> TESTS FAILED <<<\n");
        return 1;
    }

    printf("  >>> ALL TESTS PASSED <<<\n");
    return 0;
}

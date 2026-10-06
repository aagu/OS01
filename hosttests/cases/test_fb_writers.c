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

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <uapi/gfx.h>
#include <driver/fb_state.h>
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

#undef mmap

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
static size_t   g_installed_count = 0;
static int      g_inject_map_fail_page = -1;
static int      g_inject_unmap_fail_page = -1;
static int      g_current_map_page = 0;

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint64_t flags)
{
    (void)pgdir; (void)flags;
    if (g_current_map_page == g_inject_map_fail_page) {
        g_current_map_page++;
        return -ENOMEM;
    }
    g_current_map_page++;

    if (g_installed_count < MAX_TRACKED_PAGES) {
        g_installed_ptes[g_installed_count++] = virt;
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
            }
            g_installed_count--;
            return;
        }
    }
}

int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out, uint32_t *vm_out)
{
    (void)pgdir;
    for (size_t i = 0; i < g_installed_count; i++) {
        if (g_installed_ptes[i] == virt) {
            if (phys_out) *phys_out = 0;
            if (vm_out) *vm_out = VM_PRESENT;
            return 0;
        }
    }
    return -ENOENT;
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

    /* View was created at generation 1 */
    /* Now publish two mode changes: ABA back to same dimensions */
    /* Change 1: generation becomes 2 */
    fb_state__test_set_generation(2);
    /* Change 2: generation becomes 3, dimensions identical */
    fb_state__test_set_generation(3);

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
    vma.vm_flags = VMA_SHARED;
    vma.vm_start = 0x10000;
    vma.vm_end   = 0x14000; /* 4 pages = 16KB */
    vma.vm_file  = NULL;

    /* Subcase 4a: intermediate mapping failure on page 2 (0-indexed 1) */
    g_current_map_page = 0;
    g_inject_map_fail_page = 1;
    g_inject_unmap_fail_page = -1;
    g_installed_count = 0;

    int rc = fb_ops.mmap(NULL, (struct vma *)&vma);
    CHECK_EQ(-ENOMEM, rc);

    /* Assert all installed PTEs revoked */
    CHECK_EQ(0, g_installed_count);
    /* Clean rollback: raw_mmap_seen remains false */
    CHECK_EQ(false, fb_has_raw_mmap_seen());

    /* Subcase 4b: rollback unmap fails -> untrusted cleanup sets sticky=true */
    g_current_map_page = 0;
    g_inject_map_fail_page = 1;
    g_inject_unmap_fail_page = 0; /* unmap of page 0 fails! */
    g_installed_count = 0;

    rc = fb_ops.mmap(NULL, (struct vma *)&vma);
    CHECK_EQ(-ENOMEM, rc);
    /* Untrusted unmap: sticky set to true for safety! */
    CHECK_EQ(true, fb_has_raw_mmap_seen());

    /* Subcase 4c: successful mmap sets sticky=true */
    setup_test_environment();
    CHECK_EQ(false, fb_has_raw_mmap_seen());

    g_current_map_page = 0;
    g_inject_map_fail_page = -1;
    g_inject_unmap_fail_page = -1;
    g_installed_count = 0;

    rc = fb_ops.mmap(NULL, (struct vma *)&vma);
    CHECK_EQ(0, rc);
    CHECK_EQ(4, g_installed_count);
    CHECK_EQ(true, fb_has_raw_mmap_seen());
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

int main(void)
{
    printf("=== Test Runner: Framebuffer Writers Admission ===\n");

    test_present_fault_releases_lease();
    test_view_aba_stale();
    test_console_transition_serial_only();
    test_mmap_partial_failure();
    test_write_row_leased_and_resize();
    test_kpanic_trylock_no_deadlock();

    printf("\n  ---\n  Total: %u | Passed: %u | Failed: %d\n",
           __test_stats.total, __test_stats.passed, g_failed);

    if (g_failed != 0) {
        printf("  >>> TESTS FAILED <<<\n");
        return 1;
    }

    printf("  >>> ALL TESTS PASSED <<<\n");
    return 0;
}

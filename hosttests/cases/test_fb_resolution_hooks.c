/*
 * hosttests/cases/test_fb_resolution_hooks.c
 *
 * Controlled fault-injection surface for the QEMU resolution switcher
 * (Task 9).  Host-compiles the REAL kernel/driver/fb_test.c together with
 * the REAL fb_state.c / bga.c / fb.c compiled with FB_RESOLUTION_TEST, so
 * the whole injection path is exercised end to end against the fake BGA
 * transport.
 *
 * Contract under test:
 *   - Cold file ioctl/release entries initialize the test lock once
 *     before acquiring it, independently of bootstrap/reset ordering.
 *   - fb_test_req / fb_test_snapshot layout + command numbers (spec §8).
 *   - SNAPSHOT is a pure-output exception: it copies no request struct,
 *     samples DISPI index 0..10, restores the saved index, reports
 *     active_writers, and a user-copy fault leaks no display_mutex.
 *   - A register-readback fault armed for (target_pid, token) is consumed
 *     exactly once by the next layout-changing SET for that PID; a GET
 *     never consumes it, and another PID's SET never consumes it.
 *   - ARM_MISMATCH rolls the layout back (generation++, backend stays
 *     usable); ARM_ROLLBACK_FAILURE permanently closes the backend.
 *   - HOLD_WRITER acquires a real writer lease (a SET drains and times out
 *     with -EBUSY); a repeated HOLD does not double-count; RELEASE_WRITER
 *     and release_file both drop the lease.
 *   - ARM_TERMINAL_ENOMEM is consumed once by the target PID only;
 *     zero tokens and pending-slot replacement are rejected unchanged.
 *   - A hardware fault clears every pending fault.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <uapi/fb_test.h>
#include <driver/fb.h>
#include <driver/fb_state.h>
#include <driver/fb_test.h>
#include <driver/bga.h>
#include <core/printk.h>
#include <tty/console.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <bus/pci/pci.h>

static int g_failed = 0;
/* Use the host process ABI, avoiding OS01's inline fork syscall wrapper. */
extern int host_fork(void) __asm__("fork");
extern int host_waitpid(int pid, int *status, int options) __asm__("waitpid");
extern void _exit(int status) __attribute__((noreturn));
static bool g_observe_cold_lock;
static spinlock_T *g_initialized_lock;
static unsigned g_lock_inits, g_lock_acquires, g_uninitialized_acquires;

void fb_resolution_test_spin_init(spinlock_T *lock)
{
    if (!g_observe_cold_lock) return;
    g_initialized_lock = lock;
    g_lock_inits++;
}

void fb_resolution_test_spin_acquire(spinlock_T *lock)
{
    if (!g_observe_cold_lock) return;
    g_lock_acquires++;
    if (lock != g_initialized_lock) g_uninitialized_acquires++;
}

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

/* ── Compile-time layout pins (spec §8) ── */
_Static_assert(sizeof(struct fb_test_req) == 32, "fb_test_req must be 32 bytes");
_Static_assert(offsetof(struct fb_test_req, token) == 16, "fb_test_req token offset must be 16");
_Static_assert(sizeof(struct fb_test_snapshot) == 64, "fb_test_snapshot must be 64 bytes");
_Static_assert(offsetof(struct fb_test_snapshot, regs) == 32, "snapshot regs offset must be 32");
_Static_assert(offsetof(struct fb_test_snapshot, active_writers) == 56, "snapshot writers offset must be 56");

position Pos;

/* ── Fake hardware (mirrors test_fb_resolution_ioctl.c) ── */
#define FAKE_PCI_BAR0_BASE 0xE0000000ULL
#define FAKE_PCI_CMD_DEFAULT 0x0003U
#define FAKE_PCI_STATUS_DEFAULT 0x02900000U

static uint32_t s_pci_conf[16];
static uint16_t s_dispi_regs[16];
static uint16_t s_dispi_index = 0;

typedef enum {
    FAKE_BGA_IDLE = 0,
    FAKE_BGA_PROGRAMMING,
    FAKE_BGA_APPLY_VERIFY,
    FAKE_BGA_ROLLING_BACK,
} fake_bga_phase_t;

static fake_bga_phase_t s_phase = FAKE_BGA_IDLE;
/* 16 MiB backing store — fb_set_mode clears the whole new active area. */
static uint32_t g_test_vram[16 * 1024 * 1024 / 4];

static uint16_t fake_io_read16(uint16_t port)
{
    if (port == VBE_DISPI_IOPORT_INDEX) {
        return s_dispi_index;
    }
    if (port == VBE_DISPI_IOPORT_DATA) {
        uint16_t idx = s_dispi_index;
        if (s_dispi_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            if (idx == VBE_DISPI_INDEX_XRES) return 1920;
            if (idx == VBE_DISPI_INDEX_YRES) return 1080;
            if (idx == VBE_DISPI_INDEX_BPP)  return 32;
        }
        if (idx < 16) return s_dispi_regs[idx];
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
        if (idx == VBE_DISPI_INDEX_ENABLE) {
            if (val == VBE_DISPI_DISABLED) {
                if (s_phase == FAKE_BGA_APPLY_VERIFY) s_phase = FAKE_BGA_ROLLING_BACK;
                else s_phase = FAKE_BGA_PROGRAMMING;
            } else if (val & VBE_DISPI_NOCLEARMEM) {
                if (s_phase == FAKE_BGA_PROGRAMMING) s_phase = FAKE_BGA_APPLY_VERIFY;
            }
        }
        if (idx == VBE_DISPI_INDEX_VIRT_WIDTH && val > 0) {
            s_dispi_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] =
                (uint16_t)((16 * 1024 * 1024) / (val * 4));
        }
        if (idx < 16) s_dispi_regs[idx] = val;
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
    if (dword_idx == 4 && val == 0xFFFFFFFFU) {
        s_pci_conf[4] = 0xFF000008U; /* 16 MB mask */
        return 0;
    }
    s_pci_conf[dword_idx] = val;
    return 0;
}

/* ── User-copy instrumentation ── */
static uint32_t g_copy_from_user_count = 0;
static uint32_t g_copy_to_user_count = 0;
static bool g_inject_copy_to_fail = false;

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    (void)len; (void)writable;
    return addr != 0;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    g_copy_from_user_count++;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_inject_copy_to_fail) return -EFAULT;
    g_copy_to_user_count++;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ── Kernel globals / stubs the production TUs reference ── */
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }

file_t *file_alloc(void) { return (file_t *)calloc(1, sizeof(file_t)); }

void vfs_node_put(struct vfs_node *n) { (void)n; }
void write_serial_unlocked(unsigned char c) { (void)c; }
void write_serial(char c) { (void)c; }
int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint64_t flags)
{ (void)pgdir; (void)phys; (void)virt; (void)flags; return 0; }
void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt) { (void)pgdir; (void)virt; }
int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out, uint32_t *vm_out)
{ (void)pgdir; (void)virt; (void)phys_out; (void)vm_out; return -ENOENT; }

/* devfs registration spy */
static char g_reg_names[8][DEVFS_NAME_MAX];
static int g_reg_count = 0;
int devfs_register_chrdev(const char *name, void *private_data, const struct devfs_ops *ops)
{
    (void)private_data; (void)ops;
    if (name && g_reg_count < 8) {
        strncpy(g_reg_names[g_reg_count], name, DEVFS_NAME_MAX - 1);
        g_reg_names[g_reg_count][DEVFS_NAME_MAX - 1] = '\0';
    }
    g_reg_count++;
    return 0;
}

#include <driver/font.h>
void putchar_at_snap(const struct fb_snapshot *snap, int col, int row,
                     unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{ (void)snap; (void)col; (void)row; (void)FRcolor; (void)BKcolor; (void)c; }

static psf2_t g_test_font = {
    .magic = 0x864ab572, .version = 0, .headersize = 32, .flags = 0,
    .numglyph = 256, .bytesperglyph = 16, .height = 16, .width = 8,
};
psf2_t *font = &g_test_font;

/* ── Environment setup ── */
static void setup_env(void)
{
    fb_resolution_runtime_reset();
    bga_reset_for_test();

    g_copy_from_user_count = 0;
    g_copy_to_user_count = 0;
    g_inject_copy_to_fail = false;
    g_reg_count = 0;

    memset(s_pci_conf, 0, sizeof(s_pci_conf));
    s_pci_conf[0] = 0x11111234U;
    s_pci_conf[1] = FAKE_PCI_STATUS_DEFAULT | FAKE_PCI_CMD_DEFAULT;
    s_pci_conf[2] = 0x03000000U;
    s_pci_conf[4] = (uint32_t)(FAKE_PCI_BAR0_BASE | 0x08U);

    memset(s_dispi_regs, 0, sizeof(s_dispi_regs));
    s_dispi_index = 0;
    s_dispi_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s_dispi_regs[VBE_DISPI_INDEX_XRES] = 800;
    s_dispi_regs[VBE_DISPI_INDEX_YRES] = 600;
    s_dispi_regs[VBE_DISPI_INDEX_BPP] = 32;
    s_dispi_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED;
    s_dispi_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = 800;
    s_dispi_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = 4096;
    s_dispi_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = 256;
    s_phase = FAKE_BGA_IDLE;

    bga_set_transport_for_test(fake_io_read16, fake_io_write16,
                               fake_pci_read32, fake_pci_write32);

    memset(&Pos, 0, sizeof(Pos));
    Pos.XResolution = 800;
    Pos.YResolution = 600;
    Pos.Phy_addr = (uint32_t *)FAKE_PCI_BAR0_BASE;
    Pos.FB_addr = g_test_vram;
    Pos.FB_length = 16 * 1024 * 1024;

    struct fb_info init_info = {
        .width = 800, .height = 600, .stride = 800 * 4, .bpp = 32,
        .format = FB_FORMAT_RGB32,
    };
    fb_bootstrap_state(FAKE_PCI_BAR0_BASE, 16 * 1024 * 1024, &init_info);
    fb_publish_initial_mapping(g_test_vram, 16 * 1024 * 1024);
    fb_test_reset();

    mock_current_task.pid = 1;
}

static void probe_bga(void)
{
    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.vendor = BGA_PCI_VENDOR_ID;
    pdev.device = BGA_PCI_DEVICE_ID;
    pdev.class_code = 0x030000;
    pdev.bars[0].kind = PCI_BAR_MMIO32;
    pdev.bars[0].address = FAKE_PCI_BAR0_BASE;
    pdev.bars[0].valid = true;

    struct pci_device_id id;
    memset(&id, 0, sizeof(id));
    id.vendor = BGA_PCI_VENDOR_ID;
    id.device = BGA_PCI_DEVICE_ID;

    CHECK_EQ(0, bga_probe(&pdev, &id));
    Pos.FB_addr = g_test_vram;
}

static struct fb_test_req make_req(uint32_t target_pid, uint64_t token)
{
    struct fb_test_req r;
    memset(&r, 0, sizeof(r));
    r.version = 1;
    r.target_pid = target_pid;
    r.token = token;
    return r;
}

static int do_set(uint32_t w, uint32_t h)
{
    struct fb_set_mode_req req = { .width = w, .height = h, .bpp = 0 };
    return fb_ops.ioctl(NULL, FBIOSET_MODE, &req);
}

/* ── Test 1: ABI layout ── */
static void test_hooks_abi(void)
{
    printf("\n--- test_hooks_abi ---\n");
    CHECK_EQ(32, sizeof(struct fb_test_req));
    CHECK_EQ(64, sizeof(struct fb_test_snapshot));
    CHECK_EQ(16, offsetof(struct fb_test_req, token));
    CHECK_EQ(56, offsetof(struct fb_test_snapshot, active_writers));
    CHECK_EQ(0x4650, FBIOTEST_SNAPSHOT);
    CHECK_EQ(0x4651, FBIOTEST_ARM_MISMATCH);
    CHECK_EQ(0x4652, FBIOTEST_ARM_ROLLBACK_FAILURE);
    CHECK_EQ(0x4653, FBIOTEST_HOLD_WRITER);
    CHECK_EQ(0x4654, FBIOTEST_RELEASE_WRITER);
    CHECK_EQ(0x4655, FBIOTEST_ARM_TERMINAL_ENOMEM);
    CHECK_EQ(0x4656, FBIOTEST_CONSUME_TERMINAL_ENOMEM);
    CHECK_EQ(0x4657, FBIOTEST_TERMINAL_STATUS);
}

/* ── Test 2: node registration + request validation ── */
static void test_hooks_registration_and_validation(void)
{
    printf("\n--- test_hooks_registration_and_validation ---\n");
    setup_env();

    /* fb_test_init registers /dev/fbtest exactly once. */
    CHECK_EQ(0, fb_test_init());
    CHECK_EQ(0, fb_test_init());
    CHECK_EQ(1, g_reg_count);
    CHECK_TRUE(g_reg_count == 1 && strcmp(g_reg_names[0], "fbtest") == 0);

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));
    CHECK_TRUE(f != NULL);

    struct fb_test_req bad = make_req(0, 1);
    bad.version = 2;
    CHECK_EQ(-EINVAL, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &bad));

    bad = make_req(0, 1);
    bad.reserved[1] = 7;
    CHECK_EQ(-EINVAL, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &bad));

    bad = make_req(0, 0); /* ARM requires a non-zero token */
    CHECK_EQ(-EINVAL, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &bad));

    CHECK_EQ(-ENOTTY, fb_test_ops.ioctl_file(f, 0x9999, &bad));

    fb_test_ops.release_file(f);
}

/* ── Test 3: SNAPSHOT regs + index restore ── */
static void test_snapshot_regs_and_index_restore(void)
{
    printf("\n--- test_snapshot_regs_and_index_restore ---\n");
    setup_env();
    probe_bga();

    /* Deliberately leave the hardware index somewhere non-zero. */
    fake_io_write16(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_snapshot snap;
    memset(&snap, 0xAA, sizeof(snap));
    g_copy_from_user_count = 0;

    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_SNAPSHOT, &snap));
    CHECK_EQ(0, g_copy_from_user_count);           /* pure output */
    CHECK_EQ(1, g_copy_to_user_count);

    for (int i = 0; i < 11; i++) {
        CHECK_EQ(s_dispi_regs[i], snap.regs[i]);
    }
    CHECK_EQ(0, snap.reserved);
    CHECK_EQ(800, snap.state.info.width);
    CHECK_EQ(600, snap.state.info.height);
    CHECK_EQ(0, snap.active_writers);
    CHECK_EQ(VBE_DISPI_INDEX_BPP, fake_io_read16(VBE_DISPI_IOPORT_INDEX));

    fb_test_ops.release_file(f);
}

/* ── Test 4: SNAPSHOT user-copy fault must not leak the mutex ── */
static void test_snapshot_user_fault_no_leak(void)
{
    printf("\n--- test_snapshot_user_fault_no_leak ---\n");
    setup_env();
    probe_bga();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_snapshot snap;
    g_inject_copy_to_fail = true;
    CHECK_EQ(-EFAULT, fb_test_ops.ioctl_file(f, FBIOTEST_SNAPSHOT, &snap));
    g_inject_copy_to_fail = false;

    /* The display mutex was released: a normal SET still works. */
    CHECK_EQ(0, do_set(1280, 720));

    fb_test_ops.release_file(f);
}

/* ── Test 5: ARM_MISMATCH consumed once by the target-PID layout SET ── */
static void test_arm_mismatch_consumed_by_set(void)
{
    printf("\n--- test_arm_mismatch_consumed_by_set ---\n");
    setup_env();
    probe_bga();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_state st0;
    CHECK_EQ(0, fb_get_state(&st0));

    struct fb_test_req req = make_req(1, 0xABCD);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &req));

    /* A GET must NOT consume the armed fault. */
    struct fb_state st_get;
    CHECK_EQ(0, fb_get_state(&st_get));

    mock_current_task.pid = 1;
    CHECK_EQ(-EIO, do_set(1280, 720));   /* rolled back: hardware mismatch */

    struct fb_state st1;
    CHECK_EQ(0, fb_get_state(&st1));
    CHECK_EQ(800, st1.info.width);       /* layout restored */
    CHECK_TRUE(st1.generation != st0.generation); /* redraw invalidated */

    /* The fault was consumed exactly once: the next SET applies cleanly. */
    CHECK_EQ(0, do_set(1280, 720));
    struct fb_state st2;
    CHECK_EQ(0, fb_get_state(&st2));
    CHECK_EQ(1280, st2.info.width);

    fb_test_ops.release_file(f);
}

/* ── Test 6: another PID's SET neither consumes nor faults ── */
static void test_arm_other_pid_not_consumed(void)
{
    printf("\n--- test_arm_other_pid_not_consumed ---\n");
    setup_env();
    probe_bga();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_req req = make_req(100, 0x1234);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &req));

    /* A SET from a different PID applies cleanly and does not consume. */
    mock_current_task.pid = 200;
    CHECK_EQ(0, do_set(1280, 720));

    /* Now the target PID's SET consumes the still-armed fault. */
    mock_current_task.pid = 100;
    CHECK_EQ(-EIO, do_set(640, 480));

    fb_test_ops.release_file(f);
}

/* ── Test 7: ARM_ROLLBACK_FAILURE permanently closes the backend ── */
static void test_arm_rollback_failure(void)
{
    printf("\n--- test_arm_rollback_failure ---\n");
    setup_env();
    probe_bga();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_req req = make_req(1, 0x5678);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_ROLLBACK_FAILURE, &req));

    mock_current_task.pid = 1;
    CHECK_EQ(-EIO, do_set(1280, 720));

    /* Backend is permanently failed: queries report EIO and never touch hw. */
    struct fb_state st;
    CHECK_EQ(-EIO, fb_get_state(&st));
    CHECK_EQ(-EIO, do_set(640, 480));

    fb_test_ops.release_file(f);
}

/* ── Test 8: HOLD/RELEASE writer lease ── */
static void test_hold_writer_lease(void)
{
    printf("\n--- test_hold_writer_lease ---\n");
    setup_env();
    probe_bga();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_req req = make_req(1, 0);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_HOLD_WRITER, &req));
    CHECK_EQ(1, fb_active_writers_count());

    /* A repeated HOLD must not double-count. */
    CHECK_EQ(-EBUSY, fb_test_ops.ioctl_file(f, FBIOTEST_HOLD_WRITER, &req));
    CHECK_EQ(1, fb_active_writers_count());

    /* The held lease makes a layout SET drain and time out (-EBUSY). */
    g_mock_clock_step_ns = 2000000000ULL;
    mock_current_task.pid = 1;
    CHECK_EQ(-EBUSY, do_set(1280, 720));
    g_mock_clock_step_ns = 0;
    CHECK_EQ(1, fb_active_writers_count());

    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_RELEASE_WRITER, &req));
    CHECK_EQ(0, fb_active_writers_count());
    CHECK_EQ(0, do_set(1280, 720));

    /* release_file auto-releases a still-held lease. */
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_HOLD_WRITER, &req));
    CHECK_EQ(1, fb_active_writers_count());
    fb_test_ops.release_file(f);
    CHECK_EQ(0, fb_active_writers_count());
}

/* ── Test 9: ARM_TERMINAL_ENOMEM is per-target-PID, one-shot ── */
static void test_terminal_enomem_target_pid_once(void)
{
    printf("\n--- test_terminal_enomem_target_pid_once ---\n");
    setup_env();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_req req = make_req(100, 0x42);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_TERMINAL_ENOMEM, &req));

    /* Non-target PID sees nothing. */
    CHECK_EQ(0, fb_test_consume_terminal_enomem(101));
    /* Target PID consumes it exactly once. */
    CHECK_EQ(1, fb_test_consume_terminal_enomem(100));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(100));

    fb_test_ops.release_file(f);
}

static void test_terminal_arm_rejects_zero_token_without_side_effects(void)
{
    printf("\n--- test_terminal_arm_rejects_zero_token_without_side_effects ---\n");
    setup_env();
    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));
    struct fb_test_req query = make_req(0, 0);
    struct fb_test_req bad = make_req(100, 0);
    CHECK_EQ(-EINVAL, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_TERMINAL_ENOMEM, &bad));
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_TERMINAL_STATUS, &query));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(100));

    struct fb_test_req armed = make_req(100, 0x42);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_TERMINAL_ENOMEM, &armed));
    bad.target_pid = 200;
    CHECK_EQ(-EINVAL, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_TERMINAL_ENOMEM, &bad));
    CHECK_EQ(1, fb_test_ops.ioctl_file(f, FBIOTEST_TERMINAL_STATUS, &query));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(200));
    CHECK_EQ(1, fb_test_consume_terminal_enomem(100));
    CHECK_EQ(256, fb_test_ops.ioctl_file(f, FBIOTEST_TERMINAL_STATUS, &query));
    fb_test_ops.release_file(f);
    free(f);
}

static void test_terminal_arm_preserves_pending_and_allows_rearm(void)
{
    printf("\n--- test_terminal_arm_preserves_pending_and_allows_rearm ---\n");
    setup_env();
    file_t *first = NULL, *second = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &first));
    mock_current_task.pid = 2;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &second));

    struct fb_test_req query = make_req(0, 0);
    struct fb_test_req original = make_req(100, 1);
    struct fb_test_req other = make_req(200, 2);
    CHECK_EQ(0, fb_test_ops.ioctl_file(first, FBIOTEST_ARM_TERMINAL_ENOMEM, &original));
    CHECK_EQ(-EBUSY, fb_test_ops.ioctl_file(first, FBIOTEST_ARM_TERMINAL_ENOMEM, &original));
    CHECK_EQ(-EBUSY, fb_test_ops.ioctl_file(second, FBIOTEST_ARM_TERMINAL_ENOMEM, &other));
    CHECK_EQ(1, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(200));
    fb_test_ops.release_file(second);
    free(second);
    CHECK_EQ(1, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));
    CHECK_EQ(1, fb_test_consume_terminal_enomem(100));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(100));
    CHECK_EQ(256, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));

    /* Consumed slots can be armed again without losing cumulative status. */
    CHECK_EQ(0, fb_test_ops.ioctl_file(first, FBIOTEST_ARM_TERMINAL_ENOMEM, &other));
    CHECK_EQ(257, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));
    CHECK_EQ(1, fb_test_consume_terminal_enomem(200));
    CHECK_EQ(512, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));

    /* target_pid=0 still resolves to the opener's PID, not the caller's. */
    original.target_pid = 0;
    CHECK_EQ(0, fb_test_ops.ioctl_file(first, FBIOTEST_ARM_TERMINAL_ENOMEM, &original));
    CHECK_EQ(0, fb_test_consume_terminal_enomem(2));
    CHECK_EQ(1, fb_test_consume_terminal_enomem(1));
    CHECK_EQ(768, fb_test_ops.ioctl_file(first, FBIOTEST_TERMINAL_STATUS, &query));
    fb_test_ops.release_file(first);
    free(first);
}

/* ── Test 10: a hardware fault clears all pending faults ── */
static void test_fault_clears_pending(void)
{
    printf("\n--- test_fault_clears_pending ---\n");
    setup_env();

    file_t *f = NULL;
    CHECK_EQ(0, fb_test_ops.open("fbtest", &f));

    struct fb_test_req req = make_req(1, 0x99);
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_TERMINAL_ENOMEM, &req));
    CHECK_EQ(0, fb_test_ops.ioctl_file(f, FBIOTEST_ARM_MISMATCH, &req));

    fb_mark_failed();

    CHECK_EQ(0, fb_test_consume_terminal_enomem(1));
    /* The register fault was cleared too: a fresh SET transaction for the
       target PID sees no armed readback corruption. */
    fb_test_set_begin(1);
    CHECK_EQ(0, fb_test_filter_readback(FB_TEST_STEP_APPLY,
                                        VBE_DISPI_INDEX_X_OFFSET, 0));
    fb_test_set_end();
    /* fb_test_reset is idempotent and leaves nothing pending. */
    CHECK_EQ(0, fb_test_consume_terminal_enomem(1));

    fb_test_ops.release_file(f);
}

/* Run before setup_env/fb_test_init in the parent. Each child inherits
 * pristine module statics, so one entry cannot warm up another's lock. */
static void test_cold_file_callbacks_initialize_lock(void)
{
    const int commands[] = {FBIOTEST_ARM_MISMATCH, FBIOTEST_ARM_TERMINAL_ENOMEM, 0};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        int child = host_fork();
        CHECK_TRUE(child >= 0);
        if (child < 0) return;
        if (child == 0) {
            mock_current_task.pid = 31;
            g_observe_cold_lock = true;
            file_t *f = NULL;
            if (fb_test_ops.open("fbtest", &f) != 0 || !f) _exit(2);
            struct fb_test_req req = make_req(31, 7);
            int rc = 0, repeated = -EBUSY;
            if (commands[i]) {
                rc = fb_test_ops.ioctl_file(f, commands[i], &req);
                repeated = fb_test_ops.ioctl_file(f, commands[i], &req);
            }
            fb_test_ops.release_file(f);
            bool failed = rc != 0 || repeated != -EBUSY || f->dev_private ||
                          g_lock_inits != 1 || g_uninitialized_acquires != 0 ||
                          g_lock_acquires != (commands[i] ? 3u : 1u);
            free(f);
            /* Encode uninitialized acquisitions without using OS01's
             * FILE macros with the host libc in the forked child. */
            _exit(g_uninitialized_acquires ? 10 + g_uninitialized_acquires :
                  failed ? 1 : 0);
        }
        int status = 0;
        CHECK_EQ(child, host_waitpid(child, &status, 0));
        CHECK_TRUE(WIFEXITED(status));
        CHECK_EQ(0, WEXITSTATUS(status));
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_cold_file_callbacks_initialize_lock),
    TEST_ENTRY(test_hooks_abi),
    TEST_ENTRY(test_hooks_registration_and_validation),
    TEST_ENTRY(test_snapshot_regs_and_index_restore),
    TEST_ENTRY(test_snapshot_user_fault_no_leak),
    TEST_ENTRY(test_arm_mismatch_consumed_by_set),
    TEST_ENTRY(test_arm_other_pid_not_consumed),
    TEST_ENTRY(test_arm_rollback_failure),
    TEST_ENTRY(test_hold_writer_lease),
    TEST_ENTRY(test_terminal_enomem_target_pid_once),
    TEST_ENTRY(test_terminal_arm_rejects_zero_token_without_side_effects),
    TEST_ENTRY(test_terminal_arm_preserves_pending_and_allows_rearm),
    TEST_ENTRY(test_fault_clears_pending),
TEST_LIST_END

int main(void)
{
    printf("=== Test Runner ===\n");
    int n = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < n; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }

    /* The CHECK_TRUE/CHECK_EQ macros record failures in the bare g_failed
     * counter (matching sibling hosttests).  Fold those into the framework
     * counters so TEST_RESULTS() prints the real failure count and the
     * process exit status reflects a failing assertion — otherwise the
     * RES_CASE=hooks gate could never fail on an assertion regression. */
    __test_stats.total += g_failed;
    __test_stats.failed += g_failed;
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed > 0 ? 1 : 0;
}

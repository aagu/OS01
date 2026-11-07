// kernel/selftest/arch10_capacity_fixture.c — Isolated test fixture for ARCH-10 capacity.
// Compiled ONLY when CAPACITY_TEST=1.

#include <block/blockdev.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <arch/x86_64/irq_route.h>
#include <intr/interrupt.h>
#include <arch/irq.h>
#include <core/printk.h>
#include <log/log.h>
#include <string.h>

#define FAKE_BLK_COUNT  12
#define DEVFS_NODE_COUNT 40

static block_device_t *s_fake_bdevs[FAKE_BLK_COUNT];
static char s_fake_blk_names[FAKE_BLK_COUNT][16];
static char s_devfs_node_names[DEVFS_NODE_COUNT][24];

static volatile int s_poll_sleep_state = 0;

void arch10_capacity_poll_arm_hook(uint64_t nfds)
{
    if (nfds > 0) {
        s_poll_sleep_state++;
        serial_printk("[arch10-capacity] poll armed sleeping nfds=%lu\n", (unsigned long)nfds);
    }
}

void arch10_capacity_poll_wake_hook(int ready_count)
{
    if (ready_count > 0) {
        serial_printk("[arch10-capacity] poll woke ready=%d\n", ready_count);
    }
}

// ── 1. Fake Block Devices ───────────────────────────────────────────

static int fakeblk_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer)
{
    (void)lba;
    if (!dev || !buffer || count == 0) return -1;
    uintptr_t idx = (uintptr_t)dev->private_data;
    char payload[64];
    memset(payload, 0, sizeof(payload));
    // Format: FAKEBLK_PAYLOAD_XX
    payload[0] = 'F'; payload[1] = 'A'; payload[2] = 'K'; payload[3] = 'E';
    payload[4] = 'B'; payload[5] = 'L'; payload[6] = 'K'; payload[7] = '_';
    payload[8] = 'P'; payload[9] = 'A'; payload[10] = 'Y'; payload[11] = 'L';
    payload[12] = 'O'; payload[13] = 'A'; payload[14] = 'D'; payload[15] = '_';
    payload[16] = '0' + (char)(idx / 10);
    payload[17] = '0' + (char)(idx % 10);
    payload[18] = '\n';
    payload[19] = '\0';

    memset(buffer, 0, count * 512);
    size_t plen = strlen(payload);
    for (uint32_t s = 0; s < count; s++) {
        memcpy((char *)buffer + s * 512, payload, plen);
    }
    return 0;
}

static int fakeblk_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)dev; (void)lba; (void)count; (void)buffer;
    return 0;
}

static int fakeblk_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

static const struct block_device_ops s_fakeblk_ops = {
    .read = fakeblk_read,
    .write = fakeblk_write,
    .flush = fakeblk_flush,
};

// ── 2. Fake Devfs Character Nodes ───────────────────────────────────

static int arch10_node_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0) return 0;
    void *priv = devfs_get_private(node);
    uintptr_t idx = (uintptr_t)priv;

    char payload[64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 'A'; payload[1] = 'R'; payload[2] = 'C'; payload[3] = 'H';
    payload[4] = '1'; payload[5] = '0'; payload[6] = '_'; payload[7] = 'N';
    payload[8] = 'O'; payload[9] = 'D'; payload[10] = 'E'; payload[11] = '_';
    payload[12] = 'P'; payload[13] = 'A'; payload[14] = 'Y'; payload[15] = 'L';
    payload[16] = 'O'; payload[17] = 'A'; payload[18] = 'D'; payload[19] = '_';
    payload[20] = '0' + (char)(idx / 10);
    payload[21] = '0' + (char)(idx % 10);
    payload[22] = '\n';
    payload[23] = '\0';

    size_t plen = strlen(payload);
    if (offset >= plen) return 0;
    size_t to_copy = plen - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buffer, payload + offset, to_copy);
    return (int)to_copy;
}

static const struct devfs_ops s_arch10_node_ops = {
    .read = arch10_node_read,
};

// ── 3. Handshake Devfs Node ─────────────────────────────────────────

static int arch10_handshake_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node;
    if (!buffer || size == 0) return 0;
    char str[16];
    str[0] = '0' + (char)(s_poll_sleep_state % 10);
    str[1] = '\n';
    str[2] = '\0';
    size_t len = 2;
    if (offset >= len) return 0;
    size_t to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buffer, str + offset, to_copy);
    return (int)to_copy;
}

static const struct devfs_ops s_arch10_handshake_ops = {
    .read = arch10_handshake_read,
};

// ── 4. x86 Extended Vector Stub Verification ────────────────────────

static volatile uint64_t s_ext_handler_count = 0;
static volatile uint64_t s_ext_ack_count = 0;

static void s_ext_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter; (void)regs;
    s_ext_handler_count++;
}

static void s_ext_ack(uint64_t nr)
{
    (void)nr;
    s_ext_ack_count++;
}

static hw_int_controller_t s_test_controller = {
    .enable = NULL,
    .disable = NULL,
    .install = NULL,
    .uninstall = NULL,
    .ack = s_ext_ack,
};

static void test_x86_extended_vector(void)
{
    uint64_t vec = 0;
    int rc = x86_irq_route_prepare(40, &vec);
    if (rc != 0 || vec != 0x42) {
        log_err("[arch10-capacity] route prepare for GSI 40 failed: %d, vec=0x%lx\n", rc, (unsigned long)vec);
        return;
    }

    irq_desc_t *desc = irq_desc_for_gsi(40);
    if (!desc) {
        log_err("[arch10-capacity] irq_desc_for_gsi(40) is NULL\n");
        return;
    }
    desc->handler = s_ext_handler;
    desc->controller = &s_test_controller;
    desc->parameter = 0x1234;

    s_ext_handler_count = 0;
    s_ext_ack_count = 0;

    uint64_t rbx_before = 0x1122334455667788ULL, rbx_after = 0;
    uint64_t r12_before = 0x8877665544332211ULL, r12_after = 0;

    __asm__ volatile (
        "movq %2, %%rbx\n\t"
        "movq %3, %%r12\n\t"
        "int $0x42\n\t"
        "movq %%rbx, %0\n\t"
        "movq %%r12, %1\n\t"
        : "=r"(rbx_after), "=r"(r12_after)
        : "r"(rbx_before), "r"(r12_before)
        : "rbx", "r12", "memory", "cc"
    );

    if (s_ext_handler_count == 1 && s_ext_ack_count == 1 &&
        rbx_before == rbx_after && r12_before == r12_after) {
        serial_printk("[arch10-capacity] x86 extended vector 0x42 stub PASS\n");
        serial_printk("[arch10-capacity] software int does not prove electrical IOAPIC routing\n");
    } else {
        log_err("[arch10-capacity] x86 ext vector 0x42 test FAIL (h=%lu a=%lu)\n",
                (unsigned long)s_ext_handler_count, (unsigned long)s_ext_ack_count);
    }
}

// ── 5. Main Fixture Initialization ──────────────────────────────────

void arch10_capacity_fixture_init(void)
{
    // Register 12 fake block devices
    for (int i = 0; i < FAKE_BLK_COUNT; i++) {
        char *name = s_fake_blk_names[i];
        name[0] = 'f'; name[1] = 'a'; name[2] = 'k'; name[3] = 'e';
        name[4] = 'b'; name[5] = 'l'; name[6] = 'k';
        if (i < 10) {
            name[7] = '0' + (char)i;
            name[8] = '\0';
        } else {
            name[7] = '1';
            name[8] = '0' + (char)(i - 10);
            name[9] = '\0';
        }
        struct block_device_desc desc = {
            .name = name,
            .sector_count = 2048,
            .sector_size = 512,
            .ops = &s_fakeblk_ops,
            .private_data = (void *)(uintptr_t)i,
            .parent = NULL,
            .kind = BLOCK_DISK,
        };
        int bret = block_device_register(&desc, &s_fake_bdevs[i]);
        if (bret != 0) {
            log_err("[arch10-capacity] block_device_register '%s' failed: %d\n", name, bret);
        }
    }

    // Register 40 devfs character nodes
    for (int i = 0; i < DEVFS_NODE_COUNT; i++) {
        char *name = s_devfs_node_names[i];
        name[0] = 'a'; name[1] = 'r'; name[2] = 'c'; name[3] = 'h';
        name[4] = '1'; name[5] = '0'; name[6] = '-'; name[7] = 'n';
        name[8] = 'o'; name[9] = 'd'; name[10] = 'e'; name[11] = '-';
        if (i < 10) {
            name[12] = '0' + (char)i;
            name[13] = '\0';
        } else {
            name[12] = '0' + (char)(i / 10);
            name[13] = '0' + (char)(i % 10);
            name[14] = '\0';
        }
        int dret = devfs_register_chrdev(name, (void *)(uintptr_t)i, &s_arch10_node_ops);
        if (dret != 0) {
            log_err("[arch10-capacity] devfs_register_chrdev '%s' failed: %d\n", name, dret);
        }
    }

    // Register handshake node
    devfs_register_chrdev("arch10-handshake", NULL, &s_arch10_handshake_ops);

    serial_printk("[arch10-capacity] fixture: registered 12 block devices and 40 devfs nodes\n");

    // Exercise x86 extended vector stub
    test_x86_extended_vector();
}

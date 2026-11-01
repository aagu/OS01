#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <intr/interrupt.h>
#include <arch/irq.h>

/* Track host memory allocations to verify bounded descriptors and leak-free rollbacks */
static size_t g_kmalloc_live_bytes = 0;
static int g_kmalloc_fail_countdown = -1;

void *kmalloc(size_t size)
{
    if (g_kmalloc_fail_countdown == 0) {
        return NULL;
    }
    if (g_kmalloc_fail_countdown > 0) {
        g_kmalloc_fail_countdown--;
    }
    size_t *p = malloc(size + sizeof(size_t));
    if (!p) return NULL;
    *p = size;
    g_kmalloc_live_bytes += size;
    return (void *)(p + 1);
}

void kfree(void *ptr)
{
    if (!ptr) return;
    size_t *p = ((size_t *)ptr) - 1;
    g_kmalloc_live_bytes -= *p;
    free(p);
}

/* Tracking controller hooks */
static int g_ctrl_install_calls = 0;
static int g_ctrl_uninstall_calls = 0;
static int g_ctrl_enable_calls = 0;
static int g_ctrl_disable_calls = 0;
static uint64_t g_last_vector = 0;
static uint64_t g_mock_install_ret = 0;

static uint64_t mock_install(uint64_t irq, void *arg) {
    g_ctrl_install_calls++;
    g_last_vector = irq;
    return g_mock_install_ret;
}
static void mock_uninstall(uint64_t irq) {
    g_ctrl_uninstall_calls++;
    g_last_vector = irq;
}
static void mock_enable(uint64_t irq) {
    g_ctrl_enable_calls++;
    g_last_vector = irq;
}
static void mock_disable(uint64_t irq) {
    g_ctrl_disable_calls++;
    g_last_vector = irq;
}
static void mock_ack(uint64_t irq) {
    g_last_vector = irq;
}

static hw_int_controller_t s_mock_controller = {
    .install = mock_install,
    .uninstall = mock_uninstall,
    .enable = mock_enable,
    .disable = mock_disable,
    .ack = mock_ack,
};

static bool g_select_controller_avail = true;
hw_int_controller_t *arch_irq_select_controller(uint32_t gsi)
{
    (void)gsi;
    if (!g_select_controller_avail) return NULL;
    return &s_mock_controller;
}

static int g_route_prep_fail = 0;
int arch_irq_route_prepare(uint32_t gsi, uint64_t *vector)
{
    if (g_route_prep_fail)
        return -EINVAL;
    if (vector)
        *vector = 0x20 + gsi;
    return 0;
}

uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    return 0x20 + gsi;
}

uint32_t arch_irq_vector_to_gsi(uint64_t vector)
{
    return (uint32_t)(vector - 0x20);
}

void arch_irq_install(void) {}
void softirq_init(void) {}

static void dummy_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr;
    (void)parameter;
    (void)regs;
}

static void reset_test_state(void)
{
    g_kmalloc_fail_countdown = -1;
    g_ctrl_install_calls = 0;
    g_ctrl_uninstall_calls = 0;
    g_ctrl_enable_calls = 0;
    g_ctrl_disable_calls = 0;
    g_last_vector = 0;
    g_mock_install_ret = 0;
    g_select_controller_avail = true;
    g_route_prep_fail = 0;
}

/* 1. Sparse ranges are bounded in memory and queryable */
TEST_FUNC(sparse_ranges_are_bounded)
{
    reset_test_state();

    struct irq_gsi_range ranges[] = {
        { .base = 0, .count = 24 },
        { .base = 32, .count = 24 },
        { .base = 0xffff0000, .count = 4 }
    };

    size_t live_before = g_kmalloc_live_bytes;
    int rc = irq_registry_init_ranges(ranges, 3);
    assert_eq(0, rc);

    /* 40 is in [32, 56) -> must be valid */
    irq_desc_t *d40 = irq_desc_for_gsi(40);
    assert_not_null(d40);

    /* 0xffff0001 is in [0xffff0000, 0xffff0004) -> must be valid */
    irq_desc_t *d_high = irq_desc_for_gsi(0xffff0001);
    assert_not_null(d_high);

    /* 24 is NOT in [0, 24) nor [32, 56) -> NULL */
    assert_null(irq_desc_for_gsi(24));

    /* 31 is NOT in any range -> NULL */
    assert_null(irq_desc_for_gsi(31));

    /* Total desc count across ranges = 24 + 24 + 4 = 52.
     * Check that descriptor allocation bytes is bounded to ~52 descriptors,
     * not sparse gigabytes. */
    size_t allocated_delta = g_kmalloc_live_bytes - live_before;
    size_t min_expected = 52 * sizeof(irq_desc_t);
    assert_true(allocated_delta >= min_expected);
    /* Definitely nowhere near megabytes/gigabytes */
    assert_true(allocated_delta < min_expected + 4096);
}

/* 2. Range validation rejections: overlap, endpoint overflow, count=0 */
TEST_FUNC(range_validation_rejections)
{
    reset_test_state();

    /* Establish a baseline valid range [0, 16) */
    struct irq_gsi_range base_range = { .base = 0, .count = 16 };
    assert_eq(0, irq_registry_init_ranges(&base_range, 1));
    assert_not_null(irq_desc_for_gsi(10));
    assert_null(irq_desc_for_gsi(20));

    /* Overlapping ranges: {0, 24} and {16, 24} overlap at 16..23 */
    struct irq_gsi_range overlap[] = {
        { .base = 0, .count = 24 },
        { .base = 16, .count = 24 }
    };
    assert_eq(-EINVAL, irq_registry_init_ranges(overlap, 2));
    /* Old registry [0, 16) must be preserved */
    assert_not_null(irq_desc_for_gsi(10));
    assert_null(irq_desc_for_gsi(20));

    /* Endpoint overflow: UINT32_MAX + 1 */
    struct irq_gsi_range overflow[] = {
        { .base = 0xfffffffe, .count = 4 } /* 0xfffffffe + 4 exceeds UINT32_MAX + 1 */
    };
    assert_eq(-EINVAL, irq_registry_init_ranges(overflow, 1));
    assert_not_null(irq_desc_for_gsi(10));

    /* Range count == 0 */
    assert_eq(-EINVAL, irq_registry_init_ranges(overflow, 0));
    assert_eq(-EINVAL, irq_registry_init_ranges(NULL, 1));

    /* Individual count == 0 */
    struct irq_gsi_range zero_count[] = {
        { .base = 100, .count = 0 }
    };
    assert_eq(-EINVAL, irq_registry_init_ranges(zero_count, 1));
    assert_not_null(irq_desc_for_gsi(10));
}

/* 3. Kmalloc failure during init: rollback without leak, old registry preserved */
TEST_FUNC(kmalloc_failure_preserves_old_registry)
{
    reset_test_state();

    /* Establish baseline valid range [0, 16) */
    struct irq_gsi_range base_range = { .base = 0, .count = 16 };
    assert_eq(0, irq_registry_init_ranges(&base_range, 1));

    size_t live_before = g_kmalloc_live_bytes;
    int enable_before = g_ctrl_enable_calls;

    /* New multi-range allocation attempt */
    struct irq_gsi_range new_ranges[] = {
        { .base = 100, .count = 10 },
        { .base = 200, .count = 10 }
    };

    /* Fail on 1st kmalloc (the bucket array) */
    g_kmalloc_fail_countdown = 0;
    int rc = irq_registry_init_ranges(new_ranges, 2);
    assert_eq(-ENOMEM, rc);
    assert_eq(live_before, g_kmalloc_live_bytes);
    assert_eq(enable_before, g_ctrl_enable_calls);
    /* Old registry intact */
    assert_not_null(irq_desc_for_gsi(5));
    assert_null(irq_desc_for_gsi(105));

    /* Fail on 2nd kmalloc (first range descriptor array) */
    g_kmalloc_fail_countdown = 1;
    rc = irq_registry_init_ranges(new_ranges, 2);
    assert_eq(-ENOMEM, rc);
    assert_eq(live_before, g_kmalloc_live_bytes);
    assert_eq(enable_before, g_ctrl_enable_calls);
    assert_not_null(irq_desc_for_gsi(5));
    assert_null(irq_desc_for_gsi(105));

    /* Fail on 3rd kmalloc (second range descriptor array) */
    g_kmalloc_fail_countdown = 2;
    rc = irq_registry_init_ranges(new_ranges, 2);
    assert_eq(-ENOMEM, rc);
    assert_eq(live_before, g_kmalloc_live_bytes);
    assert_eq(enable_before, g_ctrl_enable_calls);
    assert_not_null(irq_desc_for_gsi(5));
    assert_null(irq_desc_for_gsi(205));

    g_kmalloc_fail_countdown = -1;
}

/* 4. PIC static early storage */
TEST_FUNC(pic_static_early_storage)
{
    reset_test_state();

    /* Free any dynamic ranges by unregistering / resetting to NULL */
    /* irq_registry_init_pic uses static 16 entries */
    irq_registry_init_pic();

    for (uint32_t i = 0; i < 16; i++) {
        irq_desc_t *desc = irq_desc_for_gsi(i);
        assert_not_null(desc);
    }

    /* GSI >= 16 must return NULL in PIC mode */
    assert_null(irq_desc_for_gsi(16));
    assert_null(irq_desc_for_gsi(24));
    assert_null(irq_desc_for_gsi(100));
}

/* 5. Active handler refuses registry reset */
TEST_FUNC(active_handler_refuses_reset)
{
    reset_test_state();

    struct irq_gsi_range base_range = { .base = 0, .count = 16 };
    assert_eq(0, irq_registry_init_ranges(&base_range, 1));

    /* Register an active handler on GSI 5 */
    int reg_ok = register_irq(5, NULL, dummy_handler, 0x1234, 0, "test5");
    assert_eq(1, reg_ok);
    assert_eq(1, g_ctrl_install_calls);
    assert_eq(1, g_ctrl_enable_calls);

    /* Try to reset ranges while GSI 5 has an active handler -> must be refused (-EBUSY) */
    struct irq_gsi_range new_range = { .base = 0, .count = 32 };
    assert_eq(-EBUSY, irq_registry_init_ranges(&new_range, 1));

    /* Old handler still active */
    irq_desc_t *desc = irq_desc_for_gsi(5);
    assert_not_null(desc);
    assert_eq((uintptr_t)dummy_handler, (uintptr_t)desc->handler);

    /* Unregister GSI 5 */
    uint32_t unreg_ok = unregister_irq(5);
    assert_eq(1, unreg_ok);
    assert_eq(1, g_ctrl_disable_calls);
    assert_eq(1, g_ctrl_uninstall_calls);
    assert_null(desc->handler);

    /* Now reset should succeed */
    assert_eq(0, irq_registry_init_ranges(&new_range, 1));
    assert_not_null(irq_desc_for_gsi(30));
}

/* 6. Register / unregister / mask / unmask validation */
TEST_FUNC(registration_validation_and_lifecycle)
{
    reset_test_state();

    struct irq_gsi_range base_range = { .base = 0, .count = 16 };
    assert_eq(0, irq_registry_init_ranges(&base_range, 1));

    /* Invalid handler */
    assert_eq(0, register_irq(2, NULL, NULL, 0, 0, "bad_handler"));

    /* Invalid name */
    assert_eq(0, register_irq(2, NULL, dummy_handler, 0, 0, NULL));
    assert_eq(0, register_irq(2, NULL, dummy_handler, 0, 0, ""));
    assert_eq(0, register_irq(2, NULL, dummy_handler, 0, 0, "name_too_long_exceeding_31_chars_strictly_forbidden"));

    /* Out of range GSI */
    assert_eq(0, register_irq(20, NULL, dummy_handler, 0, 0, "out_of_range"));

    /* arch_irq_route_prepare failure */
    g_route_prep_fail = 1;
    assert_eq(0, register_irq(2, NULL, dummy_handler, 0, 0, "prep_fail"));
    g_route_prep_fail = 0;

    /* Successful registration */
    assert_eq(1, register_irq(2, NULL, dummy_handler, 0x5678, IRQF_TRIGGER_EDGE, "valid_irq"));
    assert_eq(1, g_ctrl_install_calls);
    assert_eq(1, g_ctrl_enable_calls);
    assert_eq(0x22, g_last_vector);

    /* Double registration must fail */
    assert_eq(0, register_irq(2, NULL, dummy_handler, 0, 0, "duplicate"));

    /* Mask and unmask */
    int dis_before = g_ctrl_disable_calls;
    int en_before = g_ctrl_enable_calls;
    irq_mask(2);
    assert_eq(dis_before + 1, g_ctrl_disable_calls);
    irq_unmask(2);
    assert_eq(en_before + 1, g_ctrl_enable_calls);

    /* Unregister */
    assert_eq(1, unregister_irq(2));
    assert_eq(dis_before + 2, g_ctrl_disable_calls);
    assert_eq(1, g_ctrl_uninstall_calls);

    /* Unregistering already unassigned GSI returns 0 */
    assert_eq(0, unregister_irq(2));
}

/* 7. Controller install failure rolls back descriptor reservation */
TEST_FUNC(controller_install_failure_rolls_back)
{
    reset_test_state();

    struct irq_gsi_range base_range = { .base = 0, .count = 16 };
    assert_eq(0, irq_registry_init_ranges(&base_range, 1));

    /* Simulate controller install failure */
    g_mock_install_ret = 1;

    int rc = register_irq(5, NULL, dummy_handler, 0x1234, IRQF_TRIGGER_EDGE, "fail_irq");
    assert_eq(0, rc);
    assert_eq(1, g_ctrl_install_calls);
    assert_eq(0, g_ctrl_enable_calls);

    /* Verify descriptor reservation has been rolled back cleanly */
    irq_desc_t *p = irq_desc_for_gsi(5);
    assert_not_null(p);
    assert_null(p->handler);
    assert_null(p->controller);
    assert_eq('\0', p->irq_name[0]);
    assert_eq(0, p->parameter);
    assert_eq(0, p->flags);

    /* Subsequent registration on same GSI succeeds when install returns 0 */
    g_mock_install_ret = 0;
    rc = register_irq(5, NULL, dummy_handler, 0x5678, IRQF_TRIGGER_LEVEL, "succeed_irq");
    assert_eq(1, rc);
    assert_eq(2, g_ctrl_install_calls);
    assert_eq(1, g_ctrl_enable_calls);
    assert_not_null(p->handler);
    assert_not_null(p->controller);
    assert_eq(0, strcmp(p->irq_name, "succeed_irq"));

    /* Clean up */
    assert_eq(1, unregister_irq(5));
}

TEST_LIST_BEGIN
    TEST_ENTRY(sparse_ranges_are_bounded),
    TEST_ENTRY(range_validation_rejections),
    TEST_ENTRY(kmalloc_failure_preserves_old_registry),
    TEST_ENTRY(pic_static_early_storage),
    TEST_ENTRY(active_handler_refuses_reset),
    TEST_ENTRY(registration_validation_and_lifecycle),
    TEST_ENTRY(controller_install_failure_rolls_back),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return 0;
}

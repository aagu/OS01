#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <intr/interrupt.h>
#include <intr/apic.h>
#include <arch/irq.h>
#include <arch/x86_64/irq_route.h>
#include <subsys/subsys.h>
#include <arch/x86_64/gate.h>

uint64_t *kernel_map = NULL;
void vmm_map_page(uint64_t *pgdir, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags)
{
    (void)pgdir; (void)physical_address; (void)virtual_address; (void)flags;
}
void lapic_configure_nmi(void) {}
void x86_irq_entry(pt_regs_t *regs, uint64_t hwirq) { (void)regs; (void)hwirq; }
void x86_intr_restore_only(void) {}
struct gate_struct IDT_Table[256];

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
static int g_pin_accessed[256];

static void track_pin(uint64_t vector)
{
    uint32_t gsi = UINT32_MAX;
    if (x86_irq_route_lookup(vector, &gsi)) {
        if (gsi < 256) g_pin_accessed[gsi]++;
    } else if (vector >= 0x20 && vector < 0x20 + 256) {
        g_pin_accessed[vector - 0x20]++;
    }
}

static uint64_t mock_install(uint64_t irq, void *arg) {
    g_ctrl_install_calls++;
    g_last_vector = irq;
    track_pin(irq);
    return g_mock_install_ret;
}
static void mock_uninstall(uint64_t irq) {
    g_ctrl_uninstall_calls++;
    g_last_vector = irq;
    track_pin(irq);
}
static void mock_enable(uint64_t irq) {
    g_ctrl_enable_calls++;
    g_last_vector = irq;
    track_pin(irq);
}
static void mock_disable(uint64_t irq) {
    g_ctrl_disable_calls++;
    g_last_vector = irq;
    track_pin(irq);
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
    int rc = x86_irq_route_prepare(gsi, vector);
    if (rc != -ENOSYS)
        return rc;
    if (vector)
        *vector = 0x20 + gsi;
    return 0;
}

uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    const struct x86_irq_route *r = x86_irq_route_find_by_gsi(gsi);
    if (r && r->allocated)
        return r->vector;
    return 0x20 + gsi;
}

uint32_t arch_irq_vector_to_gsi(uint64_t vector)
{
    uint32_t gsi = UINT32_MAX;
    if (x86_irq_route_lookup(vector, &gsi))
        return gsi;
    return (uint32_t)(vector - 0x20);
}

static uint8_t s_mock_iso_source = 0;
static uint32_t s_mock_iso_gsi = 0;
static bool s_mock_iso_active = false;

uint32_t arch_irq_isa_to_gsi(uint8_t source)
{
    if (s_mock_iso_active && source == s_mock_iso_source)
        return s_mock_iso_gsi;
    return (uint32_t)source;
}

void arch_irq_install(void) {}
void softirq_init(void) {}

apic_info_t apic_info;
uint64_t arch_boot_rsdp = 0;
uint8_t irq_trigger_for_gsi(uint32_t gsi) { (void)gsi; return ISO_TRIGGER_EDGE; }

int g_log_level = 0;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

int acpi_parse(uint64_t rsdp) { (void)rsdp; return 1; }
int ioapic_init(void) { return 1; }
void arch_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }

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
    s_mock_iso_active = false;
    memset(g_pin_accessed, 0, sizeof(g_pin_accessed));
    x86_irq_routes_reset();
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

/* 8. x86 GSI/vector routing, bounds and exhaustion */
TEST_FUNC(x86_irq_routes_allocation_and_vector_bounds)
{
    reset_test_state();

    struct irq_gsi_range ranges[] = {
        { .base = 0, .count = 24 },
        { .base = 32, .count = 24 }
    };

    int rc = irq_registry_init_ranges(ranges, 2);
    assert_eq(0, rc);

    rc = x86_irq_routes_init(ranges, 2);
    assert_eq(0, rc);

    /* GSI 40 is in [32, 56) -> gets extended pool vector starting at 0x42 */
    uint64_t vec40 = 0;
    assert_eq(0, x86_irq_route_prepare(40, &vec40));
    assert_eq(0x42, vec40);

    /* GSI 24 is NOT in [0, 24) nor [32, 56) -> rejected */
    uint64_t vec24 = 0;
    assert_true(x86_irq_route_prepare(24, &vec24) != 0);

    /* GSI 16 is in low range [0, 24) -> retains historical vector 0x30 (0x20 + 16) */
    uint64_t vec16 = 0;
    assert_eq(0, x86_irq_route_prepare(16, &vec16));
    assert_eq(0x30, vec16);

    /* Route lookup */
    uint32_t gsi = 0;
    assert_true(x86_irq_route_lookup(0x42, &gsi));
    assert_eq(40, gsi);

    assert_true(x86_irq_route_lookup(0x30, &gsi));
    assert_eq(16, gsi);

    /* Unmapped vector */
    assert_false(x86_irq_route_lookup(0x24, &gsi));

    /* Isolated/reserved vectors: 0x38 (timer), 0x39 (fixed MSI-X), 0x40/0x41 (IPI), 0x80 (syscall), 0xff (spurious) */
    assert_false(x86_irq_route_lookup(0x38, &gsi));
    assert_false(x86_irq_route_lookup(0x39, &gsi));
    assert_false(x86_irq_route_lookup(0x40, &gsi));
    assert_false(x86_irq_route_lookup(0x41, &gsi));
    assert_false(x86_irq_route_lookup(0x80, &gsi));
    assert_false(x86_irq_route_lookup(0xff, &gsi));

    /* Fixture covering 24: range [0, 32) */
    struct irq_gsi_range cov24_range = { .base = 0, .count = 32 };
    assert_eq(0, irq_registry_init_ranges(&cov24_range, 1));
    assert_eq(0, x86_irq_routes_init(&cov24_range, 1));
    /* 24 is >= 24, so it enters the extended pool at 0x42 */
    vec24 = 0;
    assert_eq(0, x86_irq_route_prepare(24, &vec24));
    assert_eq(0x42, vec24);

    /* Extended pool exhaustion: exactly 62 extended vectors (0x42..0x7f) */
    struct irq_gsi_range exhaust_range = { .base = 24, .count = 63 };
    assert_eq(0, irq_registry_init_ranges(&exhaust_range, 1));
    assert_eq(0, x86_irq_routes_init(&exhaust_range, 1));

    for (uint32_t g = 24; g < 24 + 62; g++) {
        uint64_t v = 0;
        assert_eq(0, x86_irq_route_prepare(g, &v));
        assert_eq((uint64_t)(0x42 + (g - 24)), v);
    }

    /* 63rd extended allocation must fail and mapping remains unchanged */
    uint64_t v63 = 0;
    assert_true(x86_irq_route_prepare(24 + 62, &v63) != 0);

    /* Verify existing mappings are intact */
    for (uint32_t g = 24; g < 24 + 62; g++) {
        gsi = 0;
        assert_true(x86_irq_route_lookup(0x42 + (g - 24), &gsi));
        assert_eq(g, gsi);
    }
}

/* 9. ISO single conversion and pin tracking */
TEST_FUNC(iso_single_conversion_and_pin_tracking)
{
    reset_test_state();

    /* Range covering GSI 0..23 */
    struct irq_gsi_range range = { .base = 0, .count = 24 };
    assert_eq(0, irq_registry_init_ranges(&range, 1));
    assert_eq(0, x86_irq_routes_init(&range, 1));

    /* Simulate MADT ISO: source 0 -> GSI 2 */
    s_mock_iso_source = 0;
    s_mock_iso_gsi = 2;
    s_mock_iso_active = true;

    /* ISA driver calls arch_irq_isa_to_gsi ONCE */
    uint32_t actual_gsi = arch_irq_isa_to_gsi(0);
    assert_eq(2, actual_gsi);

    /* Register using actual GSI 2 */
    assert_eq(1, register_irq(actual_gsi, NULL, dummy_handler, 0x1234, IRQF_TRIGGER_EDGE, "iso_pit"));
    assert_eq(1, g_ctrl_install_calls);
    assert_eq(1, g_ctrl_enable_calls);

    /* Mask and unmask using actual GSI 2 */
    irq_mask(actual_gsi);
    assert_eq(1, g_ctrl_disable_calls);

    irq_unmask(actual_gsi);
    assert_eq(2, g_ctrl_enable_calls);

    /* Unregister using actual GSI 2 */
    assert_eq(1, unregister_irq(actual_gsi));
    assert_eq(2, g_ctrl_disable_calls);
    assert_eq(1, g_ctrl_uninstall_calls);

    /* Verify pin operations: pin 2 was accessed multiple times, pin 0 was NEVER accessed */
    assert_true(g_pin_accessed[2] > 0);
    assert_eq(0, g_pin_accessed[0]);
}

/* 10. APIC failure stops dependent subsys phases */
static jmp_buf s_panic_jb;
static bool s_panic_expected = false;
static int s_panic_calls = 0;

void kpanic(const char *fmt, ...)
{
    (void)fmt;
    s_panic_calls++;
    if (s_panic_expected) {
        longjmp(s_panic_jb, 1);
    }
    abort();
}

static int s_phase4_calls = 0;
static int s_phase5_calls = 0;
static int s_phase6_calls = 0;

static int mock_phase4_init(void) { s_phase4_calls++; return 0; }
static int mock_phase5_init(void) { s_phase5_calls++; return 0; }
static int mock_phase6_init(void) { s_phase6_calls++; return 0; }

/* Declarations for real _apic_init_wrapper and mockable test controls */
extern int _apic_init_wrapper(void);
extern void subsys_test_reset(void);
extern int g_test_inject_apic_fail;

TEST_FUNC(apic_failure_stops_dependent_phases)
{
    reset_test_state();

    /* Test 1: Inject IOAPIC OOM (value 1) -> panic, phases 4/5/6 never run */
    subsys_test_reset();
    s_phase4_calls = s_phase5_calls = s_phase6_calls = s_panic_calls = 0;
    register_subsys("apic", _apic_init_wrapper, SUBSYS_PHASE_3, 0);
    register_subsys("p4", mock_phase4_init, SUBSYS_PHASE_4, 0);
    register_subsys("p5", mock_phase5_init, SUBSYS_PHASE_5, 0);
    register_subsys("p6", mock_phase6_init, SUBSYS_PHASE_6, 0);

    g_test_inject_apic_fail = -ENOMEM; /* OOM */
    s_panic_expected = true;
    if (setjmp(s_panic_jb) == 0) {
        subsys_init_all();
        assert_true(false); /* Should not be reached */
    }
    assert_eq(1, s_panic_calls);
    assert_eq(0, s_phase4_calls);
    assert_eq(0, s_phase5_calls);
    assert_eq(0, s_phase6_calls);

    /* Test 2: Inject Illegal topology (value -EINVAL) -> panic, phases 4/5/6 never run */
    subsys_test_reset();
    s_phase4_calls = s_phase5_calls = s_phase6_calls = s_panic_calls = 0;
    register_subsys("apic", _apic_init_wrapper, SUBSYS_PHASE_3, 0);
    register_subsys("p4", mock_phase4_init, SUBSYS_PHASE_4, 0);
    register_subsys("p5", mock_phase5_init, SUBSYS_PHASE_5, 0);
    register_subsys("p6", mock_phase6_init, SUBSYS_PHASE_6, 0);

    g_test_inject_apic_fail = -EINVAL; /* Illegal topology */
    if (setjmp(s_panic_jb) == 0) {
        subsys_init_all();
        assert_true(false);
    }
    assert_eq(1, s_panic_calls);
    assert_eq(0, s_phase4_calls);
    assert_eq(0, s_phase5_calls);
    assert_eq(0, s_phase6_calls);

    /* Test 3: Inject > 4 controllers (-E2BIG) -> panic, phases 4/5/6 never run */
    subsys_test_reset();
    s_phase4_calls = s_phase5_calls = s_phase6_calls = s_panic_calls = 0;
    register_subsys("apic", _apic_init_wrapper, SUBSYS_PHASE_3, 0);
    register_subsys("p4", mock_phase4_init, SUBSYS_PHASE_4, 0);
    register_subsys("p5", mock_phase5_init, SUBSYS_PHASE_5, 0);
    register_subsys("p6", mock_phase6_init, SUBSYS_PHASE_6, 0);

    g_test_inject_apic_fail = -E2BIG; /* > 4 controllers */
    if (setjmp(s_panic_jb) == 0) {
        subsys_init_all();
        assert_true(false);
    }
    assert_eq(1, s_panic_calls);
    assert_eq(0, s_phase4_calls);
    assert_eq(0, s_phase5_calls);
    assert_eq(0, s_phase6_calls);

    /* Test 4: Legitimate no-IOAPIC (returns 0) -> PIC boots, phases 4/5/6 DO execute */
    subsys_test_reset();
    s_phase4_calls = s_phase5_calls = s_phase6_calls = s_panic_calls = 0;
    register_subsys("apic", _apic_init_wrapper, SUBSYS_PHASE_3, 0);
    register_subsys("p4", mock_phase4_init, SUBSYS_PHASE_4, 0);
    register_subsys("p5", mock_phase5_init, SUBSYS_PHASE_5, 0);
    register_subsys("p6", mock_phase6_init, SUBSYS_PHASE_6, 0);

    g_test_inject_apic_fail = 0; /* Legitimate no-IOAPIC / PIC fallback */
    s_panic_expected = false;
    subsys_init_all();
    assert_eq(0, s_panic_calls);
    assert_eq(1, s_phase4_calls);
    assert_eq(1, s_phase5_calls);
    assert_eq(1, s_phase6_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(sparse_ranges_are_bounded),
    TEST_ENTRY(range_validation_rejections),
    TEST_ENTRY(kmalloc_failure_preserves_old_registry),
    TEST_ENTRY(pic_static_early_storage),
    TEST_ENTRY(active_handler_refuses_reset),
    TEST_ENTRY(registration_validation_and_lifecycle),
    TEST_ENTRY(controller_install_failure_rolls_back),
    TEST_ENTRY(x86_irq_routes_allocation_and_vector_bounds),
    TEST_ENTRY(iso_single_conversion_and_pin_tracking),
    TEST_ENTRY(apic_failure_stops_dependent_phases),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return 0;
}

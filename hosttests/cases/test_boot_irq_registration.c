/* hosttests/cases/test_boot_irq_registration.c — BOOT-2 Task 1.
 *
 * Compiles the REAL kernel/intr/irq.c and the REAL
 * kernel/arch/x86_64/intr/pic_8259a.c against the port-IMR mock
 * (mock/boot_irq_time/).  The pinned behaviours:
 *
 *   no_autoen_never_enables       IRQF_NO_AUTOEN: enable calls == 0, the
 *                                 source stays masked, no edge latches
 *                                 (even though IF==0 would latch one),
 *                                 handler registered, entry IF unchanged.
 *   legacy_registration_autoen    default flags keep the old auto-enable
 *                                 — and an enable latches pending even
 *                                 with IF==0 (the IRR property).
 *   pic_slave_requires_cascade    IRQ8 is deliverable only when the
 *                                 slave bit AND master IRQ2 are both
 *                                 unmasked; unmasking one slave opens
 *                                 the cascade.
 *   pic_disable_keeps_other_slave disabling one slave keeps IRQ2 open
 *                                 while another slave user remains;
 *                                 the last disable masks the cascade.
 *   install_failure_does_not_enable a failed controller install rolls
 *                                 the descriptor back and never enables.
 *
 * Delivery goes through the REAL registry dispatch — the test's
 * deliver() consults the mock IMR model (slave bit ∧ master IRQ2) and
 * only then calls arch_irq_dispatch; the handler is never invoked
 * around the cascade.
 */
#include "test_framework.h"
#include <arch/irq.h>
#include <arch/io.h>
#include <intr/interrupt.h>
#include <intr/vector_dispatch.h>
#include <arch/x86_64/irq_route.h>

#include <stdio.h>
#include <string.h>

void arch_this_cpu_set(unsigned id);

static int handler_calls;
static uint64_t seen_vector;
static void test_handler(uint64_t v, uint64_t p, pt_regs_t *r)
{
    (void)p; (void)r;
    handler_calls++;
    seen_vector = v;
}

static uint64_t failing_install(uint64_t irq, void *arg);
static unsigned long failing_enable_calls;
static void failing_enable(uint64_t irq) { (void)irq; failing_enable_calls++; }
static hw_int_controller_t test_failing = {
    .install = failing_install,
    .enable = failing_enable,
    .disable = NULL,
    .ack = NULL,
};
static int wrap_active;
static uint64_t failing_install(uint64_t irq, void *arg)
{
    (void)irq; (void)arg;
    return 1;   /* nonzero = failure */
}

hw_int_controller_t *__real_arch_irq_select_controller(uint32_t gsi);
hw_int_controller_t *__wrap_arch_irq_select_controller(uint32_t gsi)
{
    if (wrap_active)
        return &test_failing;
    return __real_arch_irq_select_controller(gsi);
}

#define MASTER_IMR 0x21
#define SLAVE_IMR  0xA1

static void reset_mock(void)
{
    handler_calls = 0;
    seen_vector = 0;
    wrap_active = 0;
    boot2_if_flag = 0;
    boot2_irq_enable_calls = 0;
    failing_enable_calls = 0;
    boot2_imr_set(MASTER_IMR, 0xff);
    boot2_imr_set(SLAVE_IMR, 0xff);
    /* pending latch is monotonic; assert via deltas in the cases */

    for (uint32_t i = 0; i < 24; i++) {
        irq_desc_t *d = irq_desc_for_gsi(i);
        if (d) memset(d, 0, sizeof(*d));
    }
    struct irq_gsi_range r = { .base = 0, .count = 24 };
    irq_registry_init_ranges(&r, 1);
    /* The x86 route table must exist for arch_irq_route_prepare. */
    x86_irq_routes_init(&r, 1);
    memset(intr_handler_table, 0, sizeof(void *) * 256);
    memset(intr_handler_param, 0, sizeof(void *) * 256);
    arch_this_cpu_set(0);
}

/* Delivery model: consult the mock IMRs (the cascade), then the real
 * dispatch.  Returns 1 if the hardware would have delivered. */
static int deliver(uint64_t vector)
{
    uint8_t master = boot2_imr_get(MASTER_IMR);
    uint8_t slave = boot2_imr_get(SLAVE_IMR);
    if (vector >= 0x28) {
        if (slave & (1 << (vector - 0x28)))
            return 0;
        if (master & 0x04)          /* master IRQ2 cascade gate */
            return 0;
    } else if (vector >= 0x20) {
        if (master & (1 << (vector - 0x20)))
            return 0;
    } else {
        return 0;
    }
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    arch_irq_dispatch(&regs, vector);
    return 1;
}

static void no_autoen_never_enables(void)
{
    TEST_SUITE("boot2 registration: IRQF_NO_AUTOEN never enables");
    reset_mock();
    unsigned long pending_before = boot2_pending_get();

    int rc = register_irq(8, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                          "boot2-rtc");
    assert_eq(1, rc);
    assert_eq(0, boot2_irq_enable_calls);        /* never enabled */
    assert_eq(0x01, boot2_imr_get(SLAVE_IMR) & 0x01);   /* still masked */
    assert_eq(0x04, boot2_imr_get(MASTER_IMR) & 0x04);  /* cascade too */
    irq_desc_t *d = irq_desc_for_gsi(8);
    assert_true(d != NULL && d->handler == test_handler);
    assert_eq(0, handler_calls);
    assert_eq(pending_before, boot2_pending_get());  /* no edge latched */
    assert_eq(0, boot2_if_flag);                     /* entry IF kept 0 */

    /* The owner unmask path works and opens the cascade. */
    irq_unmask(8);
    assert_eq(0x00, boot2_imr_get(SLAVE_IMR) & 0x01);
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x04);
    assert_eq(1, deliver(0x28));
    assert_eq(1, handler_calls);
    assert_eq(0x28, seen_vector);
}

static void legacy_registration_autoen(void)
{
    TEST_SUITE("boot2 registration: default flags keep auto-enable");
    reset_mock();
    unsigned long pending_before = boot2_pending_get();

    int rc = register_irq(4, NULL, test_handler, 0, IRQF_TRIGGER_NONE,
                          "boot2-legacy");
    assert_eq(1, rc);
    /* One controller enable (measured on the IMR — the mock IF counter
     * tracks sti, not controller enables). */
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x10);  /* bit4 unmasked */
    /* The enable latched a pending edge even though IF was 0. */
    assert_eq(pending_before + (1UL << 4), boot2_pending_get());
}

static void pic_slave_requires_cascade(void)
{
    TEST_SUITE("boot2 registration: PIC slave needs master IRQ2");
    reset_mock();
    assert_eq(1, register_irq(8, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                              "boot2-rtc"));
    assert_eq(1, register_irq(9, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                              "boot2-other"));

    /* Slave unmasked but cascade still closed: NOT deliverable. */
    irq_unmask(8);
    assert_eq(0x00, boot2_imr_get(SLAVE_IMR) & 0x01);
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x04);  /* cascade opened */
    assert_eq(1, deliver(0x28));
    assert_eq(1, handler_calls);

    /* Close only the master cascade: slave bit still open, no delivery. */
    boot2_imr_set(MASTER_IMR, boot2_imr_get(MASTER_IMR) | 0x04);
    assert_eq(0, deliver(0x28));
    assert_eq(1, handler_calls);
    /* A master vector is unaffected by the slave gate. */
    assert_eq(1, register_irq(1, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                              "boot2-kbd"));
    irq_unmask(1);
    assert_eq(1, deliver(0x21));
    assert_eq(2, handler_calls);
}

static void pic_disable_keeps_other_slave(void)
{
    TEST_SUITE("boot2 registration: cascade kept while other slave live");
    reset_mock();
    assert_eq(1, register_irq(8, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                              "boot2-rtc"));
    assert_eq(1, register_irq(9, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                              "boot2-other"));
    irq_unmask(8);
    irq_unmask(9);
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x04);

    /* Disable slave 9: slave bit1 masked, IRQ2 STAYS open (8 still live). */
    irq_mask(9);
    assert_eq(0x02, boot2_imr_get(SLAVE_IMR) & 0x02);
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x04);
    assert_eq(1, deliver(0x28));    /* 8 still deliverable */
    assert_eq(1, handler_calls);
    assert_eq(0, deliver(0x29));    /* 9 masked */

    /* Last slave disable: only now may IRQ2 be masked. */
    irq_mask(8);
    assert_eq(0x03, boot2_imr_get(SLAVE_IMR) & 0x03);
    assert_eq(0x04, boot2_imr_get(MASTER_IMR) & 0x04);
    assert_eq(0, deliver(0x28));
    assert_eq(1, handler_calls);
}

static void install_failure_does_not_enable(void)
{
    TEST_SUITE("boot2 registration: install failure rolls back, no enable");
    reset_mock();
    wrap_active = 1;    /* arch_irq_select_controller returns test_failing */
    unsigned long pending_before = boot2_pending_get();

    int rc = register_irq(3, NULL, test_handler, 0, IRQF_NO_AUTOEN,
                          "boot2-fail");
    assert_eq(0, rc);                          /* refused */
    assert_eq(0, boot2_irq_enable_calls);      /* never enabled */
    assert_eq(0, failing_enable_calls);        /* not even by the failing ctrl */
    assert_eq(pending_before, boot2_pending_get());
    irq_desc_t *d = irq_desc_for_gsi(3);
    assert_true(d != NULL);
    assert_true(d->handler == NULL);           /* rolled back */
    assert_true(d->controller == NULL);

    /* Other owners unaffected: GSI 4 still registrable and auto-enabled. */
    wrap_active = 0;
    assert_eq(1, register_irq(4, NULL, test_handler, 0, IRQF_TRIGGER_NONE,
                              "boot2-ok"));
    assert_eq(0x00, boot2_imr_get(MASTER_IMR) & 0x10);  /* auto-enabled */
}

int main(void)
{
    no_autoen_never_enables();
    legacy_registration_autoen();
    pic_slave_requires_cascade();
    pic_disable_keeps_other_slave();
    install_failure_does_not_enable();
    printf("\ntest_boot_irq_registration: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

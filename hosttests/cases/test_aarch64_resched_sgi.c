/* hosttests/cases/test_aarch64_resched_sgi.c — P2 Task 1: the resched
 * SGI4 transport, driven against the REAL ipi.c + gic_driver.c with
 * mock MMIO (same pattern as test_aarch64_ipi_broadcast.c).
 *
 * Coverage:
 *   1. raw IAR injection ((source_cpu<<10)|4): gic_dev_dispatch writes
 *      the COMPLETE original IAR to EOIR exactly once (source bits
 *      preserved, no double EOI).
 *   2. the PRODUCTION aarch64_resched_sgi_handler publishes this
 *      CPU's need_resched 0→1 with a release store; it never calls
 *      schedule() and never takes a lock; it never touches EOI itself.
 *   3. SGI 3 still dispatches the TLB handler (registered spy) — the
 *      new mapping must not regress the existing one.
 *   4. repeated SGI4 while the flag is already 1: the handler is
 *      idempotent (no queue, no counting) — merged/duplicated SGIs
 *      never map to "one interrupt per task".
 *   5. the handler is boot-stack safe: no current access (the host
 *      build of ipi.c has no percpu dependency at all).
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <intr/ipi.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/gic.h>

#define M_GICD_SGIR 0xF00
#define M_GICC_IAR  0x00C
#define M_GICC_EOIR 0x010

static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;

static unsigned eoi_write_count;
static uint32_t eoi_write_values[8];
void gic_host_mmio_write(volatile uint32_t *address, uint32_t value)
{
    *address=value;
    if (address == &gicc_mock[M_GICC_EOIR/4]) {
        if (eoi_write_count < 8) eoi_write_values[eoi_write_count]=value;
        eoi_write_count++;
    }
}

struct gic_dev *gic_dev_current(void) { return &dev; }

#include <arch/aarch64/dtb.h>
uint32_t dtb_cpu_count(void) { return 4; }
uint64_t dtb_gicd_base(void) { return 0; }

static jmp_buf panic_jb;
static int panic_armed;
void ipi_panic_unsupported_vector(uint32_t vector)
{
    (void)vector;
    if (panic_armed) {
        panic_armed = 0;
        longjmp(panic_jb, 1);
    }
    fprintf(stderr, "ipi fatal hook outside armed test\n");
    exit(2);
}

/* The per-CPU need_resched slot seam (P2): the host build of ipi.c
 * cannot run TPIDR_EL1, so the handler resolves its target through
 * this overridable accessor; the target build inlines the arch read. */
static uint64_t mock_need_slot;
uint64_t *aarch64_resched_need_slot(void) { return &mock_need_slot; }

/* schedule spy: the handler must NEVER call it. */
int mock_schedule_calls;
void schedule(void) { mock_schedule_calls++; }

/* Lock spies: the handler must never take a lock. */
int mock_lock_calls;
void mock_record_lock_op(void *l, int is_lock) { (void)l; (void)is_lock; mock_lock_calls++; }

/* TLB handler spy (registered on intid 3). */
int tlb_handler_calls;
static void tlb_spy(uint32_t intid, uint64_t param, struct pt_regs *regs)
{ (void)intid; (void)param; (void)regs; tlb_handler_calls++; }

/* CNTP PPI handler spy (registered on intid 30, the PPI the QEMU virt
 * DTB reports for the physical timer). */
int ppi_handler_calls;
static void ppi_spy(uint32_t intid, uint64_t param, struct pt_regs *regs)
{ (void)intid; (void)param; (void)regs; ppi_handler_calls++; }

#define M_CNTP_PPI 30u

static uint32_t rd(const uint32_t *m, uint32_t off) { return m[off / 4]; }

static void mock_reset(void)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[0x004 / 4] = 0x1f;
    gicd_mock[0x008 / 4] = 0x0200143b;
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    /* The handler table is static and survives gic_dev_init: drop the
     * previous suite's registrations before re-registering. */
    gic_unregister_handler(3);
    gic_unregister_handler(4);
    gic_unregister_handler(M_CNTP_PPI);
    assert_eq(0, gic_register_handler(3, tlb_spy, 0, "tlb-spy"));
    eoi_write_count=0;
    mock_need_slot = 0;
    mock_schedule_calls = 0;
    mock_lock_calls = 0;
    tlb_handler_calls = 0;
    ppi_handler_calls = 0;
}

static void suite_raw_iar_eoi(void)
{
    TEST_SUITE("raw IAR (src<<10)|4 dispatches and EOIs the exact IAR once");
    mock_reset();
    /* handler registered by production gic_init; this harness
     * registers the PRODUCTION handler on intid 4 directly. */
    assert_eq(0, gic_register_handler(4, aarch64_resched_sgi_handler, 0,
                                      "resched"));
    /* Inject: CPU1 sent SGI 4 to us → IAR = (1<<10)|4. */
    gicc_mock[M_GICC_IAR / 4] = (1u << 10) | 4u;
    struct pt_regs *regs = NULL;
    gic_dev_dispatch(&dev, regs);
    assert_eq((uint32_t)((1u << 10) | 4u), rd(gicc_mock, M_GICC_EOIR));
    assert_eq(1,eoi_write_count);
    assert_eq((1u<<10)|4u,eoi_write_values[0]);
    assert_eq(1, (int)mock_need_slot);
    assert_eq(0, mock_schedule_calls);
    assert_eq(0, mock_lock_calls);
}

static void suite_tlb_not_regressed(void)
{
    TEST_SUITE("SGI 3 still dispatches the TLB handler");
    mock_reset();
    gicc_mock[M_GICC_IAR / 4] = (2u << 10) | 3u;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1,eoi_write_count);
    assert_eq((2u<<10)|3u,eoi_write_values[0]);
    assert_eq(1, tlb_handler_calls);
    assert_eq((uint32_t)((2u << 10) | 3u), rd(gicc_mock, M_GICC_EOIR));
    assert_eq(0, (int)mock_need_slot);   /* TLB SGI does not touch it */
}

static void suite_duplicate_sgi_merges(void)
{
    TEST_SUITE("repeated SGI4 stays idempotent (no per-interrupt work)");
    mock_reset();
    assert_eq(0, gic_register_handler(4, aarch64_resched_sgi_handler, 0,
                                      "resched"));
    gicc_mock[M_GICC_IAR / 4] = 4u;
    gic_dev_dispatch(&dev, NULL);
    uint32_t eoi_after_first = rd(gicc_mock, M_GICC_EOIR);
    gicc_mock[M_GICC_IAR / 4] = 4u;
    gic_dev_dispatch(&dev, NULL);
    /* Still exactly need=1 — merged SGIs never enqueue extra work. */
    assert_eq(1, (int)mock_need_slot);
    assert_eq(0, mock_schedule_calls);
    assert_eq(2,eoi_write_count);
    assert_eq(4,eoi_write_values[0]);
    assert_eq(4,eoi_write_values[1]);
    (void)eoi_after_first;
}

static void suite_spurious_no_eoi(void)
{
    TEST_SUITE("spurious IAR is never EOIed");
    mock_reset();
    gicc_mock[M_GICC_IAR / 4] = 0x3ffu;   /* GIC_INTID_SPURIOUS */
    gic_dev_dispatch(&dev, NULL);
    assert_eq(0,eoi_write_count);
    assert_eq((uint32_t)0, rd(gicc_mock, M_GICC_EOIR));
}

/* The CNTP PPI is delivered through the SAME production GIC dispatch
 * path as an SGI, but it is a private peripheral interrupt: the
 * complete IAR must reach EOIR exactly once and a spurious IAR must
 * never be EOIed at all (a duplicated EOI would acknowledge a
 * non-delivered interrupt and lose the next tick). */
static void suite_ppi_eoi_once(void)
{
    TEST_SUITE("CNTP PPI is EOIed once with the complete IAR");
    mock_reset();
    assert_eq(0, gic_register_handler(M_CNTP_PPI, ppi_spy, 0, "cntp-ppi"));
    /* CPU2 delivered the PPI -> IAR = (2 << 10) | 30. */
    gicc_mock[M_GICC_IAR / 4] = (2u << 10) | M_CNTP_PPI;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, eoi_write_count);
    assert_eq((2u << 10) | M_CNTP_PPI, eoi_write_values[0]);
    assert_eq((uint32_t)((2u << 10) | M_CNTP_PPI), rd(gicc_mock, M_GICC_EOIR));
    assert_eq(1, ppi_handler_calls);
    /* The PPI is not a resched SGI: it never touches the need_resched
     * slot and it never calls schedule() from the handler. */
    assert_eq(0, (int)mock_need_slot);
    assert_eq(0, mock_schedule_calls);
}

static void suite_ppi_spurious_not_eoied(void)
{
    TEST_SUITE("a spurious IAR is never EOIed (no PPI double-EOI)");
    mock_reset();
    assert_eq(0, gic_register_handler(M_CNTP_PPI, ppi_spy, 0, "cntp-ppi"));
    gicc_mock[M_GICC_IAR / 4] = 0x3ffu;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(0, eoi_write_count);
    assert_eq(0, ppi_handler_calls);
    assert_eq((uint32_t)0, rd(gicc_mock, M_GICC_EOIR));
}

int main(void)
{
    suite_raw_iar_eoi();
    suite_tlb_not_regressed();
    suite_duplicate_sgi_merges();
    suite_spurious_no_eoi();
    suite_ppi_eoi_once();
    suite_ppi_spurious_not_eoied();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_aarch64_resched_sgi",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

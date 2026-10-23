/* hosttests/cases/test_intr_dispatch.c
 *
 * Common stateless interrupt dispatcher (dispatch phase 1, Task 2).
 *
 * Links the PRODUCTION dispatcher kernel/intr/dispatch.c :: arch_intr_dispatch()
 * and drives it with STRONG spy backends defined in this translation unit:
 * arch_intr_handle_sync(), arch_intr_handle_irq() and arch_intr_bad_event().
 * The per-architecture bad-event backends (kernel/arch/<arch>/intr/bad_event.c)
 * are NOT linked here — they mask interrupts and park the CPU forever, so they
 * are host-unfriendly; this TU supplies the same contract in a testable form.
 *
 * Contract under test (spec §5.2):
 *   • the dispatcher validates ONLY event != NULL, regs != NULL and the kind;
 *   • INTR_SYNC routes to arch_intr_handle_sync() exactly once, INTR_IRQ to
 *     arch_intr_handle_irq() exactly once, with the ORIGINAL pointers;
 *   • a NULL event, a NULL regs or an out-of-range kind reaches the
 *     non-returning arch_intr_bad_event();
 *   • the dispatcher owns no cleanup: a backend may escape non-locally and the
 *     dispatcher performs no tail work afterwards.
 *
 * The death path is never mocked into a normal return: the bad-event spy
 * escapes with longjmp, exactly as a real bad-event backend never comes back.
 *
 * Every expectation is an independent literal; the event struct has padding,
 * so all event comparisons are member by member, never memcmp.
 */
#include <test_framework.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arch/regs.h>
#include <intr/dispatch.h>
#include <arch/intr.h>

/* ── Spy state ───────────────────────────────────────────────────────── */

static int g_sync_calls;
static int g_irq_calls;
static int g_bad_calls;

/* Pointers the backend last received — the dispatcher must pass the ORIGINAL
 * event / regs pointers through unchanged. */
static const struct intr_event *g_sync_event;
static pt_regs_t              *g_sync_regs;
static const struct intr_event *g_irq_event;
static pt_regs_t              *g_irq_regs;
static const struct intr_event *g_bad_event;
static pt_regs_t              *g_bad_regs;

/* setjmp targets. */
static jmp_buf g_bad_jb;      /* bad-event escape                     */
static jmp_buf g_escape_jb;   /* non-local escape out of the sync spy */

/* When set, the sync spy escapes non-locally instead of returning. */
static int g_sync_escapes;

/* ── Strong spy backends ─────────────────────────────────────────────── */

void arch_intr_handle_sync(const struct intr_event *event, pt_regs_t *regs)
{
    g_sync_calls++;
    g_sync_event = event;
    g_sync_regs  = regs;

    if (g_sync_escapes) {
        /* Non-local escape: unwind straight back to the test's restore
         * point.  The dispatcher must not require any cleanup before or
         * after this call. */
        longjmp(g_escape_jb, 1);
    }
}

void arch_intr_handle_irq(const struct intr_event *event, pt_regs_t *regs)
{
    g_irq_calls++;
    g_irq_event = event;
    g_irq_regs  = regs;
}

__attribute__((noreturn))
void arch_intr_bad_event(const struct intr_event *event, pt_regs_t *regs)
{
    g_bad_calls++;
    g_bad_event = event;
    g_bad_regs  = regs;

    /* A real backend never returns; the spy escapes the same way. */
    longjmp(g_bad_jb, 1);
}

/* ── Fixtures ────────────────────────────────────────────────────────── */

/* Independent literals for the event fixture — no production helper
 * generates them. */
#define FIX_ORIGIN_KERNEL  INTR_ORIGIN_KERNEL
#define FIX_REASON_MEMORY  INTR_REASON_MEMORY
#define FIX_ACCESS_WRITE   INTR_ACCESS_WRITE
#define FIX_FAULT_ADDR     0x0000123456789000ULL
#define FIX_NATIVE_VECTOR  14ULL
#define FIX_NATIVE_STATUS  0x0002ULL
#define FIX_NATIVE_ADDRESS 0x0000123456789000ULL

#define FIX_REG_RIP        0x00000000DEADBEEFULL
#define FIX_REG_CS         0x08ULL
#define FIX_REG_RAX        0x00000000CAFEBABEULL

/* A kind value outside the contract's enumeration. */
#define FIX_BAD_KIND       42

static void fill_event(struct intr_event *ev, enum intr_kind kind)
{
    ev->kind             = kind;
    ev->origin           = FIX_ORIGIN_KERNEL;
    ev->reason           = FIX_REASON_MEMORY;
    ev->access           = FIX_ACCESS_WRITE;
    ev->fault_addr_valid = true;
    ev->fault_addr       = FIX_FAULT_ADDR;
    ev->native_vector    = FIX_NATIVE_VECTOR;
    ev->native_status    = FIX_NATIVE_STATUS;
    ev->native_address   = FIX_NATIVE_ADDRESS;
}

/* Member-by-member comparison of the event the backend received against the
 * independent literals above.  The struct has padding, so memcmp is wrong. */
static void check_event_members(const struct intr_event *ev,
                                enum intr_kind kind)
{
    assert_eq(kind,                ev->kind);
    assert_eq(FIX_ORIGIN_KERNEL,   ev->origin);
    assert_eq(FIX_REASON_MEMORY,   ev->reason);
    assert_eq(FIX_ACCESS_WRITE,    ev->access);
    assert_eq(true,                ev->fault_addr_valid);
    assert_eq(FIX_FAULT_ADDR,      ev->fault_addr);
    assert_eq(FIX_NATIVE_VECTOR,   ev->native_vector);
    assert_eq(FIX_NATIVE_STATUS,   ev->native_status);
    assert_eq(FIX_NATIVE_ADDRESS,  ev->native_address);
}

static void fill_regs(pt_regs_t *regs)
{
    for (unsigned i = 0; i < sizeof(*regs) / sizeof(uint64_t); i++) {
        ((uint64_t *)regs)[i] = 0;
    }
    regs->rip = FIX_REG_RIP;
    regs->cs  = FIX_REG_CS;
    regs->rax = FIX_REG_RAX;
}

static void reset_spies(void)
{
    g_sync_calls   = 0;
    g_irq_calls    = 0;
    g_bad_calls    = 0;
    g_sync_event   = NULL;
    g_sync_regs    = NULL;
    g_irq_event    = NULL;
    g_irq_regs     = NULL;
    g_bad_event    = NULL;
    g_bad_regs     = NULL;
    g_sync_escapes = 0;
}

/* ── Case: sync_routes_once ─────────────────────────────────────────── */

TEST_FUNC(sync_routes_once)
{
    struct intr_event ev;
    struct intr_event ev_before;
    pt_regs_t regs;
    pt_regs_t regs_before;

    TEST_SUITE("sync_routes_once");

    reset_spies();
    fill_event(&ev, INTR_SYNC);
    fill_regs(&regs);

    ev_before = ev;
    memcpy(&regs_before, &regs, sizeof(regs));

    arch_intr_dispatch(&ev, &regs);

    /* Routed to the sync backend exactly once ... */
    assert_eq(1, g_sync_calls);
    /* ... and to nobody else. */
    assert_eq(0, g_irq_calls);
    assert_eq(0, g_bad_calls);

    /* The original pointers are handed through unchanged. */
    assert_true(g_sync_event == &ev);
    assert_true(g_sync_regs  == &regs);

    /* The event reached the backend with its members intact. */
    check_event_members(g_sync_event, INTR_SYNC);

    /* The dispatcher changed neither the event nor the register frame. */
    assert_eq(INTR_SYNC,             ev_before.kind);
    assert_eq(FIX_ORIGIN_KERNEL,     ev_before.origin);
    assert_eq(FIX_REASON_MEMORY,     ev_before.reason);
    assert_eq(FIX_ACCESS_WRITE,      ev_before.access);
    assert_eq(true,                  ev_before.fault_addr_valid);
    assert_eq(FIX_FAULT_ADDR,        ev_before.fault_addr);
    assert_eq(FIX_NATIVE_VECTOR,     ev_before.native_vector);
    assert_eq(FIX_NATIVE_STATUS,     ev_before.native_status);
    assert_eq(FIX_NATIVE_ADDRESS,    ev_before.native_address);
    assert_eq(INTR_SYNC,             ev.kind);
    assert_eq(FIX_ORIGIN_KERNEL,     ev.origin);
    assert_eq(FIX_REASON_MEMORY,     ev.reason);
    assert_eq(FIX_ACCESS_WRITE,      ev.access);
    assert_eq(true,                  ev.fault_addr_valid);
    assert_eq(FIX_FAULT_ADDR,        ev.fault_addr);
    assert_eq(FIX_NATIVE_VECTOR,     ev.native_vector);
    assert_eq(FIX_NATIVE_STATUS,     ev.native_status);
    assert_eq(FIX_NATIVE_ADDRESS,    ev.native_address);

    /* pt_regs_t is a pure sequence of 64-bit words (no padding), so the
     * frame can be compared byte for byte. */
    assert_eq(0, memcmp(&regs_before, &regs, sizeof(regs)));
    assert_eq(FIX_REG_RIP, regs.rip);
    assert_eq(FIX_REG_CS,  regs.cs);
    assert_eq(FIX_REG_RAX, regs.rax);
}

/* ── Case: irq_routes_once ──────────────────────────────────────────── */

TEST_FUNC(irq_routes_once)
{
    struct intr_event ev;
    pt_regs_t regs;
    pt_regs_t regs_before;

    TEST_SUITE("irq_routes_once");

    reset_spies();
    fill_event(&ev, INTR_IRQ);
    fill_regs(&regs);
    memcpy(&regs_before, &regs, sizeof(regs));

    arch_intr_dispatch(&ev, &regs);

    /* Routed to the IRQ backend exactly once ... */
    assert_eq(1, g_irq_calls);
    /* ... and to nobody else. */
    assert_eq(0, g_sync_calls);
    assert_eq(0, g_bad_calls);

    /* The original pointers are handed through unchanged. */
    assert_true(g_irq_event == &ev);
    assert_true(g_irq_regs  == &regs);

    /* The event reached the backend with its members intact. */
    check_event_members(g_irq_event, INTR_IRQ);

    /* The frame was not touched. */
    assert_eq(0, memcmp(&regs_before, &regs, sizeof(regs)));
}

/* ── Case: null_and_invalid_kind_die ────────────────────────────────── */

TEST_FUNC(null_and_invalid_kind_die)
{
    /* Static storage duration: a longjmp must not leave these
     * indeterminate (C17 7.13.2.1p3). */
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("null_and_invalid_kind_die");

    /* (1) NULL event. */
    reset_spies();
    fill_regs(&regs);
    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(NULL, &regs);
        /* The dispatcher must not come back on a NULL event. */
        assert_true(false);
    }
    assert_eq(1, g_bad_calls);
    assert_eq(0, g_sync_calls);
    assert_eq(0, g_irq_calls);
    assert_true(g_bad_event == NULL);
    assert_true(g_bad_regs  == &regs);

    /* (2) NULL regs. */
    reset_spies();
    fill_event(&ev, INTR_SYNC);
    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(&ev, NULL);
        assert_true(false);
    }
    assert_eq(1, g_bad_calls);
    assert_eq(0, g_sync_calls);
    assert_eq(0, g_irq_calls);
    assert_true(g_bad_event == &ev);
    assert_true(g_bad_regs  == NULL);

    /* (3) Out-of-range kind. */
    reset_spies();
    fill_event(&ev, INTR_SYNC);
    ev.kind = (enum intr_kind)FIX_BAD_KIND;   /* neither INTR_SYNC nor INTR_IRQ */
    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        assert_true(false);
    }
    assert_eq(1, g_bad_calls);
    assert_eq(0, g_sync_calls);
    assert_eq(0, g_irq_calls);
    assert_true(g_bad_event == &ev);
    assert_true(g_bad_regs  == &regs);
}

/* ── Case: nonlocal_escape_skips_tail ───────────────────────────────── */

TEST_FUNC(nonlocal_escape_skips_tail)
{
    /* Static storage duration: a longjmp must not leave these
     * indeterminate (C17 7.13.2.1p3). */
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("nonlocal_escape_skips_tail");

    reset_spies();
    fill_event(&ev, INTR_SYNC);
    fill_regs(&regs);

    g_sync_escapes = 1;

    if (setjmp(g_escape_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        /* The backend escaped non-locally, so the dispatcher never
         * returned here. */
        assert_true(false);
    }

    g_sync_escapes = 0;

    /* The sync backend was entered exactly once ... */
    assert_eq(1, g_sync_calls);
    /* ... the IRQ channel was never touched (no tail work) ... */
    assert_eq(0, g_irq_calls);
    /* ... and no bad-event was raised. */
    assert_eq(0, g_bad_calls);

    /* The escape happened with the original pointers. */
    assert_true(g_sync_event == &ev);
    assert_true(g_sync_regs  == &regs);
}

TEST_LIST_BEGIN
    TEST_ENTRY(sync_routes_once),
    TEST_ENTRY(irq_routes_once),
    TEST_ENTRY(null_and_invalid_kind_die),
    TEST_ENTRY(nonlocal_escape_skips_tail),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}

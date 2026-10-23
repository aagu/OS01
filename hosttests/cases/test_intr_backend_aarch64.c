/* hosttests/cases/test_intr_backend_aarch64.c
 *
 * aarch64 routing backend (dispatch phase 1, Task 3).
 *
 * Links three PRODUCTION objects:
 *   • kernel/arch/aarch64/intr/intr_decode.c  (pure ARM decoders)
 *   • kernel/arch/aarch64/intr/dispatch.c     (arch_intr_handle_sync/irq)
 *   • kernel/intr/dispatch.c                  (common arch_intr_dispatch)
 *
 * and replaces the terminal ends with spies defined in this TU:
 *   • aarch64_el1_sync_fatal — records the raw esr/far it was handed and
 *     escapes via longjmp (a real fatal never returns),
 *   • gic_dev_dispatch       — counts calls and records dev/regs,
 *   • gic_dev_current        — returns a fixed fake device (gic.c is too
 *     heavy for the host harness),
 *   • arch_intr_bad_event    — noreturn spy escaping via longjmp.
 *
 * Named cases (task brief):
 *   sync_fatal_preserves_raw — slot 0x200; the ORIGINAL ESR/FAR reach the
 *     fatal spy unchanged (also with FnV=1 and a nonzero raw FAR); the GIC
 *     is never called; an unknown EC goes to the original fatal path, not
 *     bad-event.
 *   irq_gic_once — slot 0x280; the GIC dispatcher is called EXACTLY once
 *     with the current device and the original regs.
 *   bad_mode_or_payload — a non-EL1h frame, an UNKNOWN origin, a wrong
 *     slot, a wrong kind, a source/decode field inconsistency and the
 *     lower-EL EC contradiction all reach bad-event (and nothing else).
 *
 * The aarch64 pt_regs_t comes from the same mock shim Task 1 used
 * (mock/intr_dispatch/aarch64/arch/regs.h shadows the arch/regs.h facade
 * on an x86_64 host).  All expectations are independent literals; event
 * comparisons are member by member — struct intr_event has padding, so
 * memcmp is never used on it.
 */
#include <test_framework.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arch/regs.h>
#include <intr/dispatch.h>
#include <arch/intr.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/intr_dispatch.h>
#include <arch/aarch64/gic.h>

/* ── Independent literal expectations ────────────────────────────────── */

#define A64_SYNC_SLOT_LIT   0x200ULL      /* VBAR slot: Sync  SP_ELx    */
#define A64_IRQ_SLOT_LIT    0x280ULL      /* VBAR slot: IRQ   SP_ELx    */

#define SPSR_EL1H_LIT       0x5ULL        /* M[4:0] = 0b00101           */
#define SPSR_EL0T_LIT       0x0ULL        /* M[4:0] = 0b00000 → USER    */
#define SPSR_EL1T_LIT       0x4ULL        /* M[4:0] = 0b00100 → UNKNOWN */

#define ESR_DA_SAME_LIT     ((0x25ULL << 26) | 0x05ULL)
        /* data abort, same EL, translation-fault FSC 0x05, WnR=0 → READ */
#define ESR_DA_SAME_FNV_LIT (((0x25ULL << 26) | 0x05ULL) | (1ULL << 10))
        /* same data abort with ISS.FnV=1 → normalized addr invalid      */
#define ESR_UNKNOWN_EC_LIT  (0x3CULL << 26)   /* BRK — OTHER to the decoder */
#define ESR_DA_LOWER_LIT    ((0x24ULL << 26) | 0x05ULL)
        /* data abort, LOWER EL — contradicts a saved EL1h frame         */

#define FAR_LIT             0x0000123456789000ULL
#define FAR_FNV_LIT         0x00000000DEAD0000ULL
#define ELR_LIT             0x00000000DEADBEEFULL
#define X0_LIT              0x00000000CAFEBABEULL

/* ── Spy state ───────────────────────────────────────────────────────── */

static int g_fatal_calls;
static int g_gic_calls;
static int g_bad_calls;

static uint64_t g_fatal_esr;
static uint64_t g_fatal_far;
static struct pt_regs *g_fatal_regs;

static struct gic_dev *g_gic_dev;
static struct pt_regs *g_gic_regs;

static struct gic_dev g_fake_dev;    /* gic_dev_current()'s fixed answer */

static const struct intr_event *g_bad_event;
static pt_regs_t              *g_bad_regs;

static jmp_buf g_fatal_jb;
static jmp_buf g_bad_jb;

/* ── Spy terminal ends ───────────────────────────────────────────────── */

/* The production declaration (arch/aarch64/intr_dispatch.h) is included
 * above, so a signature drift is a compile error.  A real fatal masks
 * DAIF and parks forever; the spy records and escapes the same way the
 * caller would never come back. */
__attribute__((noreturn))
void aarch64_el1_sync_fatal(struct pt_regs *regs, uint64_t esr, uint64_t far)
{
    g_fatal_calls++;
    g_fatal_esr  = esr;
    g_fatal_far  = far;
    g_fatal_regs = regs;
    longjmp(g_fatal_jb, 1);
}

void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs)
{
    g_gic_calls++;
    g_gic_dev  = dev;
    g_gic_regs = regs;
}

struct gic_dev *gic_dev_current(void)
{
    return &g_fake_dev;
}

__attribute__((noreturn))
void arch_intr_bad_event(const struct intr_event *event, pt_regs_t *regs)
{
    g_bad_calls++;
    g_bad_event = event;
    g_bad_regs  = regs;
    longjmp(g_bad_jb, 1);
}

/* ── Fixtures ────────────────────────────────────────────────────────── */

static void fill_regs(pt_regs_t *regs, uint64_t spsr)
{
    memset(regs, 0, sizeof(*regs));
    regs->x0        = X0_LIT;
    regs->elr_el1   = ELR_LIT;
    regs->spsr_el1  = spsr;
}

static void reset_spies(void)
{
    g_fatal_calls = 0;
    g_gic_calls   = 0;
    g_bad_calls   = 0;
    g_fatal_esr   = 1;
    g_fatal_far   = 1;
    g_fatal_regs  = NULL;
    g_gic_dev     = NULL;
    g_gic_regs    = NULL;
    g_bad_event   = NULL;
    g_bad_regs    = NULL;
}

/* Member-by-member expected-event check for a decoded EL1h sync event.
 * Independent literals only — never the decoder's own output. */
static void check_sync_event_members(const struct intr_event *ev,
                                     uint64_t esr, uint64_t far)
{
    assert_eq(INTR_SYNC,            ev->kind);
    assert_eq(INTR_ORIGIN_KERNEL,   ev->origin);
    assert_eq(A64_SYNC_SLOT_LIT,    ev->native_vector);
    assert_eq(esr,                  ev->native_status);
    assert_eq(far,                  ev->native_address);
    /* EC 0x25 is a data abort → MEMORY.  FSC 0x05, WnR=0, CM=0, S1PTW=0
     * is inside the read/write whitelist → READ. */
    assert_eq(INTR_REASON_MEMORY,   ev->reason);
    assert_eq(INTR_ACCESS_READ,     ev->access);
}

/* ── Case: sync_fatal_preserves_raw ──────────────────────────────────── */

TEST_FUNC(sync_fatal_preserves_raw)
{
    /* Static storage duration: the fatal spy longjmps out of the dispatch
     * chain, so nothing on an unwound stack may be read afterwards
     * (C17 7.13.2.1p3). */
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("sync_fatal_preserves_raw");

    /* (a) plain same-EL data abort: the raw ESR/FAR reach the fatal spy
     * unchanged, the GIC is never called, no bad-event. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_LIT, FAR_LIT);

    /* Slot check against the independent literal BEFORE dispatch. */
    assert_eq(A64_SYNC_SLOT_LIT, ev.native_vector);
    check_sync_event_members(&ev, ESR_DA_SAME_LIT, FAR_LIT);

    if (setjmp(g_fatal_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        assert_true(false);    /* the sync terminal end never returns */
    }
    assert_eq(1, g_fatal_calls);
    assert_eq(ESR_DA_SAME_LIT, g_fatal_esr);
    assert_eq(FAR_LIT,         g_fatal_far);
    assert_true(g_fatal_regs == &regs);
    assert_eq(0, g_gic_calls);
    assert_eq(0, g_bad_calls);

    /* (b) FnV=1 with a NONZERO raw FAR: the normalized address is invalid
     * and zeroed, but the ORIGINAL FAR is still handed to the fatal spy —
     * the diagnostic payload must not be lost to normalization. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_FNV_LIT, FAR_FNV_LIT);

    assert_eq(A64_SYNC_SLOT_LIT, ev.native_vector);
    assert_eq(false,             ev.fault_addr_valid);
    assert_eq(0,                 ev.fault_addr);
    assert_eq(FAR_FNV_LIT,       ev.native_address);

    if (setjmp(g_fatal_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        assert_true(false);
    }
    assert_eq(1, g_fatal_calls);
    assert_eq(ESR_DA_SAME_FNV_LIT, g_fatal_esr);
    assert_eq(FAR_FNV_LIT,         g_fatal_far);
    assert_eq(0, g_gic_calls);
    assert_eq(0, g_bad_calls);

    /* (c) an EC the decoder maps to OTHER is NOT a contract violation:
     * it goes to the ORIGINAL fatal path, never to bad-event. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_UNKNOWN_EC_LIT, FAR_LIT);

    assert_eq(A64_SYNC_SLOT_LIT,  ev.native_vector);
    assert_eq(INTR_REASON_OTHER,  ev.reason);
    assert_eq(INTR_ACCESS_UNKNOWN, ev.access);

    if (setjmp(g_fatal_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        assert_true(false);
    }
    assert_eq(1, g_fatal_calls);
    assert_eq(ESR_UNKNOWN_EC_LIT, g_fatal_esr);
    assert_eq(FAR_LIT,            g_fatal_far);
    assert_eq(0, g_bad_calls);
    assert_eq(0, g_gic_calls);
}

/* ── Case: irq_gic_once ──────────────────────────────────────────────── */

TEST_FUNC(irq_gic_once)
{
    static struct intr_event ev;
    static pt_regs_t regs;
    static pt_regs_t regs_before;

    TEST_SUITE("irq_gic_once");

    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    memcpy(&regs_before, &regs, sizeof(regs));

    aarch64_intr_decode_irq(&ev, &regs);

    /* Payload contract against independent literals (spec §5.1): IRQ
     * event from the 0x280 slot with an all-zero native payload and no
     * fault fields. */
    assert_eq(INTR_IRQ,             ev.kind);
    assert_eq(INTR_ORIGIN_KERNEL,   ev.origin);
    assert_eq(A64_IRQ_SLOT_LIT,     ev.native_vector);
    assert_eq(0,                    ev.native_status);
    assert_eq(0,                    ev.native_address);
    assert_eq(INTR_REASON_OTHER,    ev.reason);
    assert_eq(INTR_ACCESS_UNKNOWN,  ev.access);
    assert_eq(false,                ev.fault_addr_valid);
    assert_eq(0,                    ev.fault_addr);

    /* The IRQ path RETURNS normally — no setjmp escape here. */
    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_gic_calls);
    assert_true(g_gic_dev  == &g_fake_dev);
    assert_true(g_gic_regs == &regs);
    assert_eq(0, g_fatal_calls);
    assert_eq(0, g_bad_calls);

    /* The normal-return contract: the register frame is untouched. */
    assert_eq(0, memcmp(&regs_before, &regs, sizeof(regs)));
}

/* ── Case: bad_mode_or_payload ───────────────────────────────────────── */

/* One bad-path probe: dispatch (event, regs) and require bad-event exactly
 * once with nothing else touched. */
static void expect_bad_event(struct intr_event *ev, pt_regs_t *regs)
{
    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(ev, regs);
        assert_true(false);    /* bad-event never returns */
    }
    assert_eq(1, g_bad_calls);
    assert_true(g_bad_event == ev);
    assert_true(g_bad_regs   == regs);
    assert_eq(0, g_fatal_calls);
    assert_eq(0, g_gic_calls);
}

TEST_FUNC(bad_mode_or_payload)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("bad_mode_or_payload");

    /* (a) non-EL1h frame: SPSR mode EL0t decodes to USER; the EL1h sync
     * entry must reject it via the origin, not claim an EL0 path. */
    reset_spies();
    fill_regs(&regs, SPSR_EL0T_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_LIT, FAR_LIT);
    assert_eq(INTR_ORIGIN_USER, ev.origin);
    expect_bad_event(&ev, &regs);

    /* (b) UNKNOWN origin: SPSR mode EL1t (0b00100) is neither EL1h nor
     * EL0t — the decoder reports UNKNOWN and the backend must halt. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1T_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_LIT, FAR_LIT);
    assert_eq(INTR_ORIGIN_UNKNOWN, ev.origin);
    expect_bad_event(&ev, &regs);

    /* (c) wrong slot: a decoded EL1h sync event whose native_vector was
     * corrupted no longer re-decodes member by member. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_LIT, FAR_LIT);
    ev.native_vector = A64_IRQ_SLOT_LIT;    /* tampered */
    expect_bad_event(&ev, &regs);

    /* (d) wrong kind: an IRQ-decoded event tampered to INTR_SYNC reaches
     * the sync backend, whose re-decode (slot 0x200, EC from a zero ESR)
     * disagrees member by member. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_irq(&ev, &regs);
    ev.kind = INTR_SYNC;                     /* tampered */
    expect_bad_event(&ev, &regs);

    /* (e) source/decode field inconsistency: a correctly decoded sync
     * event with one normalized member tampered must not pass the
     * deterministic re-validation. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_SAME_LIT, FAR_LIT);
    ev.reason = INTR_REASON_ARITHMETIC;      /* tampered */
    expect_bad_event(&ev, &regs);

    /* (f) lower-EL EC contradiction: ESR.EC 0x24 (data abort, lower EL)
     * cannot come from a frame whose saved SPSR is EL1h. */
    reset_spies();
    fill_regs(&regs, SPSR_EL1H_LIT);
    aarch64_intr_decode_sync(&ev, &regs, ESR_DA_LOWER_LIT, FAR_LIT);
    expect_bad_event(&ev, &regs);
}

TEST_LIST_BEGIN
    TEST_ENTRY(sync_fatal_preserves_raw),
    TEST_ENTRY(irq_gic_once),
    TEST_ENTRY(bad_mode_or_payload),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}

/* hosttests/cases/test_intr_backend_x86.c
 *
 * x86_64 routing backend (phase2 installed exception coverage).
 *
 * Links three PRODUCTION objects:
 *   • kernel/arch/x86_64/intr/intr_decode.c  (pure x86 decoder)
 *   • kernel/arch/x86_64/intr/dispatch.c     (arch_intr_handle_sync/irq)
 *   • kernel/intr/dispatch.c                 (common arch_intr_dispatch)
 *
 * and replaces the terminal ends with spies defined in this TU:
 *   • x86_handle_divide_error / x86_handle_undefined_opcode /
 *     x86_handle_general_protection — count calls and record the error
 *     code and regs pointer they were handed,
 *   • x86_handle_page_fault — also records the CR2 snapshot,
 *   • arch_intr_bad_event — noreturn spy escaping via longjmp.
 *
 * Named cases (task brief):
 *   sync_de_routes_once / sync_ud_routes_once / sync_gp_routes_once /
 *   sync_pf_routes_once — for each of the four migrated vectors the
 *     corresponding private handler is called EXACTLY once with the
 *     ORIGINAL regs pointer, the error code is passed through unchanged
 *     and (#PF only) the CR2 snapshot is passed through unchanged; no
 *     bad-event, no other handler touched.  The #PF case also proves a
 *     KERNEL-origin event whose #PF error-code U/S bit is set is NOT
 *     rejected: the CS-derived origin and the U/S bit are independent
 *     (spec §6.1 — the kernel legitimately faults on user addresses).
 *   irq_unknown_vector_rejected — vector zero is not an installed IRQ;
 *     the production IRQ chain is covered in test_intr_irq_x86.
 *   bad_origin_or_payload — an UNKNOWN origin (CPL 1/2), a tampered
 *     origin, a tampered classification, a #GP borrowing the #PF CR2
 *     and an UNMIGRATED vector (21) all reach bad-event and nothing
 *     else: unmigrated vectors must fail closed.
 *
 * The x86_64 pt_regs_t is the NATIVE production layout (the host is
 * x86_64, so the arch/regs.h facade selects arch/x86_64/regs.h) — no
 * mock shim is needed.  All expectations are independent literals;
 * event comparisons are member by member — struct intr_event has
 * padding, so memcmp is never used on it.
 */
#include <test_framework.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arch/regs.h>
#include <intr/dispatch.h>
#include <arch/intr.h>
#include <arch/x86_64/intr_decode.h>
#include <arch/x86_64/intr_dispatch.h>

/* ── Independent literal expectations ────────────────────────────────── */

/* The four migrated vectors (spec §6.1) plus one unmigrated one. */
#define VEC_DE_LIT          0ULL
#define VEC_UD_LIT          6ULL
#define VEC_GP_LIT          13ULL
#define VEC_PF_LIT          14ULL
#define VEC_UNMIGRATED_LIT  21ULL    /* #CP-ish reserved slot: NOT migrated */

/* Saved CS selectors: low two bits are the RPL the exception was
 * taken at (0 → KERNEL, 3 → USER, 1/2 → UNKNOWN). */
#define CS_KERNEL_LIT       0x08ULL
#define CS_USER_LIT         0x1bULL
#define CS_CPL1_LIT         0x09ULL  /* RPL 1 → INTR_ORIGIN_UNKNOWN */

#define ERR_DE_LIT          0x00ULL
#define ERR_UD_LIT          0x00ULL
#define ERR_GP_LIT          0x00ULL
#define ERR_PF_WRITE_LIT    0x02ULL  /* P=0, W/R=1 → WRITE             */
#define ERR_PF_USERADDR_LIT 0x06ULL  /* P=0, W/R=1, U/S=1: kernel CS +  */
                                     /* user target — independent (§6.1) */

#define CR2_LIT             0x00001234deadb000ULL
#define RIP_LIT             0x00000000deadbeefULL

/* ── Spy state ───────────────────────────────────────────────────────── */

static int g_de_calls, g_ud_calls, g_gp_calls, g_pf_calls, g_bad_calls;

static uint64_t g_de_err, g_ud_err, g_gp_err, g_pf_err, g_pf_cr2;
static pt_regs_t *g_de_regs, *g_ud_regs, *g_gp_regs, *g_pf_regs;

static const struct intr_event *g_bad_event;
static pt_regs_t              *g_bad_regs;

static jmp_buf g_bad_jb;

/* ── Spy terminal ends ───────────────────────────────────────────────── */

/* The production declarations (arch/x86_64/intr_dispatch.h) are included
 * above, so a signature drift is a compile error.  The real business
 * handlers may never return through the dispatcher (user kill / kernel
 * halt / longjmp); the spy records and returns normally so the routing
 * contract itself can be asserted. */
void x86_handle_divide_error(pt_regs_t *regs, uint64_t error_code)
{
    g_de_calls++;
    g_de_err  = error_code;
    g_de_regs = regs;
}

void x86_handle_undefined_opcode(pt_regs_t *regs, uint64_t error_code)
{
    g_ud_calls++;
    g_ud_err  = error_code;
    g_ud_regs = regs;
}

void x86_handle_general_protection(pt_regs_t *regs, uint64_t error_code)
{
    g_gp_calls++;
    g_gp_err  = error_code;
    g_gp_regs = regs;
}

void x86_handle_page_fault(pt_regs_t *regs, uint64_t error_code, uint64_t cr2)
{
    g_pf_calls++;
    g_pf_err  = error_code;
    g_pf_cr2  = cr2;
    g_pf_regs = regs;
}

__attribute__((noreturn))
void arch_intr_bad_event(const struct intr_event *event, pt_regs_t *regs)
{
    g_bad_calls++;
    g_bad_event = event;
    g_bad_regs  = regs;
    longjmp(g_bad_jb, 1);
}

static int extra_calls[21];
static uint64_t extra_err;
static pt_regs_t *extra_regs;
static jmp_buf fatal_jb;
void x86_handle_debug(pt_regs_t *regs, uint64_t err)
{
    extra_calls[1]++;
    extra_err = err;
    extra_regs = regs;
    longjmp(fatal_jb, 1);
}

void x86_handle_nmi(pt_regs_t *regs, uint64_t err)
{
    extra_calls[2]++;
    extra_err = err;
    extra_regs = regs;
    longjmp(fatal_jb, 1);
}

void x86_handle_int3(pt_regs_t *regs, uint64_t err)
{
    extra_calls[3]++;
    extra_err = err;
    extra_regs = regs;
    longjmp(fatal_jb, 1);
}

void x86_handle_overflow(pt_regs_t *regs, uint64_t err)
{
    extra_calls[4]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_bounds(pt_regs_t *regs, uint64_t err)
{
    extra_calls[5]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_dev_not_available(pt_regs_t *regs, uint64_t err)
{
    extra_calls[7]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_double_fault(pt_regs_t *regs, uint64_t err)
{
    extra_calls[8]++;
    extra_err = err;
    extra_regs = regs;
    longjmp(fatal_jb, 1);
}

void x86_handle_coprocessor_segment_overrun(pt_regs_t *regs, uint64_t err)
{
    extra_calls[9]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_invalid_TSS(pt_regs_t *regs, uint64_t err)
{
    extra_calls[10]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_segment_not_present(pt_regs_t *regs, uint64_t err)
{
    extra_calls[11]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_stack_segment_fault(pt_regs_t *regs, uint64_t err)
{
    extra_calls[12]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_x87_FPU_error(pt_regs_t *regs, uint64_t err)
{
    extra_calls[16]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_alignment_check(pt_regs_t *regs, uint64_t err)
{
    extra_calls[17]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_machine_check(pt_regs_t *regs, uint64_t err)
{
    extra_calls[18]++;
    extra_err = err;
    extra_regs = regs;
    longjmp(fatal_jb, 1);
}

void x86_handle_SIMD_exception(pt_regs_t *regs, uint64_t err)
{
    extra_calls[19]++;
    extra_err = err;
    extra_regs = regs;
}

void x86_handle_virtualization_exception(pt_regs_t *regs, uint64_t err)
{
    extra_calls[20]++;
    extra_err = err;
    extra_regs = regs;
}

/* ── Fixtures ────────────────────────────────────────────────────────── */

static void fill_regs(pt_regs_t *regs, uint64_t cs)
{
    memset(regs, 0, sizeof(*regs));
    regs->rip = RIP_LIT;
    regs->cs  = cs;
}

static void reset_spies(void)
{
    g_de_calls = g_ud_calls = g_gp_calls = g_pf_calls = g_bad_calls = 0;
    g_de_err = g_ud_err = g_gp_err = g_pf_err = g_pf_cr2 = 1;
    g_de_regs = g_ud_regs = g_gp_regs = g_pf_regs = NULL;
    g_bad_event = NULL;
    g_bad_regs  = NULL;
}

/* No business handler and no bad-event may have fired. */
static void assert_no_handlers_fired(void)
{
    assert_eq(0, g_de_calls);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_pf_calls);
    assert_eq(0, g_bad_calls);
}

/* ── Case: sync_*_routes_once ────────────────────────────────────────── */

TEST_FUNC(sync_de_routes_once)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("sync_de_routes_once");

    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_DE_LIT, ERR_DE_LIT, 0);

    /* Payload contract against independent literals. */
    assert_eq(INTR_SYNC,              ev.kind);
    assert_eq(INTR_ORIGIN_USER,       ev.origin);
    assert_eq(INTR_REASON_ARITHMETIC, ev.reason);
    assert_eq(INTR_ACCESS_UNKNOWN,    ev.access);
    assert_eq(false,                  ev.fault_addr_valid);
    assert_eq(0,                      ev.fault_addr);
    assert_eq(VEC_DE_LIT,             ev.native_vector);
    assert_eq(ERR_DE_LIT,             ev.native_status);
    assert_eq(0,                      ev.native_address);

    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_de_calls);
    assert_eq(ERR_DE_LIT, g_de_err);
    assert_true(g_de_regs == &regs);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_pf_calls);
    assert_eq(0, g_bad_calls);
}

TEST_FUNC(sync_ud_routes_once)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("sync_ud_routes_once");

    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_UD_LIT, ERR_UD_LIT, 0);

    assert_eq(INTR_SYNC,            ev.kind);
    assert_eq(INTR_ORIGIN_USER,     ev.origin);
    assert_eq(INTR_REASON_ILLEGAL,  ev.reason);
    assert_eq(VEC_UD_LIT,           ev.native_vector);
    assert_eq(ERR_UD_LIT,           ev.native_status);
    assert_eq(0,                    ev.native_address);

    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_ud_calls);
    assert_eq(ERR_UD_LIT, g_ud_err);
    assert_true(g_ud_regs == &regs);
    assert_eq(0, g_de_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_pf_calls);
    assert_eq(0, g_bad_calls);
}

TEST_FUNC(sync_gp_routes_once)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("sync_gp_routes_once");

    reset_spies();
    fill_regs(&regs, CS_KERNEL_LIT);
    x86_intr_decode(&ev, &regs, VEC_GP_LIT, ERR_GP_LIT, 0);

    assert_eq(INTR_SYNC,               ev.kind);
    assert_eq(INTR_ORIGIN_KERNEL,      ev.origin);
    assert_eq(INTR_REASON_PROTECTION,  ev.reason);
    assert_eq(INTR_ACCESS_UNKNOWN,     ev.access);
    /* A #GP never owns an address: neither fault_addr nor the raw
     * native_address may borrow the #PF CR2. */
    assert_eq(false,                   ev.fault_addr_valid);
    assert_eq(0,                       ev.fault_addr);
    assert_eq(VEC_GP_LIT,              ev.native_vector);
    assert_eq(ERR_GP_LIT,              ev.native_status);
    assert_eq(0,                       ev.native_address);

    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_gp_calls);
    assert_eq(ERR_GP_LIT, g_gp_err);
    assert_true(g_gp_regs == &regs);
    assert_eq(0, g_de_calls);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_pf_calls);
    assert_eq(0, g_bad_calls);
}

TEST_FUNC(sync_pf_routes_once)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("sync_pf_routes_once");

    /* (a) user-origin write fault: the CR2 snapshot is handed to the
     * private handler unchanged. */
    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_PF_LIT, ERR_PF_WRITE_LIT, CR2_LIT);

    assert_eq(INTR_SYNC,            ev.kind);
    assert_eq(INTR_ORIGIN_USER,     ev.origin);
    assert_eq(INTR_REASON_MEMORY,   ev.reason);
    assert_eq(INTR_ACCESS_WRITE,    ev.access);
    assert_eq(true,                 ev.fault_addr_valid);
    assert_eq(CR2_LIT,              ev.fault_addr);
    assert_eq(VEC_PF_LIT,           ev.native_vector);
    assert_eq(ERR_PF_WRITE_LIT,     ev.native_status);
    assert_eq(CR2_LIT,              ev.native_address);

    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_pf_calls);
    assert_eq(ERR_PF_WRITE_LIT, g_pf_err);
    assert_eq(CR2_LIT,          g_pf_cr2);
    assert_true(g_pf_regs == &regs);
    assert_eq(0, g_de_calls);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_bad_calls);

    /* (b) KERNEL origin with the #PF error-code U/S bit SET: the CS
     * rule and the U/S bit are independent (spec §6.1) — the kernel
     * legitimately faults on user addresses, so this must route to
     * the #PF handler, not bad-event. */
    reset_spies();
    fill_regs(&regs, CS_KERNEL_LIT);
    x86_intr_decode(&ev, &regs, VEC_PF_LIT, ERR_PF_USERADDR_LIT, CR2_LIT);

    assert_eq(INTR_ORIGIN_KERNEL, ev.origin);
    assert_eq(INTR_ACCESS_WRITE,  ev.access);

    arch_intr_dispatch(&ev, &regs);

    assert_eq(1, g_pf_calls);
    assert_eq(ERR_PF_USERADDR_LIT, g_pf_err);
    assert_eq(CR2_LIT,             g_pf_cr2);
    assert_eq(0, g_bad_calls);
}

/* ── Case: irq_unknown_vector_rejected ───────────────────────────────────────────── */

TEST_FUNC(irq_unknown_vector_rejected)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("irq_unknown_vector_rejected");

    /* Vector zero is outside the installed IRQ families. */
    reset_spies();
    fill_regs(&regs, CS_KERNEL_LIT);
    memset(&ev, 0, sizeof(ev));
    ev.kind = INTR_IRQ;

    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(&ev, &regs);
        assert_true(false);    /* bad-event never returns */
    }
    assert_eq(1, g_bad_calls);
    assert_true(g_bad_event == &ev);
    assert_true(g_bad_regs   == &regs);
    assert_eq(0, g_de_calls);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_pf_calls);
}

/* ── Case: bad_origin_or_payload ─────────────────────────────────────── */

/* One bad-path probe: dispatch (event, regs) and require bad-event
 * exactly once with nothing else touched. */
static void expect_bad_event(struct intr_event *ev, pt_regs_t *regs)
{
    if (setjmp(g_bad_jb) == 0) {
        arch_intr_dispatch(ev, regs);
        assert_true(false);    /* bad-event never returns */
    }
    assert_eq(1, g_bad_calls);
    assert_true(g_bad_event == ev);
    assert_true(g_bad_regs   == regs);
    assert_eq(0, g_de_calls);
    assert_eq(0, g_ud_calls);
    assert_eq(0, g_gp_calls);
    assert_eq(0, g_pf_calls);
}

TEST_FUNC(bad_origin_or_payload)
{
    static struct intr_event ev;
    static pt_regs_t regs;

    TEST_SUITE("bad_origin_or_payload");

    /* (a) UNKNOWN origin: CS RPL 1 (CPL 1/2) decodes to UNKNOWN and the
     * sync backend must fail closed on it. */
    reset_spies();
    fill_regs(&regs, CS_CPL1_LIT);
    x86_intr_decode(&ev, &regs, VEC_UD_LIT, ERR_UD_LIT, 0);
    assert_eq(INTR_ORIGIN_UNKNOWN, ev.origin);
    expect_bad_event(&ev, &regs);

    /* (b) wrong origin: a user-frame event tampered to KERNEL origin no
     * longer re-decodes member by member. */
    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_DE_LIT, ERR_DE_LIT, 0);
    ev.origin = INTR_ORIGIN_KERNEL;          /* tampered */
    expect_bad_event(&ev, &regs);

    /* (c) wrong classification: #UD decoded ILLEGAL, tampered to
     * ARITHMETIC. */
    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_UD_LIT, ERR_UD_LIT, 0);
    ev.reason = INTR_REASON_ARITHMETIC;      /* tampered */
    expect_bad_event(&ev, &regs);

    /* (d) payload mismatch: a #GP event must never borrow the #PF CR2 —
     * a nonzero native_address fails the deterministic re-validation. */
    reset_spies();
    fill_regs(&regs, CS_KERNEL_LIT);
    x86_intr_decode(&ev, &regs, VEC_GP_LIT, ERR_GP_LIT, 0);
    ev.native_address = CR2_LIT;             /* tampered */
    expect_bad_event(&ev, &regs);

    /* (e) fault fields on a non-#PF vector: #UD cannot own a valid
     * fault address. */
    reset_spies();
    fill_regs(&regs, CS_USER_LIT);
    x86_intr_decode(&ev, &regs, VEC_UD_LIT, ERR_UD_LIT, 0);
    ev.fault_addr_valid = true;              /* tampered */
    ev.fault_addr       = CR2_LIT;
    expect_bad_event(&ev, &regs);

    /* (f) UNMIGRATED vector: 21 decodes consistently (OTHER/UNKNOWN/
     * no address) but is outside the four-case whitelist — it must
     * fail closed, never reach a business handler. */
    reset_spies();
    fill_regs(&regs, CS_KERNEL_LIT);
    x86_intr_decode(&ev, &regs, VEC_UNMIGRATED_LIT, 0, 0);
    assert_eq(INTR_REASON_OTHER, ev.reason);
    expect_bad_event(&ev, &regs);

    /* Sanity: nothing fired outside the probes. */
    reset_spies();
    assert_no_handlers_fired();
}

TEST_FUNC(all_installed_routes)
{
    static const unsigned vectors[] = {
        1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20
    };
    static pt_regs_t regs;
    static struct intr_event ev;
    TEST_SUITE("all_installed_routes");
    for (unsigned cpl = 0; cpl < 2; cpl++) {
      for (unsigned i = 0; i < sizeof(vectors)/sizeof(vectors[0]); i++) {
        unsigned v = vectors[i];
        memset(extra_calls, 0, sizeof(extra_calls));
        reset_spies();
        fill_regs(&regs, cpl ? CS_USER_LIT : CS_KERNEL_LIT);
        regs.errcode = 0xdead; /* The saved pseudo-error slot is not the C payload. */
        x86_intr_decode(&ev, &regs, v, 0x123400+v, CR2_LIT);
        if (setjmp(g_bad_jb) == 0) {
            if (setjmp(fatal_jb) == 0) {
                arch_intr_dispatch(&ev,&regs);
                assert_true(v!=1 && v!=2 && v!=3 && v!=8 && v!=18);
            }
        }
        assert_eq(0,g_bad_calls);
        for (unsigned j=0;j<21;j++) assert_eq(j==v ? 1 : 0,extra_calls[j]);
        assert_eq(0x123400+v,extra_err); assert_true(extra_regs==&regs);
        assert_eq(0, g_de_calls+g_ud_calls+g_gp_calls+g_pf_calls);
      }
    }
    /* Every installed vector rejects CPL1/2 and malformed normalized fields. */
    static const unsigned installed[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,16,17,18,19,20};
    for (unsigned i = 0; i < sizeof(installed)/sizeof(installed[0]); i++) {
        for (unsigned cpl = 1; cpl <= 2; cpl++) {
            reset_spies(); fill_regs(&regs, 0x08+cpl);
            x86_intr_decode(&ev, &regs, installed[i], 0, 0);
            expect_bad_event(&ev, &regs);
        }
        reset_spies(); fill_regs(&regs, CS_KERNEL_LIT);
        x86_intr_decode(&ev, &regs, installed[i], 0, 0);
        ev.access = INTR_ACCESS_EXEC;
        expect_bad_event(&ev, &regs);
    }
    static const unsigned unsupported[] = {15, 21, 0x80};
    for (unsigned i = 0; i < 3; i++) {
        reset_spies(); fill_regs(&regs,CS_KERNEL_LIT);
        x86_intr_decode(&ev,&regs,unsupported[i],0,0); expect_bad_event(&ev,&regs);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(all_installed_routes),
    TEST_ENTRY(sync_de_routes_once),
    TEST_ENTRY(sync_ud_routes_once),
    TEST_ENTRY(sync_gp_routes_once),
    TEST_ENTRY(sync_pf_routes_once),
    TEST_ENTRY(irq_unknown_vector_rejected),
    TEST_ENTRY(bad_origin_or_payload),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}

/* IRQ production business is exercised by test_intr_irq_x86; these boundaries
 * must stay unreachable for this suite's malformed vector-zero IRQ event. */
void x86_handle_device_irq(pt_regs_t *r, uint64_t v) { __builtin_trap(); }
void x86_handle_registered_irq(pt_regs_t *r, uint64_t v) { __builtin_trap(); }
void x86_handle_lapic_timer_irq(pt_regs_t *r, uint64_t v) { __builtin_trap(); }
void x86_handle_spurious_irq(pt_regs_t *r, uint64_t v) { __builtin_trap(); }

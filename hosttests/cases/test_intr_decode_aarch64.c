/* hosttests/cases/test_intr_decode_aarch64.c
 *
 * aarch64 normalized interrupt-event decoder (dispatch phase 1, Task 1).
 *
 * Links the PRODUCTION decoder
 *   kernel/arch/aarch64/intr/intr_decode.c :: aarch64_intr_decode_sync()
 *   kernel/arch/aarch64/intr/intr_decode.c :: aarch64_intr_decode_irq()
 * and checks it against independent expectation tables written here.
 * No expected value is derived from calling the decoder or from any
 * shared helper: the tables below are literal constants of the
 * contract (spec §5.1 event shape, §6.2 aarch64 decoding rules).
 *
 * The host is x86_64, so the production aarch64 pt_regs_t is reached
 * through the mock shim
 *   hosttests/mock/intr_dispatch/aarch64/arch/regs.h
 * which shadows the arch/regs.h facade and re-includes the production
 * arch/aarch64/regs.h.
 *
 * The event struct has padding, so every comparison is member by
 * member — never memcmp.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/regs.h>
#include <intr/dispatch.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/uaccess.h>

/* ── Independent expectation tables ────────────────────────────── */

/* spec §6.2: ESR_EL1.EC → reason. */
struct a64_ec_expect {
    unsigned         ec;
    enum intr_reason reason;
    const char      *name;
};

static const struct a64_ec_expect k_a64_ec_table[] = {
    { 0x20, INTR_REASON_MEMORY,     "instruction abort, lower EL" },
    { 0x21, INTR_REASON_MEMORY,     "instruction abort, same EL"  },
    { 0x24, INTR_REASON_MEMORY,     "data abort, lower EL"        },
    { 0x25, INTR_REASON_MEMORY,     "data abort, same EL"         },
    { 0x22, INTR_REASON_PROTECTION, "PC alignment fault"          },
    { 0x3c, INTR_REASON_OTHER,      "BRK"                         },
    { 0x00, INTR_REASON_OTHER,      "unknown EC"                  },
};

/* spec §6.2: SPSR_EL1 mode M[4:0] → origin. */
struct a64_mode_expect {
    uint64_t         mode;
    enum intr_origin origin;
    const char      *name;
};

static const struct a64_mode_expect k_a64_mode_table[] = {
    { 0x05, INTR_ORIGIN_KERNEL,  "EL1h"     },
    { 0x00, INTR_ORIGIN_USER,    "EL0t"     },
    { 0x04, INTR_ORIGIN_UNKNOWN, "EL1t"     },
    { 0x0d, INTR_ORIGIN_UNKNOWN, "SVC"      },
};

/* spec §6.2: data abort syndrome → access.  The READ/WRITE
 * whitelist is the translation/access-flag/permission fault FSC
 * range 0x04..0x0f with CM = 0 and S1PTW = 0. */
struct a64_data_access_expect {
    unsigned     fsc;
    unsigned     extra_iss;   /* CM / S1PTW bits, already shifted */
    bool         wnr;         /* syndrome says "write"            */
    enum intr_access access;
    const char  *name;
};

static const struct a64_data_access_expect k_a64_data_access_table[] = {
    { 0x04, 0,         false, INTR_ACCESS_READ,    "translation fault L0, read"  },
    { 0x07, 0,         true,  INTR_ACCESS_WRITE,   "translation fault L3, write" },
    { 0x08, 0,         false, INTR_ACCESS_READ,    "access flag fault L0"       },
    { 0x0b, 0,         true,  INTR_ACCESS_WRITE,   "access flag fault L3"       },
    { 0x0c, 0,         false, INTR_ACCESS_READ,    "permission fault L0"        },
    { 0x0f, 0,         true,  INTR_ACCESS_WRITE,   "permission fault L3"        },
    { 0x10, 0,         false, INTR_ACCESS_UNKNOWN, "alignment fault FSC"        },
    { 0x1f, 0,         false, INTR_ACCESS_UNKNOWN, "exception identification"   },
    { 0x05, (1u << 7), false, INTR_ACCESS_UNKNOWN, "stage-1 walk (S1PTW=1)"     },
    { 0x05, (1u << 8), false, INTR_ACCESS_UNKNOWN, "cache maintenance (CM=1)"   },
    /* ISS[9] is reserved: on its own it changes nothing, so the
     * syndrome is still classified by WnR. */
    { 0x05, (1u << 9), false, INTR_ACCESS_READ,    "reserved ISS[9] alone"      },
    { 0x05, (1u << 9), true,  INTR_ACCESS_WRITE,   "reserved ISS[9] with WnR"   },
};

/* Constants of the contract, spelled out here so the test does not
 * depend on any production decoder symbol. */
#define A64_ESR_EC_SHIFT   26
#define A64_ESR_ISS_WNR     6u    /* WnR (data abort)          */
#define A64_ESR_ISS_S1PTW   7u    /* stage-1 page-table walk   */
#define A64_ESR_ISS_CM      8u    /* cache maintenance         */
#define A64_ESR_ISS_RES9    9u    /* reserved — no effect      */
#define A64_ESR_ISS_FNV    10u    /* FAR not valid             */

#define A64_EC_SVC_LOWER   0x15u
#define A64_EC_INSN_LOWER 0x20u
#define A64_EC_INSN_SAME  0x21u
#define A64_EC_PC_ALIGN   0x22u
#define A64_EC_DATA_LOWER 0x24u
#define A64_EC_DATA_SAME  0x25u

#define A64_MODE_EL0T 0x00u
#define A64_MODE_EL1H 0x05u

#define A64_FAR_RAW   0x12345678ULL
#define A64_FAR_ZERO  0ULL

#define A64_SLOT_SYNC     0x200ULL
#define A64_SLOT_IRQ      0x280ULL
#define A64_SLOT_EL0_SYNC 0x400ULL

/* ── Helpers ───────────────────────────────────────────────────── */

static uint64_t mk_esr(unsigned ec, unsigned iss)
{
    return ((uint64_t)ec << A64_ESR_EC_SHIFT) | (uint64_t)iss;
}

static void fill_regs(pt_regs_t *regs, uint64_t mode)
{
    for (unsigned i = 0; i < sizeof(*regs) / sizeof(uint64_t); i++) {
        ((uint64_t *)regs)[i] = 0;
    }
    regs->spsr_el1 = mode;
}

/* Member-by-member comparison of the normalized event against the
 * expectation table row. */
static void check_event(const struct intr_event *ev,
                        enum intr_kind kind,
                        enum intr_origin origin,
                        enum intr_reason reason,
                        enum intr_access access,
                        bool fault_addr_valid,
                        uint64_t fault_addr,
                        uint64_t native_vector,
                        uint64_t native_status,
                        uint64_t native_address)
{
    assert_eq(kind, ev->kind);
    assert_eq(origin, ev->origin);
    assert_eq(reason, ev->reason);
    assert_eq(access, ev->access);
    assert_eq(fault_addr_valid, ev->fault_addr_valid);
    assert_eq(fault_addr, ev->fault_addr);
    assert_eq(native_vector, ev->native_vector);
    assert_eq(native_status, ev->native_status);
    assert_eq(native_address, ev->native_address);
}

/* ── Case: a64_modes_and_classes ─────────────────────────────── */

static void case_a64_modes_and_classes(void)
{
    pt_regs_t regs;

    TEST_SUITE("a64_modes_and_classes");

    /* ESR.EC → reason.  The origin is EL1h for all of these rows. */
    for (unsigned i = 0; i < sizeof(k_a64_ec_table) /
                            sizeof(k_a64_ec_table[0]); i++) {
        const struct a64_ec_expect *e = &k_a64_ec_table[i];
        struct intr_event ev;
        uint64_t esr = mk_esr(e->ec, 0);

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        assert_eq(INTR_SYNC, ev.kind);
        assert_eq(INTR_ORIGIN_KERNEL, ev.origin);
        assert_eq(e->reason, ev.reason);
        assert_eq(A64_SLOT_SYNC, ev.native_vector);
        assert_eq(esr, ev.native_status);
        /* The raw FAR is always retained, valid or not. */
        assert_eq(A64_FAR_RAW, ev.native_address);
    }

    /* The saved SPSR mode decides the origin. */
    for (unsigned i = 0; i < sizeof(k_a64_mode_table) /
                            sizeof(k_a64_mode_table[0]); i++) {
        const struct a64_mode_expect *e = &k_a64_mode_table[i];
        struct intr_event ev;

        fill_regs(&regs, e->mode);
        aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_DATA_SAME, 0),
                                 A64_FAR_RAW);

        assert_eq(INTR_SYNC, ev.kind);
        assert_eq(e->origin, ev.origin);
    }
}

/* ── Case: a64_el0_slots ────────────────────────────────────────── */

/* The decoder derives the VBAR slot from the SAVED mode: an EL0t
 * frame was taken through the lower-EL sync slot (0x400), an EL1h
 * frame through the EL1h sync slot (0x200).  The entry adapter states
 * the slot it was really taken through and the backend re-decodes and
 * compares — a frame that claims one slot and arrives through another
 * is rejected. */
static void case_a64_el0_slots(void)
{
    pt_regs_t regs;
    struct intr_event ev;

    TEST_SUITE("a64_el0_slots");

    /* An EL0t sync frame: the lower-EL sync slot. */
    fill_regs(&regs, A64_MODE_EL0T);
    aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_DATA_LOWER, 0x05u),
                             A64_FAR_RAW);

    assert_eq(INTR_SYNC,            ev.kind);
    assert_eq(INTR_ORIGIN_USER,     ev.origin);
    assert_eq(A64_SLOT_EL0_SYNC,    ev.native_vector);
    assert_eq(INTR_REASON_MEMORY,   ev.reason);
    assert_eq(INTR_ACCESS_READ,     ev.access);
    assert_eq(true,                 ev.fault_addr_valid);
    assert_eq(A64_FAR_RAW,          ev.fault_addr);

    /* An EL1h sync frame keeps the EL1h slot. */
    fill_regs(&regs, A64_MODE_EL1H);
    aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_DATA_SAME, 0x05u),
                             A64_FAR_RAW);
    assert_eq(A64_SLOT_SYNC, ev.native_vector);

    /* An SVC carries no FAR at all: the address is invalid and
     * zeroed, while the raw FAR stays in the diagnostic payload. */
    fill_regs(&regs, A64_MODE_EL0T);
    aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_SVC_LOWER, 0x4d40u),
                             A64_FAR_RAW);

    assert_eq(INTR_SYNC,            ev.kind);
    assert_eq(INTR_ORIGIN_USER,     ev.origin);
    assert_eq(A64_SLOT_EL0_SYNC,    ev.native_vector);
    assert_eq(INTR_REASON_OTHER,    ev.reason);
    assert_eq(INTR_ACCESS_UNKNOWN,  ev.access);
    assert_eq(false,                ev.fault_addr_valid);
    assert_eq(0,                    ev.fault_addr);
    assert_eq(A64_FAR_RAW,          ev.native_address);
}

/* ── Case: a64_far_validity ───────────────────────────────────── */

static void case_a64_far_validity(void)
{
    pt_regs_t regs;

    TEST_SUITE("a64_far_validity");

    /* FnV = 1: the normalized address is invalid and zeroed, while
     * the raw FAR survives in native_address. */
    {
        struct intr_event ev;
        uint64_t esr = mk_esr(A64_EC_DATA_LOWER, (1u << A64_ESR_ISS_FNV));

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        /* The FSC is 0 here, so the access stays conservative. */
        check_event(&ev, INTR_SYNC, INTR_ORIGIN_KERNEL, INTR_REASON_MEMORY,
                    INTR_ACCESS_UNKNOWN, false, 0, A64_SLOT_SYNC, esr,
                    A64_FAR_RAW);
    }

    /* FnV = 0: the FAR is the normalized fault address. */
    {
        struct intr_event ev;
        uint64_t esr = mk_esr(A64_EC_DATA_LOWER, 0);

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        assert_eq(true, ev.fault_addr_valid);
        assert_eq(A64_FAR_RAW, ev.fault_addr);
        assert_eq(A64_FAR_RAW, ev.native_address);
    }

    /* An instruction abort with FnV = 1 is equally invalid. */
    {
        struct intr_event ev;
        uint64_t esr = mk_esr(A64_EC_INSN_SAME, (1u << A64_ESR_ISS_FNV));

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        assert_eq(false, ev.fault_addr_valid);
        assert_eq(0, ev.fault_addr);
        assert_eq(A64_FAR_RAW, ev.native_address);
    }

    /* PC alignment does not follow FnV — the FAR stays valid. */
    {
        struct intr_event ev;
        uint64_t esr = mk_esr(A64_EC_PC_ALIGN, (1u << A64_ESR_ISS_FNV));

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        assert_eq(INTR_REASON_PROTECTION, ev.reason);
        assert_eq(true, ev.fault_addr_valid);
        assert_eq(A64_FAR_RAW, ev.fault_addr);
        assert_eq(A64_FAR_RAW, ev.native_address);
    }

    /* FAR == 0 is a valid address too — the validity bit decides. */
    {
        struct intr_event ev;
        uint64_t esr = mk_esr(A64_EC_DATA_LOWER, 0);

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_ZERO);

        assert_eq(true, ev.fault_addr_valid);
        assert_eq(A64_FAR_ZERO, ev.fault_addr);
        assert_eq(A64_FAR_ZERO, ev.native_address);
    }
}

/* ── Case: a64_abort_access ───────────────────────────────────── */

static void case_a64_abort_access(void)
{
    pt_regs_t regs;

    TEST_SUITE("a64_abort_access");

    /* An instruction abort is an instruction fetch. */
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_INSN_LOWER, 0),
                                 A64_FAR_RAW);
        assert_eq(INTR_REASON_MEMORY, ev.reason);
        assert_eq(INTR_ACCESS_EXEC, ev.access);
    }
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, mk_esr(A64_EC_INSN_SAME, 0),
                                 A64_FAR_RAW);
        assert_eq(INTR_REASON_MEMORY, ev.reason);
        assert_eq(INTR_ACCESS_EXEC, ev.access);
    }

    /* A data abort only yields READ/WRITE inside the FSC whitelist
     * with CM = 0 and S1PTW = 0. */
    for (unsigned i = 0; i < sizeof(k_a64_data_access_table) /
                            sizeof(k_a64_data_access_table[0]); i++) {
        const struct a64_data_access_expect *e = &k_a64_data_access_table[i];
        struct intr_event ev;
        unsigned iss = e->fsc | e->extra_iss;
        if (e->wnr) {
            iss |= (1u << A64_ESR_ISS_WNR);
        }
        uint64_t esr = mk_esr(A64_EC_DATA_SAME, iss);

        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs, esr, A64_FAR_RAW);

        assert_eq(INTR_REASON_MEMORY, ev.reason);
        assert_eq(e->access, ev.access);
    }

    /* WnR selects write, otherwise read. */
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs,
                                 mk_esr(A64_EC_DATA_SAME,
                                        0x05u | (1u << A64_ESR_ISS_WNR)),
                                 A64_FAR_RAW);
        assert_eq(INTR_ACCESS_WRITE, ev.access);
    }
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_sync(&ev, &regs,
                                 mk_esr(A64_EC_DATA_LOWER, 0x05u),
                                 A64_FAR_RAW);
        assert_eq(INTR_ACCESS_READ, ev.access);
    }
}

/* ── Case: a64_irq_fields ─────────────────────────────────────── */

static void case_a64_irq_fields(void)
{
    pt_regs_t regs;

    TEST_SUITE("a64_irq_fields");

    /* An IRQ carries no fault payload at all. */
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL1H);
        aarch64_intr_decode_irq(&ev, &regs);

        check_event(&ev, INTR_IRQ, INTR_ORIGIN_KERNEL, INTR_REASON_OTHER,
                    INTR_ACCESS_UNKNOWN, false, 0, A64_SLOT_IRQ, 0, 0);
    }

    /* The IRQ origin follows the saved SPSR mode as well. */
    {
        struct intr_event ev;
        fill_regs(&regs, A64_MODE_EL0T);
        aarch64_intr_decode_irq(&ev, &regs);
        assert_eq(INTR_ORIGIN_USER, ev.origin);
    }
    {
        struct intr_event ev;
        fill_regs(&regs, 0x04u);
        aarch64_intr_decode_irq(&ev, &regs);
        assert_eq(INTR_ORIGIN_UNKNOWN, ev.origin);
    }
}

/* ── Case: a64_uaccess_abort_match ────────────────────────────── */

/* The PURE formal EL1 copy-fixup predicate
 * (kernel/include/arch/aarch64/uaccess.h).  It compares INPUTS only:
 * the exception snapshot, the registered window and the live
 * task/mm/root/TTBR0 facts are all plain values here, so no case
 * fabricates hardware recovery evidence — every assertion is the
 * predicate's own boolean. */
#define A64_FIX_LOAD_PC   0x00000000A0001000ULL
#define A64_FIX_STORE_PC  0x00000000A0002000ULL
#define A64_FIX_WIN_LO    0x0000000010000000ULL
#define A64_FIX_WIN_HI    0x0000000010001000ULL
#define A64_FIX_FAR       0x0000000010000080ULL
#define A64_FIX_TASK      0x0000000000011100ULL
#define A64_FIX_MM        0x0000000000022200ULL
#define A64_FIX_ROOT      0x0000000000003000ULL

struct a64_fix_fixture {
    struct aarch64_uaccess_abort  ab;
    struct aarch64_uaccess_window win;
    struct aarch64_uaccess_live   live;
};

static void fix_fixture(struct a64_fix_fixture *f, bool write)
{
    uint64_t iss = 0x05ULL | (write ? (1u << A64_ESR_ISS_WNR) : 0);

    f->ab = (struct aarch64_uaccess_abort){
        .esr       = ((uint64_t)A64_EC_DATA_SAME << A64_ESR_EC_SHIFT) | iss,
        .spsr      = 0x3c5ULL,      /* EL1h with D/A/F/I set */
        .pc        = write ? A64_FIX_STORE_PC : A64_FIX_LOAD_PC,
        .far       = A64_FIX_FAR,
        .read_ptr  = A64_FIX_FAR,   /* x1 at the LDTRB */
        .write_ptr = A64_FIX_FAR,   /* x0 at the STTRB */
    };
    f->win = (struct aarch64_uaccess_window){
        .present = true,
        .write   = write,
        .lo      = A64_FIX_WIN_LO,
        .hi      = A64_FIX_WIN_HI,
        .task    = (const void *)(uintptr_t)A64_FIX_TASK,
        .mm      = (const void *)(uintptr_t)A64_FIX_MM,
        .root_pa = A64_FIX_ROOT,
    };
    f->live = (struct aarch64_uaccess_live){
        .task     = (const void *)(uintptr_t)A64_FIX_TASK,
        .mm       = (const void *)(uintptr_t)A64_FIX_MM,
        .root_pa  = A64_FIX_ROOT,
        .ttbr0_pa = A64_FIX_ROOT,
    };
}

static bool fix_matches(const struct a64_fix_fixture *f)
{
    return aarch64_uaccess_abort_matches(&f->ab, &f->win, &f->live,
                                         A64_FIX_LOAD_PC, A64_FIX_STORE_PC);
}

static void case_a64_uaccess_abort_match(void)
{
    struct a64_fix_fixture f;

    TEST_SUITE("a64_uaccess_abort_match");

    /* The two positive shapes: a recoverable read and a recoverable
     * write abort of the exact unprivileged instruction, at the exact
     * user pointer, inside the registered window. */
    fix_fixture(&f, false);
    assert_true(fix_matches(&f));
    fix_fixture(&f, true);
    assert_true(fix_matches(&f));

    /* The complete M[4:0] must be EL1h.  Extra PSTATE bits are
     * allowed; a different mode is not. */
    fix_fixture(&f, false);
    f.ab.spsr = 0x05ULL;
    assert_true(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.spsr = 0x00ULL;                    /* EL0t */
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.spsr = 0x04ULL;                    /* EL1t */
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.spsr = 0x0dULL;                    /* SVC/other mode */
    assert_false(fix_matches(&f));

    /* EC: only the same-EL data abort is a copy fault. */
    fix_fixture(&f, false);
    f.ab.esr = ((uint64_t)A64_EC_DATA_LOWER << A64_ESR_EC_SHIFT) | 0x05ULL;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr = ((uint64_t)A64_EC_INSN_SAME << A64_ESR_EC_SHIFT) | 0x05ULL;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr = ((uint64_t)A64_EC_SVC_LOWER << A64_ESR_EC_SHIFT);
    assert_false(fix_matches(&f));

    /* FSC outside translation/access/permission faults. */
    fix_fixture(&f, false);
    f.ab.esr = ((uint64_t)A64_EC_DATA_SAME << A64_ESR_EC_SHIFT) | 0x03ULL;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr = ((uint64_t)A64_EC_DATA_SAME << A64_ESR_EC_SHIFT) | 0x10ULL;
    assert_false(fix_matches(&f));

    /* Real syndrome bits: S1PTW (bit 7), CM (bit 8) and FnV (bit 10)
     * each make the abort unrecoverable on their own. */
    fix_fixture(&f, false);
    f.ab.esr |= (1u << A64_ESR_ISS_S1PTW);
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr |= (1u << A64_ESR_ISS_CM);
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr |= (1u << A64_ESR_ISS_FNV);
    assert_false(fix_matches(&f));

    /* WnR must agree with the registered direction. */
    fix_fixture(&f, true);
    f.ab.esr &= ~(1u << A64_ESR_ISS_WNR);         /* read abort, write window */
    f.ab.pc = A64_FIX_LOAD_PC;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.esr |= (1u << A64_ESR_ISS_WNR);          /* write abort, read window */
    f.ab.pc = A64_FIX_STORE_PC;
    assert_false(fix_matches(&f));

    /* The faulting PC must be exactly the direction's user instruction. */
    fix_fixture(&f, false);
    f.ab.pc = A64_FIX_STORE_PC;
    assert_false(fix_matches(&f));
    fix_fixture(&f, true);
    f.ab.pc = A64_FIX_LOAD_PC;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.pc += 4;
    assert_false(fix_matches(&f));

    /* FAR must equal the user pointer the instruction still holds. */
    fix_fixture(&f, false);
    f.ab.far = A64_FIX_FAR + 1;
    assert_false(fix_matches(&f));
    fix_fixture(&f, true);
    f.ab.far = A64_FIX_FAR + 1;
    assert_false(fix_matches(&f));

    /* FAR must lie inside the registered half-open window. */
    fix_fixture(&f, false);
    f.ab.far = A64_FIX_WIN_LO - 1;
    f.ab.read_ptr = f.ab.far;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.ab.far = A64_FIX_WIN_HI;
    f.ab.read_ptr = f.ab.far;
    assert_false(fix_matches(&f));

    /* No window: the fixup never invents one. */
    fix_fixture(&f, false);
    f.win.present = false;
    assert_false(fix_matches(&f));

    /* The window's own identity fields must be present. */
    fix_fixture(&f, false);
    f.win.task = NULL;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.win.mm = NULL;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.win.root_pa = 0;
    assert_false(fix_matches(&f));

    /* A different task, mm or root — live vs registered — is never a
     * match.  Both the mm root AND the actual TTBR0 must agree. */
    fix_fixture(&f, false);
    f.live.task = (const void *)(uintptr_t)(A64_FIX_TASK + 0x100);
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.live.mm = (const void *)(uintptr_t)(A64_FIX_MM + 0x100);
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.live.root_pa = A64_FIX_ROOT + 0x1000;
    assert_false(fix_matches(&f));
    fix_fixture(&f, false);
    f.live.ttbr0_pa = A64_FIX_ROOT + 0x1000;
    assert_false(fix_matches(&f));
}

int main(void)
{
    case_a64_modes_and_classes();
    case_a64_el0_slots();
    case_a64_far_validity();
    case_a64_abort_access();
    case_a64_irq_fields();
    case_a64_uaccess_abort_match();
    TEST_RESULTS();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_intr_decode_aarch64",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

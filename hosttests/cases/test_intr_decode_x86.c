/* hosttests/cases/test_intr_decode_x86.c
 *
 * x86_64 normalized interrupt-event decoder (dispatch phase 1, Task 1).
 *
 * Links the PRODUCTION decoder
 *   kernel/arch/x86_64/intr/intr_decode.c :: x86_intr_decode()
 * and checks it against independent expectation tables written here.
 * No expected value is derived from calling the decoder or from any
 * shared helper: the tables below are literal constants of the
 * contract (spec §5.1 event shape, §6.1 x86 decoding rules).
 *
 * The event struct has padding, so every comparison is member by
 * member — never memcmp.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/regs.h>
#include <intr/dispatch.h>
#include <arch/x86_64/intr_decode.h>

/* ── Independent expectation tables ────────────────────────────── */

/* spec §6.1: vector → reason. */
struct x86_vector_expect {
    uint64_t          vector;
    enum intr_reason  reason;
    const char       *name;
};

static const struct x86_vector_expect k_x86_vector_table[] = {
    {  0, INTR_REASON_ARITHMETIC, "#DE divide error"       },
    {  6, INTR_REASON_ILLEGAL,    "#UD undefined opcode"   },
    { 13, INTR_REASON_PROTECTION, "#GP general protection" },
    { 14, INTR_REASON_MEMORY,     "#PF page fault"         },
};

/* spec §6.1: saved CS privilege level → origin.  The RPL bits are
 * the low two bits of the selector. */
struct x86_cpl_expect {
    uint64_t         cs;
    enum intr_origin origin;
    const char      *name;
};

static const struct x86_cpl_expect k_x86_cpl_table[] = {
    { 0x08, INTR_ORIGIN_KERNEL,  "CPL 0 (kernel CS)"  },
    { 0x2B, INTR_ORIGIN_USER,    "CPL 3 (user CS)"    },
    { 0x09, INTR_ORIGIN_UNKNOWN, "CPL 1"              },
    { 0x0A, INTR_ORIGIN_UNKNOWN, "CPL 2"              },
};

/* spec §6.1: #PF error code → access.  Bit 4 = I/D (instruction
 * fetch), bit 1 = W/R (write).  I/D wins over W/R. */
struct x86_pf_access_expect {
    uint64_t        error_code;
    enum intr_access access;
    const char     *name;
};

static const struct x86_pf_access_expect k_x86_pf_access_table[] = {
    { 0x00, INTR_ACCESS_READ,  "data, read"          },
    { 0x02, INTR_ACCESS_WRITE, "data, write"         },
    { 0x10, INTR_ACCESS_EXEC,  "instruction fetch"   },
    { 0x12, INTR_ACCESS_EXEC,  "I/D wins over W/R"   },
};

/* Constants of the contract, spelled out here so the test does not
 * depend on any production decoder symbol. */
#define X86_VEC_DE  0ULL
#define X86_VEC_UD  6ULL
#define X86_VEC_GP  13ULL
#define X86_VEC_PF  14ULL

#define X86_CS_KERNEL 0x08ULL
#define X86_CS_USER   0x2BULL

#define X86_CR2_ZERO      0ULL
#define X86_CR2_SOME      0x0000123456789000ULL
#define X86_ERRCODE_ZERO  0ULL

/* ── Helpers ───────────────────────────────────────────────────── */

static void fill_regs(pt_regs_t *regs, uint64_t cs)
{
    for (unsigned i = 0; i < sizeof(*regs) / sizeof(uint64_t); i++) {
        ((uint64_t *)regs)[i] = 0;
    }
    regs->cs = cs;
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

/* ── Case: x86_vectors_and_cpl ────────────────────────────────── */

static void case_x86_vectors_and_cpl(void)
{
    pt_regs_t regs;

    TEST_SUITE("x86_vectors_and_cpl");

    /* Every migrated vector maps to its own reason; the non-#PF
     * vectors carry no address at all. */
    for (unsigned i = 0; i < sizeof(k_x86_vector_table) /
                            sizeof(k_x86_vector_table[0]); i++) {
        const struct x86_vector_expect *e = &k_x86_vector_table[i];
        struct intr_event ev;

        fill_regs(&regs, X86_CS_KERNEL);
        x86_intr_decode(&ev, &regs, e->vector, X86_ERRCODE_ZERO,
                        X86_CR2_SOME);

        if (e->vector == X86_VEC_PF) {
            /* #PF is the only vector that owns an address. */
            check_event(&ev, INTR_SYNC, INTR_ORIGIN_KERNEL,
                        INTR_REASON_MEMORY, INTR_ACCESS_READ, true,
                        X86_CR2_SOME, X86_VEC_PF, X86_ERRCODE_ZERO,
                        X86_CR2_SOME);
        } else {
            check_event(&ev, INTR_SYNC, INTR_ORIGIN_KERNEL, e->reason,
                        INTR_ACCESS_UNKNOWN, false, 0, e->vector,
                        X86_ERRCODE_ZERO, 0);
        }
    }

    /* An unmigrated vector is conservative: OTHER, no address. */
    {
        struct intr_event ev;
        fill_regs(&regs, X86_CS_KERNEL);
        x86_intr_decode(&ev, &regs, 21ULL, X86_ERRCODE_ZERO, X86_CR2_SOME);
        check_event(&ev, INTR_SYNC, INTR_ORIGIN_KERNEL, INTR_REASON_OTHER,
                    INTR_ACCESS_UNKNOWN, false, 0, 21ULL, X86_ERRCODE_ZERO,
                    0);
    }

    /* The saved CS privilege level — not the error code — decides the
     * origin. */
    for (unsigned i = 0; i < sizeof(k_x86_cpl_table) /
                            sizeof(k_x86_cpl_table[0]); i++) {
        const struct x86_cpl_expect *e = &k_x86_cpl_table[i];
        struct intr_event ev;

        fill_regs(&regs, e->cs);
        x86_intr_decode(&ev, &regs, X86_VEC_DE, X86_ERRCODE_ZERO,
                        X86_CR2_ZERO);

        assert_eq(INTR_SYNC, ev.kind);
        assert_eq(e->origin, ev.origin);
        assert_eq(INTR_REASON_ARITHMETIC, ev.reason);
    }
}

/* ── Case: x86_pf_access_and_zero ────────────────────────────── */

static void case_x86_pf_access_and_zero(void)
{
    pt_regs_t regs;

    TEST_SUITE("x86_pf_access_and_zero");

    /* I/D beats W/R; otherwise W/R picks write, else read. */
    for (unsigned i = 0; i < sizeof(k_x86_pf_access_table) /
                            sizeof(k_x86_pf_access_table[0]); i++) {
        const struct x86_pf_access_expect *e = &k_x86_pf_access_table[i];
        struct intr_event ev;

        fill_regs(&regs, X86_CS_USER);
        x86_intr_decode(&ev, &regs, X86_VEC_PF, e->error_code,
                        X86_CR2_SOME);

        assert_eq(INTR_SYNC, ev.kind);
        assert_eq(INTR_ORIGIN_USER, ev.origin);
        assert_eq(INTR_REASON_MEMORY, ev.reason);
        assert_eq(e->access, ev.access);
        assert_eq(true, ev.fault_addr_valid);
        assert_eq(X86_CR2_SOME, ev.fault_addr);
        assert_eq(X86_VEC_PF, ev.native_vector);
        assert_eq(e->error_code, ev.native_status);
        assert_eq(X86_CR2_SOME, ev.native_address);
    }

    /* CR2 == 0 is still a VALID fault address — the validity bit,
     * not the numeric value, carries the information. */
    {
        struct intr_event ev;
        fill_regs(&regs, X86_CS_KERNEL);
        x86_intr_decode(&ev, &regs, X86_VEC_PF, X86_ERRCODE_ZERO,
                        X86_CR2_ZERO);
        assert_eq(true, ev.fault_addr_valid);
        assert_eq(X86_CR2_ZERO, ev.fault_addr);
        assert_eq(X86_CR2_ZERO, ev.native_address);
    }

    /* A non-#PF vector never borrows CR2 as an address. */
    {
        struct intr_event ev;
        fill_regs(&regs, X86_CS_KERNEL);
        x86_intr_decode(&ev, &regs, X86_VEC_GP, X86_ERRCODE_ZERO,
                        X86_CR2_SOME);
        assert_eq(INTR_REASON_PROTECTION, ev.reason);
        assert_eq(false, ev.fault_addr_valid);
        assert_eq(0, ev.fault_addr);
        assert_eq(0, ev.native_address);
        assert_eq(X86_VEC_GP, ev.native_vector);
    }
}

int main(void)
{
    case_x86_vectors_and_cpl();
    case_x86_pf_access_and_zero();
    TEST_RESULTS();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_intr_decode_x86",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}

/* Controlled host coverage for the scheduled x86 IRQ probe decisions.
 *
 * The guest probe (kernel/selftest/test_x86_intr.c) routes every one of
 * its success/failure choices through the pure helpers in
 * kernel/include/selftest/x86_intr.h.  This fixture exercises those
 * exact decisions: runtime IDT gate decoding (target/selector/type/
 * DPL/IST/reserved), sentinel snapshot comparison, bounded-wait timeout
 * vs completion, cross-CPU target/ack accounting, the live tick/resched
 * verdicts and the post-cleanup worker verdict.  The guest-only
 * machinery (inline-asm sentinel, remote scratch-CR3 worker, real IPIs)
 * is verified by the SMP2/SMP4 kernel-selftest runs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <selftest/x86_intr.h>

/* Encode a 16-byte x86_64 IDT descriptor the way the kernel's
 * _set_gate does: offset low[0..1], selector[2], zero[3], IST[4],
 * type/attr[5], offset mid[6..7], offset high[8..11], reserved
 * [12..15].  A wrongly encoded gate below is a stand-in for a runtime
 * IDT the probe must reject. */
static void encode_gate(unsigned char g[16], uint64_t addr,
                        unsigned char attr, unsigned char ist)
{
    memset(g, 0, 16);
    g[0]  = (unsigned char)(addr >> 0);
    g[1]  = (unsigned char)(addr >> 8);
    g[2]  = 8;                       /* kernel text selector */
    g[4]  = ist;
    g[5]  = attr;
    g[6]  = (unsigned char)(addr >> 16);
    g[7]  = (unsigned char)(addr >> 24);
    g[8]  = (unsigned char)(addr >> 32);
    g[9]  = (unsigned char)(addr >> 40);
    g[10] = (unsigned char)(addr >> 48);
    g[11] = (unsigned char)(addr >> 56);
}

static void test_gate_mismatch(void)
{
    /* A -mcmodel=large kernel stub address: high bytes in [8..11]. */
    const uint64_t stub = 0xffff800000201234ULL;
    unsigned char g[16];

    encode_gate(g, stub, 0x8e, 0);
    assert(x86_intr_gate_mismatch(g, stub, 0x8e, 0) == NULL);

    /* Stub moved (e.g. wrong entry installed): named "target".  The
     * last two cases keep the low 24 bits identical, so a decoder
     * that ignores the high offset bytes cannot pass. */
    encode_gate(g, stub + 0x10, 0x8e, 0);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "target"));
    encode_gate(g, stub ^ (1ULL << 40), 0x8e, 0);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "target"));

    /* Wrong CS selector. */
    encode_gate(g, stub, 0x8e, 0);
    g[2] = 0x10;
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "selector"));

    /* Type/attr drift: interrupt vs trap gate, and DPL drift (a DPL3
     * system gate where the kernel must have installed DPL0). */
    encode_gate(g, stub, 0x8f, 0);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "attr"));
    encode_gate(g, stub, 0xef, 1);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8f, 1), "attr"));

    /* IST drift (#DF must stay IST3, #PF IST0, others IST1). */
    encode_gate(g, stub, 0x8e, 0);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 3), "ist"));
    encode_gate(g, stub, 0x8f, 3);
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8f, 0), "ist"));

    /* Reserved bytes must be zero: byte 3 and the upper dword. */
    encode_gate(g, stub, 0x8e, 0);
    g[3] = 1;
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "rsvd3"));
    encode_gate(g, stub, 0x8e, 0);
    g[12] = 1;
    assert(!strcmp(x86_intr_gate_mismatch(g, stub, 0x8e, 0), "rsvd"));
}

static void test_snapshot_equal(void)
{
    struct x86_intr_snapshot a, b;
    for (unsigned i = 0; i < 18; ++i)
        a.word[i] = b.word[i] = 0x12340000 + i;
    assert(x86_intr_snapshot_equal(&a, &b));
    /* Every word carries a distinct sentinel: GPRs, RSP, RFLAGS and
     * the packed DS/ES pair must each be restored exactly. */
    for (unsigned i = 0; i < 18; ++i) {
        b.word[i] ^= 1;
        assert(!x86_intr_snapshot_equal(&a, &b));
        b.word[i] ^= 1;
    }
}

static void test_wait_state(void)
{
    /* Waiting / timed out / completed against a deadline budget. */
    assert(x86_intr_wait_state(0, 109, 100, 10) == 0);
    assert(x86_intr_wait_state(0, 110, 100, 10) == -1);
    assert(x86_intr_wait_state(1, 110, 100, 10) == 1);
    /* Completion wins over an expired budget. */
    assert(x86_intr_wait_state(1, 500, 100, 10) == 1);
    /* Cycle-counter wraparound must still expire. */
    assert(x86_intr_wait_state(0, 3, UINT64_MAX - 5, 9) == -1);
}

static void test_ack_matches(void)
{
    /* The remote CPU observed is the one targeted, its ack generation
     * advanced by exactly one, the observer fired inside this
     * shootdown (not a stale one) and the post-flush translation read
     * the flipped page. */
    assert(x86_intr_ack_matches(2, 2, 7, 8, 7, X86_INTR_FLUSHED_VALUE));
    /* Generation wraparound is an equality protocol, not ordering. */
    assert(x86_intr_ack_matches(2, 2, UINT32_MAX, 0, UINT32_MAX,
                                X86_INTR_FLUSHED_VALUE));
    /* Ack from the wrong CPU. */
    assert(!x86_intr_ack_matches(2, 1, 7, 8, 7, X86_INTR_FLUSHED_VALUE));
    /* No advance: the target never flushed. */
    assert(!x86_intr_ack_matches(2, 2, 7, 7, 7, X86_INTR_FLUSHED_VALUE));
    /* Double advance: a second interleaved shootdown over-counted. */
    assert(!x86_intr_ack_matches(2, 2, 7, 9, 7, X86_INTR_FLUSHED_VALUE));
    /* Observer fired in a different (stale) shootdown window. */
    assert(!x86_intr_ack_matches(2, 2, 7, 8, 6, X86_INTR_FLUSHED_VALUE));
    /* Stale TLB: the remote read the pre-flush page. */
    assert(!x86_intr_ack_matches(2, 2, 7, 8, 7, X86_INTR_PRE_FLUSH_VALUE));
}

static void test_spurious_verdict(void)
{
    /* All preserved: snapshot, no ret_from_intr return work, no
     * schedule, need_resched not consumed. */
    assert(x86_intr_spurious_verdict(true, 5, 5, 9, 9, 1));
    /* Any drift fails the probe. */
    assert(!x86_intr_spurious_verdict(false, 5, 5, 9, 9, 1));
    /* The int $0xff wrongly routed through ret_from_intr. */
    assert(!x86_intr_spurious_verdict(true, 5, 6, 9, 9, 1));
    /* Return work consumed need_resched via do_resched. */
    assert(!x86_intr_spurious_verdict(true, 5, 5, 9, 9, 0));
    /* A schedule ran inside the return path. */
    assert(!x86_intr_spurious_verdict(true, 5, 5, 9, 10, 1));
}

static void test_live_verdicts(void)
{
    /* progress_done gates the BSP's bounded wait loop. */
    assert(x86_intr_progress_done(11, 10, 4, 3));
    assert(!x86_intr_progress_done(10, 10, 4, 3));   /* BSP tick stalled */
    assert(!x86_intr_progress_done(11, 10, 3, 3));   /* AP tick stalled  */

    /* live_verdict additionally requires the global clock (BSP tick
     * handler) to have advanced and post-IPI task progress. */
    assert(x86_intr_live_verdict(11, 10, 4, 3, 101, 100, 1));
    assert(!x86_intr_live_verdict(10, 10, 4, 3, 101, 100, 1));
    assert(!x86_intr_live_verdict(11, 10, 3, 3, 101, 100, 1));
    /* AP ticks counted but jiffies never moved: synthetic advance. */
    assert(!x86_intr_live_verdict(11, 10, 4, 3, 100, 100, 1));
    /* No task-context progress after the resched IPI return. */
    assert(!x86_intr_live_verdict(11, 10, 4, 3, 101, 100, 0));
}

static void test_worker_verdict(void)
{
    /* The remote worker released cleanly and observed both timer
     * ticks and resched receipts after arming. */
    assert(x86_intr_worker_verdict(0, 4, 9, 1, 2));
    /* Worker self-reported failure (wrong initial mapping / timeout). */
    assert(!x86_intr_worker_verdict(-1, 4, 9, 1, 2));
    /* No timer tick reached the worker while armed. */
    assert(!x86_intr_worker_verdict(0, 4, 4, 1, 2));
    /* The resched IPI never arrived. */
    assert(!x86_intr_worker_verdict(0, 4, 9, 2, 2));
}

int main(void)
{
    test_gate_mismatch();
    test_snapshot_equal();
    test_wait_state();
    test_ack_matches();
    test_spurious_verdict();
    test_live_verdicts();
    test_worker_verdict();
    puts("x86_intr probe decisions: PASS");
    return 0;
}

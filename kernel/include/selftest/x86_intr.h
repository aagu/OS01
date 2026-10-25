#ifndef SELFTEST_X86_INTR_H
#define SELFTEST_X86_INTR_H
#include <stdint.h>
#include <stdbool.h>

/* Pure decision helpers for the scheduled x86 IRQ probe.  Shared
 * verbatim by the guest (kernel/selftest/test_x86_intr.c) and the host
 * fixture (hosttests/cases/test_x86_intr_probe.c): every success and
 * failure verdict the guest reaches at runtime is made here, so the
 * host fixture exercises the exact decisions, not a restatement. */

/* 15 GPRs, RSP, RFLAGS, packed DS/ES. Layout shared with sentinel asm. */
struct x86_intr_snapshot { uint64_t word[18]; };

static inline bool x86_intr_snapshot_equal(const struct x86_intr_snapshot *a,
                                           const struct x86_intr_snapshot *b)
{
    for (unsigned i = 0; i < 18; ++i)
        if (a->word[i] != b->word[i]) return false;
    return true;
}

/* Decode a runtime 16-byte x86_64 IDT descriptor and compare it with
 * the independently expected stub address, type/attr byte and IST.
 * Returns the first mismatch reason, or NULL when the gate is exactly
 * as expected (kernel CS selector, reserved bytes zero). */
static inline const char *x86_intr_gate_mismatch(const unsigned char gate[16],
                                                 uint64_t target,
                                                 unsigned char attr,
                                                 unsigned char ist)
{
    uint64_t address = (uint64_t)gate[0] | ((uint64_t)gate[1] << 8) |
                       ((uint64_t)gate[6] << 16) | ((uint64_t)gate[7] << 24) |
                       ((uint64_t)gate[8] << 32) | ((uint64_t)gate[9] << 40) |
                       ((uint64_t)gate[10] << 48) | ((uint64_t)gate[11] << 56);
    if (address != target) return "target";
    if (gate[2] != 8) return "selector";
    if (gate[3] != 0) return "rsvd3";
    if (gate[4] != ist) return "ist";
    if (gate[5] != attr) return "attr";
    if (gate[12] | gate[13] | gate[14] | gate[15]) return "rsvd";
    return 0;   /* no mismatch (plain 0: kernel freestanding has no NULL) */
}

/* 0 waiting, 1 completed, -1 timed out; unsigned elapsed handles wrap. */
static inline int x86_intr_wait_state(int done, uint64_t now,
                                      uint64_t start, uint64_t budget)
{
    return now - start >= budget ? -1 : (done ? 1 : 0);
}

/* The two sentinel values written into the scratch pages: the remote
 * worker must read PRE_FLUSH through the initial mapping, and the
 * shootdown observer must read FLUSHED through the flipped PTE (a
 * stale TLB still answers PRE_FLUSH). */
#define X86_INTR_PRE_FLUSH_VALUE 0x1111ULL
#define X86_INTR_FLUSHED_VALUE   0x2222ULL

/* Cross-CPU shootdown accounting: the observer fired on the targeted
 * CPU, inside this shootdown's generation window (stale generations
 * rejected), the ack generation advanced by exactly one (equality,
 * uint32 wraparound-safe) and the remote read the flipped page. */
static inline bool x86_intr_ack_matches(unsigned target, unsigned observed,
                                        uint32_t gen_before, uint32_t ack_after,
                                        uint32_t observed_gen, uint64_t value)
{
    return observed == target && ack_after == (uint32_t)(gen_before + 1) &&
           observed_gen == gen_before && value == X86_INTR_FLUSHED_VALUE;
}

/* int $0xff verdict: registers restored exactly, the spurious path ran
 * no ret_from_intr return work (per-CPU counter), no schedule() and
 * need_resched was not consumed by do_resched. */
static inline bool x86_intr_spurious_verdict(bool snapshot_equal,
                                             uint64_t returns_before,
                                             uint64_t returns_after,
                                             uint64_t schedules_before,
                                             uint64_t schedules_after,
                                             uint64_t need_resched)
{
    return snapshot_equal && returns_after == returns_before &&
           schedules_after == schedules_before && need_resched == 1;
}

/* Final live verdict: real ticks on both CPUs, the global clock (BSP
 * tick_handler drives jiffies) advanced, and the remote task made
 * progress after the resched IPI's interrupt return. */
static inline bool x86_intr_live_verdict(uint64_t bsp_ticks, uint64_t bsp_base,
                                         uint64_t ap_ticks, uint64_t ap_base,
                                         uint64_t jiffies_now,
                                         uint64_t jiffies_base,
                                         uint64_t progress)
{
    return bsp_ticks > bsp_base && ap_ticks > ap_base &&
           jiffies_now > jiffies_base && progress > 0;
}

/* The bounded wait and final decision share every asynchronous condition.
 * Receipt alone is insufficient: progress is acquired from task context
 * after the remote interrupt returns. */
static inline bool x86_intr_progress_done(uint64_t bsp_ticks, uint64_t bsp_base,
                                          uint64_t ap_ticks, uint64_t ap_base,
                                          uint64_t clock, uint64_t clock_base,
                                          uint64_t receipts, uint64_t receipt_base,
                                          uint64_t progress)
{
    return receipts > receipt_base &&
           x86_intr_live_verdict(bsp_ticks, bsp_base, ap_ticks, ap_base,
                                 clock, clock_base, progress);
}

/* Post-cleanup remote-worker verdict: released cleanly (no timeout, no
 * wrong initial mapping) and it observed both timer ticks and resched
 * receipts while armed. */
static inline bool x86_intr_worker_verdict(int result,
                                           uint64_t ticks_before,
                                           uint64_t ticks_after,
                                           uint64_t resched_before,
                                           uint64_t resched_after)
{
    return result == 0 && ticks_after > ticks_before &&
           resched_after > resched_before;
}

#ifdef OS01_SELFTEST
int test_x86_intr(void);
void selftest_x86_intr_tick(unsigned cpu);
void selftest_x86_intr_tlb(unsigned cpu);
void selftest_x86_intr_resched(unsigned cpu);
#endif
#endif

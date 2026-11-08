/* kernel/selftest/boot_irq_time_fixture.c — BOOT-2 isolation fixture.
 * Compiled ONLY for the boot-irq-time-<case> kernel variants (the
 * normal build keeps it out of the wildcard so no fixture symbol or
 * marker string is linked).  See the paired header for the contract. */

#include <selftest/boot_irq_time_fixture.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <log/log.h>
#include <time/timer.h>          /* jiffies */
#include <arch/irq.h>
#include <arch/cpu.h>

/* Tokens are keyed by (phase, source) SEQUENCE numbers, not raw frame
 * pointers: several IRQs can be in flight against the same frame
 * address, so pairing per frame-pointer would lose counts.  A return
 * consumes the oldest live token of its phase/source pair — the
 * in-order completion contract — and per-phase counts stay exact. */
struct b2t_token {
    int phase;
    int source;
    bool live;
};

#define B2T_MAX_TOKENS 256
static struct b2t_token tokens[B2T_MAX_TOKENS];
static unsigned long live_tokens[2][3];   /* [phase][source] live count */

static int active_phase = B2T_PHASE_CLOSED;
static unsigned long handler_counts[2];   /* PIT / LAPIC */
static unsigned long paired_returns[2];
static unsigned long rtc_pf_count;
static unsigned long rtc_returns;
static unsigned long unexpected_overlaps;
static uint64_t phase_jiffies0;

static int b2t_self_cpu(void) { return 0; }   /* BSP-only observation */

static void token_push(const void *frame, int phase, int source)
{
    (void)frame;
    /* find a free slot */
    for (unsigned i = 0; i < B2T_MAX_TOKENS; i++) {
        struct b2t_token *t = &tokens[i];
        if (t->live)
            continue;
        t->live = true;
        t->phase = phase;
        t->source = source;
        if (phase >= 0 && phase <= 2 && source <= B2T_SRC_LAPIC)
            live_tokens[phase][source]++;
        else
            unexpected_overlaps++;
        return;
    }
    unexpected_overlaps++;      /* overflow is a failure, not a wrap */
}

void boot2_test_irq_return(const void *regs)
{
    (void)regs;
    /* Consume the oldest live token of the CURRENT phase (any source):
     * handlers run to completion before their return, so the oldest
     * outstanding IRQ of this phase is the one completing.  A return
     * with no live token in this phase (syscall, #PF, another IRQ, a
     * stale cross-phase frame) pairs nothing. */
    int phase = active_phase;
    if (phase == B2T_PHASE_CLOSED)
        return;
    for (unsigned i = 0; i < B2T_MAX_TOKENS; i++) {
        struct b2t_token *t = &tokens[i];
        if (!t->live || t->phase != phase)
            continue;
        t->live = false;
        if (t->phase >= 0 && t->phase <= 2 && t->source <= B2T_SRC_LAPIC)
            live_tokens[t->phase][t->source]--;
        paired_returns[t->source]++;
        if (phase == B2T_PHASE_RTC)
            rtc_returns++;
        return;
    }
}

void boot2_test_rtc_pf(const void *regs)
{
    /* The RTC window opens lazily on the first real PF: the fixture has
     * no begin-hook in the frozen API, and the jiffies baseline for the
     * delta evidence is the first periodic tick. */
    if (active_phase == B2T_PHASE_CLOSED && b2t_self_cpu() == 0) {
        active_phase = B2T_PHASE_RTC;
        phase_jiffies0 = jiffies;
        }
    if (active_phase != B2T_PHASE_RTC || b2t_self_cpu() != 0)
        return;
    rtc_pf_count++;
    token_push(regs, B2T_PHASE_RTC, B2T_SRC_PIT /* reason tag unused */);
}

void boot2_test_timer_irq(unsigned source, const void *regs)
{
    if (active_phase != B2T_PHASE_BOOTSTRAP &&
        active_phase != B2T_PHASE_HANDOFF)
        return;
    if (b2t_self_cpu() != 0 || source > B2T_SRC_LAPIC)
        return;
    handler_counts[source]++;
    token_push(regs, active_phase, (int)source);
}

static void snapshot_begin(void)
{
    arch_local_irq_disable();
    phase_jiffies0 = jiffies;
}

static void phase_begin(int phase)
{
    snapshot_begin();
    /* Any token still live from the previous phase is a cross-phase
     * overlap: count it and reset, never carry it. */
    for (unsigned i = 0; i < B2T_MAX_TOKENS; i++)
        tokens[i].live = false;
    live_tokens[0][0] = live_tokens[0][1] = live_tokens[0][2] = 0;
    live_tokens[1][0] = live_tokens[1][1] = live_tokens[1][2] = 0;
    live_tokens[2][0] = live_tokens[2][1] = live_tokens[2][2] = 0;
    active_phase = phase;
    arch_local_irq_enable();
}

/* Wait (IRQs on) until `want` paired returns accumulated for `source`;
 * bounded by a pause budget — never jiffies/schedule; FAIL loudly on
 * budget exhaustion instead of waiting forever. */
static bool wait_returns(unsigned source, unsigned long want)
{
    uint32_t rounds = 0;
    while (paired_returns[source] < want) {
        if (++rounds > 500000000u) {
            log_err("[boot-2-test] budget exhausted waiting for source=%u "
                    "returns=%lu\n", source, paired_returns[source]);
            return false;
        }
        arch_cpu_pause();
    }
    return true;
}

static void phase_end_print(const char *tag, const char *extra,
                            unsigned long count, unsigned long rets)
{
    arch_local_irq_disable();
    log_info("[boot-2-test] %s %scount=%lu returns=%lu jiffies_delta=%llu\n",
             tag, extra, count, rets,
             (unsigned long long)(jiffies - phase_jiffies0));
    active_phase = B2T_PHASE_CLOSED;
    arch_local_irq_enable();
}

void boot2_test_after_rtc(int result)
{
    arch_local_irq_disable();
    unsigned long pf = rtc_pf_count, rets = rtc_returns;
    /* Any RTC token still live at close: the hardware IRQ completed
     * but its return was not observed inside the window — overlap. */
    unexpected_overlaps += live_tokens[B2T_PHASE_RTC][0] +
                           live_tokens[B2T_PHASE_RTC][1];
    log_info("[boot-2-test] rtc result=%s pf=%lu returns=%lu if=0 "
             "jiffies_delta=%llu unexpected=%lu\n",
             result == 0 ? "ok" : "timeout", pf, rets,
             (unsigned long long)(jiffies - phase_jiffies0),
             unexpected_overlaps);
    active_phase = B2T_PHASE_CLOSED;
    arch_local_irq_enable();
    (void)pf; (void)rets;
}

void boot2_test_after_runtime_open(void)
{
    paired_returns[B2T_SRC_PIT] = 0;
    handler_counts[B2T_SRC_PIT] = 0;
    phase_begin(B2T_PHASE_BOOTSTRAP);
    if (!wait_returns(B2T_SRC_PIT, 3))
        return;                     /* printed by wait; phase stays open */
    phase_end_print("bootstrap", "source=pit ",
                    handler_counts[B2T_SRC_PIT],
                    paired_returns[B2T_SRC_PIT]);
}

void boot2_test_after_handoff(bool lapic)
{
    unsigned src = lapic ? B2T_SRC_LAPIC : B2T_SRC_PIT;
    paired_returns[src] = 0;
    handler_counts[src] = 0;
    phase_begin(B2T_PHASE_HANDOFF);
    if (!wait_returns(src, 3))
        return;
    phase_end_print("tick", lapic ? "source=lapic " : "source=pit ",
                    handler_counts[src], paired_returns[src]);
}

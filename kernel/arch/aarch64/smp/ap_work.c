/* kernel/arch/aarch64/smp/ap_work.c — M3 AP work-item protocol (spec §6.2).
 * Contract in <arch/aarch64/ap_work.h>.
 *
 * NOTE on atomics: this file uses the __atomic builtins on the
 * _Atomic-qualified slot fields instead of <stdatomic.h> — the
 * freestanding aarch64 build has no usable <stdatomic.h> (same
 * constraint as memory/vmm_gate.c), and the builtins give identical
 * acquire/release semantics. */

#include <stdint.h>
#include <stdbool.h>

#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/boot_log.h>  /* kputs/kputu */
#include <arch/cpu.h>               /* arch_cpu_halt, arch_cycle_counter,
                                     * arch_cpu_pause */

struct ap_work ap_work[NR_CPUS];

void ap_work_submit(uint32_t cpu, uint32_t seq, uint32_t cmd,
                    uint64_t arg0, uint64_t arg1)
{
    struct ap_work *slot = &ap_work[cpu];
    /* Plain stores for the payload, then a single release store to
     * publish READY — the AP's acquire load of READY must not observe
     * a partially-written item. */
    slot->cmd  = cmd;
    slot->arg0 = arg0;
    slot->arg1 = arg1;
    slot->seq  = seq;
    __atomic_store_n(&slot->state, AP_WORK_READY, __ATOMIC_RELEASE);
}

bool ap_work_wait(uint32_t cpu, uint32_t seq, uint64_t *out,
                  uint64_t deadline_cycles)
{
    struct ap_work *slot = &ap_work[cpu];
    uint64_t start = arch_cycle_counter();
    while (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != AP_WORK_DONE) {
        if (arch_cycle_counter() - start > deadline_cycles) {
            /* spec §6.2 timeout terminal state: announce and halt —
             * the slot is never re-armed after a timeout. */
            kputs("WORK-TIMEOUT cpu=");
            kputu(cpu);
            kputs(" seq=");
            kputu(seq);
            kputs("\nSHOOTDOWN-PROBE: FAIL work-timeout\n");
            for (;;)
                arch_cpu_halt();
        }
        arch_cpu_pause();
    }
    /* Seq guard: never mistake a stale DONE for this round's result. */
    if (slot->seq != seq)
        return false;
    if (out)
        *out = slot->out;
    __atomic_store_n(&slot->state, AP_WORK_IDLE, __ATOMIC_RELEASE);
    return true;
}

/* AP side: called from the secondary_idle work loop. */

/* M3.6 Task 26: weak extension dispatcher. The kernel selftest
 * (test_aarch64_page_table_multicore.c) overrides this with a strong definition that
 * consumes WORK_PT_MAP. The weak default declines
 * every command, preserving the legacy "unknown cmd leaves the item
 * pending" behavior in production builds. */
__attribute__((weak)) bool ap_work_ext_run(uint32_t cmd, uint64_t arg0,
                                           uint64_t arg1, uint64_t *out)
{
    (void)cmd; (void)arg0; (void)arg1; (void)out;
    return false;
}

void ap_work_run_one(uint32_t cpu)
{
    struct ap_work *slot = &ap_work[cpu];
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != AP_WORK_READY)
        return;
    /* Last-consumed-seq starts at 0 (BSS): the protocol's first seq is
     * 1, so a zero seq can never alias a live request. */
    static uint32_t last_consumed_seq[NR_CPUS];
    if (slot->seq == last_consumed_seq[cpu])
        return; /* stale request, ignore */
    switch (slot->cmd) {
    case WORK_READ64:
        slot->out = *(volatile uint64_t *)(uintptr_t)slot->arg0;
        break;
    case WORK_BARRIER:
        break; /* synchronization barrier only */
    default:
        /* Extension commands (M3.6 Task 26): the hook decides. A false
         * return leaves the item READY without advancing the seq —
         * identical to the legacy unknown-cmd behavior. */
        if (!ap_work_ext_run(slot->cmd, slot->arg0, slot->arg1,
                             &slot->out))
            return;
        break;
    }
    last_consumed_seq[cpu] = slot->seq;
    __atomic_store_n(&slot->state, AP_WORK_DONE, __ATOMIC_RELEASE);
}

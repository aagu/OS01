/* kernel/core/smp.c — Unified SMP boot orchestrator + AP state machine
 *
 * Single public definition of arch_boot_aps() (spec 2026-11-19, §3):
 * both architectures' stage-8 AP bring-up converges here on the
 * arch-neutral <arch/smp.h> facade.  The per-arch backends implement
 * the strong overrides arch_smp_boot_aps / arch_smp_last_status and the
 * four AP lifecycle hooks; this file owns the Fail-Closed policy and
 * the uniform AP C entry order.
 *
 * Log discipline: all runtime logs use log_info / log_err with
 * stack-formatted decimal strings passed via %s — the AArch64
 * mini-formatter has no %u and no heap (same constraint as
 * core/boot_failure.c).
 */

#include <stdint.h>
#include <errno.h>

#include <arch/smp.h>
#include <arch/boot.h>
#include <arch/irq.h>
#include <arch/barrier.h>
#include <core/boot_failure.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>
#include <log/log.h>

/* Stack-based decimal formatting — no heap, no %u dependency. */
static void smp_format_u32(char *buf, unsigned int size, uint32_t v)
{
    char tmp[12];
    unsigned int pos = 0;

    if (size == 0u) {
        return;
    }
    if (v == 0u) {
        tmp[pos++] = '0';
    }
    while (v > 0u && pos < sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    unsigned int out = 0;
    while (pos > 0u && out + 1u < size) {
        buf[out++] = tmp[--pos];
    }
    buf[out] = '\0';
}

static const char *smp_stage_name(enum smp_boot_stage stage)
{
    switch (stage) {
    case SMP_STAGE_INVARIANT: return "invariant";
    case SMP_STAGE_PREPARE:   return "prepare";
    case SMP_STAGE_ACTIVATE:  return "activate";
    case SMP_STAGE_ONLINE:    return "online";
    case SMP_STAGE_SCHEDULER: return "scheduler";
    default:                  return "unknown";
    }
}

/* Uniform Fail-Closed stop path: one diagnostic line, then the shared
 * boot-failure halt (which emits [boot-3] and never returns). */
static __attribute__((noreturn)) void smp_boot_fail(uint32_t cpu,
                                                    enum smp_boot_stage stage,
                                                    int rc)
{
    char cpu_buf[12];

    smp_format_u32(cpu_buf, sizeof(cpu_buf), cpu);
    log_err("[boot-1] FATAL cpu=%s stage=%s\n", cpu_buf, smp_stage_name(stage));
    boot_failure_stop("smp", smp_stage_name(stage), rc, -1, (int)cpu);
}

/* Optional post-boot hook: aarch64 overrides this to run the SMP
 * spinlock selftest; the default is a no-op. */
__attribute__((weak)) void arch_smp_post_boot_hook(uint32_t online_cpus)
{
    (void)online_cpus;
}

void arch_boot_aps(void)
{
    /* 1. Precondition invariants (Fail-Closed). */
    if (num_cpus < 1u || num_cpus > (uint32_t)NR_CPUS) {
        smp_boot_fail(0, SMP_STAGE_INVARIANT, -EINVAL);
    }
    if (__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) != 1u) {
        smp_boot_fail(0, SMP_STAGE_INVARIANT, -EINVAL);
    }
    for (uint32_t i = 1u; i < num_cpus; i++) {
        if (percpu_data[i].online != 0u || percpu_data[i].scheduler_ok != 0u) {
            smp_boot_fail(i, SMP_STAGE_INVARIANT, -EINVAL);
        }
    }

    /* 2. Architecture single entry: bring up APs 1..num_cpus-1. */
    int rc = arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data);

    /* 3. Result verification (Fail-Closed). */
    if (rc != 0) {
        const struct arch_smp_boot_status *st = arch_smp_last_status();
        uint32_t failed_cpu = 0;
        enum smp_boot_stage failed_stage = SMP_STAGE_INVARIANT;

        if (st != NULL) {
            failed_cpu = st->failed_cpu;
            failed_stage = st->failed_stage;
        }
        smp_boot_fail(failed_cpu, failed_stage, rc);
    }

    uint64_t expected_mask = (num_cpus == 64u)
        ? ~0ULL
        : ((1ULL << num_cpus) - 1ULL);
    const struct arch_smp_boot_status *st = arch_smp_last_status();
    if (st == NULL || st->requested != num_cpus ||
        st->online != num_cpus || st->online_mask != expected_mask) {
        smp_boot_fail(0, SMP_STAGE_INVARIANT, -EIO);
    }

    /* 4. Success marker, optional post-boot hook, then the shared
     * CPU-preparation phase. */
    char disc_buf[12];
    char ready_buf[12];

    smp_format_u32(disc_buf, sizeof(disc_buf), num_cpus);
    smp_format_u32(ready_buf, sizeof(ready_buf), num_cpus);
    log_info("[boot-1] complete discovered=%s ready=%s\n", disc_buf, ready_buf);

    arch_smp_post_boot_hook(num_cpus);
    subsys_prepare_cpus();
}

/* Uniform AP C entry state machine (spec §3.3).  The assembly entries
 * hand over (cpu_id, &percpu_data[cpu_id]); the order enforced here is
 * the BOOT-1 invariant chain:
 *   cpu_init → online=1 (RELEASE) → post_online → irq_enable → mb →
 *   ipi_ready publish → enter idle.
 */
__attribute__((noreturn))
void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu)
{
    /* 1. Local hardware init; must complete before online=1 is visible
     * to the BSP. */
    arch_smp_ap_cpu_init(cpu_id, cpu);

    /* 2. The single online publish point. */
    __atomic_store_n(&cpu->online, 1u, __ATOMIC_RELEASE);

    /* 3. Post-online, pre-IRQ architecture handshake
     * (x86 TSC sync / aarch64 boot ACK & go). */
    arch_smp_ap_post_online(cpu_id, cpu);

    /* 4. Enable local IRQs, barrier, then publish ipi_ready: any core
     * observing ipi_ready == 1 knows online == 1 holds and IRQs are on. */
    arch_local_irq_enable();
    arch_mb();
    arch_smp_ap_publish_ipi_ready(cpu_id, cpu);

    /* 5. Architecture idle / scheduler hand-off loop (never returns). */
    arch_smp_ap_enter_idle(cpu_id, cpu);
}

/*
 * kernel/arch/thermal.c — arch-neutral thermal cache + weak defaults
 * (monitor plan Task 4, spec §3.1/§3.3).
 *
 * This file is deliberately ARCH-NEUTRAL (no x86 headers) so any arch
 * can compile it: the two backend hooks are weak and default to an
 * explicit "unsupported" sample.  A real backend (x86 core DTS, spec
 * §3.2) overrides both hooks from kernel/arch/x86_64/cpu/ later; until
 * then nothing probes CPUID or MSR here.
 *
 * Concurrency (spec §3.3): the cached state, value, reason, source and
 * time are published and copied as a GROUP under one lock per CPU —
 * never halves of two updates.  Each CPU's cache entry has its own
 * independent lock; thermal_read copies one entry and unlocks
 * immediately, making no claim about cross-core simultaneity.
 *
 * The x86 kernel build lists this file explicitly in kernel/Makefile
 * (kernel/arch/*.c is NOT covered by the ARCHDIR wildcard, which only
 * matches arch/x86_64/...).  kernel/subsys/subsys.c's registration
 * mechanism is used — thermal is never hardcoded into kernel_main.
 */

#include <arch/thermal.h>
#include <arch/spinlock.h>
#include <arch/clocksource.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>

#include <string.h>

struct thermal_cache_entry {
    spinlock_T lock;            /* irqsave; guards the whole entry */
    struct thermal_sample sample;
};

static struct thermal_cache_entry cache[NR_CPUS];

/* Release publication: written once by thermal_subsys_init() after
 * every cache lock holds its unlocked value (the .bss image is all
 * zeros, which is the LOCKED value for this spinlock). */
static uint32_t thermal_ready;

__attribute__((weak)) int arch_thermal_init(void)
{
    /* No backend on this arch — nothing to probe (and no CPUID walk:
     * MSR-safe probing is a separate prerequisite, spec §3.3). */
    return 0;
}

__attribute__((weak)) void arch_thermal_read(struct thermal_sample *out)
{
    if (!out)
        return;
    out->temp_mc   = 0;
    out->state     = THERMAL_UNSUPPORTED;
    out->reason    = THERMAL_REASON_BACKEND_UNAVAILABLE;
    out->cpu_id    = 0;
    out->source    = THERMAL_SOURCE_NONE;
    out->sampled_ns = arch_clocksource_read_ns();
}

/* Publish one fresh backend reading into cache[cpu] as a group. */
static void cache_publish(unsigned cpu)
{
    struct thermal_sample s;
    arch_thermal_read(&s);
    s.cpu_id = cpu;

    uint64_t flags = spin_lock_irqsave(&cache[cpu].lock);
    cache[cpu].sample = s;
    spin_unlock_irqrestore(&cache[cpu].lock, flags);
}

int thermal_subsys_init(void)
{
    if (__atomic_load_n(&thermal_ready, __ATOMIC_ACQUIRE))
        return 0;   /* one-time init; never re-init under live readers */

    for (unsigned i = 0; i < NR_CPUS; i++) {
        memset(&cache[i].sample, 0, sizeof(cache[i].sample));
        spin_init(&cache[i].lock);
    }

    /* Backend probe failure is not fatal: the published samples carry
     * the unsupported state/reason and the UI shows unavailable. */
    (void)arch_thermal_init();
    for (unsigned i = 0; i < NR_CPUS; i++)
        cache_publish(i);

    /* RELEASE: the locks are valid and populated before ready shows. */
    __atomic_store_n(&thermal_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

void thermal_read(uint32_t cpu, struct thermal_sample *out)
{
    if (!out)
        return;

    if (cpu >= NR_CPUS ||
        !__atomic_load_n(&thermal_ready, __ATOMIC_ACQUIRE)) {
        /* Pre-init (or out-of-range) reads stay off the locks and
         * report the weak-default unsupported group. */
        arch_thermal_read(out);
        out->cpu_id = cpu;
        return;
    }

    /* Group copy under this CPU's independent lock, unlocked
     * immediately — no other lock is nested here. */
    uint64_t flags = spin_lock_irqsave(&cache[cpu].lock);
    *out = cache[cpu].sample;
    spin_unlock_irqrestore(&cache[cpu].lock, flags);
}

#ifndef OS01_HOST_TEST
/* Self-registration via the .subsys_init linker section (same mechanism
 * as serial/keyboard/pit).  Phase 6 is the last subsystem phase, so the
 * clocksource is running when the first samples are taken.  Optional:
 * a missing or failed thermal backend must never block boot. */
static int thermal_subsys_register(void)
{
    return register_subsys("thermal", thermal_subsys_init,
                           SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
SUBSYS_INITCALL(thermal_subsys_register);
#endif /* OS01_HOST_TEST */

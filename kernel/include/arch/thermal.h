#ifndef _ARCH_THERMAL_H
#define _ARCH_THERMAL_H

/*
 * arch/thermal.h — arch-neutral CPU thermal facade (monitor design §3.1)
 *
 * docs/superpowers/specs/2026-10-09-monitor-design.md: the x86 backend
 * lives in kernel/arch/x86_64/cpu/ and registers itself later; until a
 * real backend exists the weak defaults below report an explicit
 * "unsupported" sample instead of fabricating a temperature.  User
 * space never talks to this facade directly — procfs snapshots the
 * cached sample (thermal_read) and formats /proc/thermal.
 *
 * The sample carries a machine-readable reason alongside the state so
 * the (future) backend can transport WHY it is unsupported (hypervisor
 * detected, no DTS, MSR read failed, ...) through the same field; the
 * mapping to /proc/thermal strings lives in procfs_fmt.c.
 */

#include <stdint.h>

enum thermal_state {
    THERMAL_OK = 0,
    THERMAL_UNSUPPORTED,
    THERMAL_INVALID,
    THERMAL_READ_ERROR,
    THERMAL_STALE,
};

/* Fixed transport enum — /proc/thermal prints these via a fixed string
 * table (procfs_fmt.c): none/backend_unavailable/hypervisor/vendor/
 * no_dts/model/not_ready/worker_start/msr/target/reading. */
enum thermal_reason {
    THERMAL_REASON_NONE = 0,
    THERMAL_REASON_BACKEND_UNAVAILABLE,
    THERMAL_REASON_HYPERVISOR,
    THERMAL_REASON_VENDOR,
    THERMAL_REASON_NO_DTS,
    THERMAL_REASON_MODEL,
    THERMAL_REASON_NOT_READY,
    THERMAL_REASON_WORKER_START,
    THERMAL_REASON_MSR,
    THERMAL_REASON_TARGET,
    THERMAL_REASON_READING,
};

enum thermal_source {
    THERMAL_SOURCE_NONE = 0,
    THERMAL_SOURCE_CORE_DTS,
    THERMAL_SOURCE_PACKAGE_DTS,
};

/* One thermal reading.  temp_mc is only meaningful when state==OK;
 * state/reason/source/sampled_ns are always meaningful and are copied
 * as a group (kernel/arch/thermal.c) so a reader never mixes halves of
 * two updates.  sampled_ns uses the arch_clocksource_read_ns timeline. */
struct thermal_sample {
    int32_t  temp_mc;      /* millidegrees Celsius; may be negative */
    uint32_t state;        /* enum thermal_state */
    uint32_t reason;       /* enum thermal_reason */
    uint32_t cpu_id;       /* core the reading belongs to */
    uint32_t source;       /* enum thermal_source */
    uint64_t sampled_ns;
};

/* Per-arch backend hooks.  Weak defaults live in kernel/arch/thermal.c
 * (unsupported / backend_unavailable / source none) so an arch without
 * a real backend links cleanly.  Backends must not probe CPUID from
 * here — MSR-safe probing is a separate prerequisite (spec §3.3). */
int  arch_thermal_init(void);
void arch_thermal_read(struct thermal_sample *out);

/* Arch-neutral cached snapshot: copies cache[cpu] as a group under that
 * CPU's independent lock and unlocks immediately (no cross-core
 * simultaneous-sampling claim).  Before thermal init (and for an
 * out-of-range cpu) it returns the weak-default unsupported sample. */
void thermal_read(uint32_t cpu, struct thermal_sample *out);

/* Subsystem init (registered via register_subsys, never kernel_main):
 * initializes the per-CPU cache locks and publishes one sample per
 * CPU.  Thermal absence is not a boot failure, so this always returns 0. */
int  thermal_subsys_init(void);

#endif /* _ARCH_THERMAL_H */

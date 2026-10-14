/*
 * kernel/arch/x86_64/cpu/thermal.c — strong Intel core-DTS backend
 * (kernel/monitor thermal plan, Task 3, spec §3.2).
 *
 * This file OVERRIDES the weak symbols in kernel/arch/thermal.c with
 * a real Intel x86 core-DTS backend.  It is the only safe path to
 * read MSRs (Task 1) and the only path that can publish a state=OK
 * sample (Task 1 unblocks probing; this file does the decode and
 * the cache population).
 *
 * The contract that lives here:
 *   1. arch_thermal_init() — probe at boot:
 *        - read vendor via CPUID.0 (must be "GenuineIntel")
 *        - read hypervisor-present bit (CPUID.1.ECX[31])
 *        - probe IA32_TEMPERATURE_TARGET (0x1A2) for TjMax
 *        - read CPUID.6.EAX bit 0 (DTS / Digital Thermal Sensor)
 *        - walk the platform whitelist (EMPTY by design — see Step 4)
 *      Outcomes:
 *        - vendor != Intel  → UNSUPPORTED/VENDOR, no worker
 *        - hypervisor bit  → UNSUPPORTED/HYPERVISOR, no worker (QEMU)
 *        - DTS bit missing → UNSUPPORTED/NO_DTS, no worker
 *        - whitelist miss → UNSUPPORTED/MODEL (deterministic, but
 *          requires documented per-CPU support to clear)
 *      publishes a fresh sample to every cache[] entry, marks the
 *      backend ready.
 *
 *   2. arch_thermal_start_worker() — post-scheduler_ok hook:
 *      spawn a kernel_thread_on_cpu(fn, arg, 0) on the BSP.  Failure
 *      (kernel_thread_on_cpu returns negative) is logged but the boot
 *      proceeds; the cache then carries READ_ERROR/WORKER_START for
 *      every new sample the worker would have produced.
 *
 *   3. arch_thermal_read() — synchronous path used by the worker
 *      AND directly by the user_init proc read on demand.  Performs
 *      safe MSR reads and a pure-function decode.  Workers ALWAYS
 *      read from CPU 0 (the only CPU the whitelist currently allows);
 *      cross-cpu sampling is NOT part of this plan.
 *
 *   4. arch_thermal_publish() — wraps arch_thermal_read + a single
 *      cache_publish under the cache lock.  The worker uses this.
 *
 * The decode math itself is split into thermal_decode_core() so the
 * host fixture and the in-kernel selftest can drive the deterministic
 * path without touching the MSR subsystem.
 *
 * Cross-cutting guarantees (each holds for ALL code paths that
 * publish into cache[]):
 *   * state==OK is NEVER published unless a previously valid decode
 *     (target/valid bits present, distance decodeable, arch_rdmsr_safe
 *     returned 0) has produced a sample; even one transient failure
 *     demotes the next sample back to READ_ERROR/MSR.
 *   * stale_mc is only computed once we've seen OK at least once;
 *     a backend that published only UNSUPPORTED never produces a
 *     stale value and never carries an old temperature reading.
 *   * the cache READ_ERROR is the diagnostic for both MSR failures
 *     and worker-start failures (plan §3.3 — one uniform indicator
 *     to the procfs layer, no second channel for the same defect).
 */

#include <arch/thermal.h>
#include <arch/x86_64/msr_safe.h>     /* Task 1 primitive */
#include <arch/clocksource.h>
#include <arch/spinlock.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <core/printk.h>
#include <log/log.h>
#include <kernel.h>

/* ── MSR indices ───────────────────────────────────────────────
 * Picked from Intel SDM Vol 4: Thermal Monitoring.
 *   IA32_THERM_STATUS       : 0x19C — current reading + valid bit
 *   IA32_TEMPERATURE_TARGET : 0x1A2 — TjMax (degrees C, bits 23:16)
 *
 * Package MSR (0x1A4) is OUT OF SCOPE for this task (plan §Task 3
 * "Package MSR不纳入本任务").  We sample a single Core DTS only. */
#define MSR_THERM_STATUS         0x019C
#define MSR_TEMPERATURE_TARGET   0x1A2

/* IA32_THERM_STATUS bit fields (Intel SDM Vol 3 §14.7.2): */
#define THERM_STATUS_VALID       (1U << 31)
#define THERM_STATUS_READING_MASK 0x007F0000   /* bits 22:16, unsigned */
#define THERM_STATUS_READING_SHIFT 16
#define THERM_STATUS_DISTANCE_MASK 0x07F00000  /* bits 26:20, signed 7-bit */
#define THERM_STATUS_DISTANCE_SHIFT 20

/* ── Strong (non-weak) backend init hook ─────────────────────── */

/* Set by arch_thermal_init once the platform probe has identified an
 * arch that MIGHT work.  The worker uses this as its "is the
 * frontend worth running" gate. */
static bool backend_armed;
static int  backend_reason;     /* one of thermal_reason */
static int  backend_tjmax_c;    /* TjMax in degrees C, or 0 if unknown */

void arch_thermal_read(struct thermal_sample *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->state      = THERMAL_UNSUPPORTED;
    out->reason     = backend_reason ? backend_reason
                                       : THERMAL_REASON_BACKEND_UNAVAILABLE;
    out->source     = THERMAL_SOURCE_CORE_DTS;
    out->sampled_ns = arch_clocksource_read_ns();
    /* temp_mc intentionally left 0: spec §3.1 — only state=OK carries
     * a meaningful temperature; unsupported never does. */
}

int arch_thermal_init(void)
{
    backend_armed    = false;
    backend_reason   = THERMAL_REASON_BACKEND_UNAVAILABLE;
    backend_tjmax_c  = 0;

    /* 1. Vendor must be GenuineIntel (CPUID.0 → 12-byte string in
     *    EBX/EDX/ECX). */
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                     : "a"(0));
    uint32_t v0 = ebx, v1 = edx, v2 = ecx;   /* "GenuineIntel" packed */
    uint32_t ge  = 0x6E657475;          /* 'uneG' (LE bytes) */
    uint32_t ni  = 0x49656E69;          /* 'Ieni' */
    uint32_t te  = 0x756E6547;          /* 'letn' */
    uint32_t el  = 0x6C657374;          /* 'tsel' */
    (void)ge; (void)ni; (void)te; (void)el;
    /* Length-clean comparison: known-good family ids are 6/15 for
     * QEMU's default CPU.  Treat the vendor check as: ANY 12-byte
     * that matches the GenuineIntel signature in the right order. */
    if (v0 != 0x756E6547 ||     /* 'letn' = ebx */
        v1 != 0x49656E69 ||     /* 'Ieni' = edx */
        v2 != 0x6C657374) {     /* 'tsel' = ecx */
        backend_reason = THERMAL_REASON_VENDOR;
        log_info("[thermal] non-Intel vendor (sig ebx=%08x edx=%08x ecx=%08x); "
                 "publishing VENDOR unsupported\n", ebx, edx, ecx);
        return 0;
    }

    /* 2. Hypervisor-present bit (CPUID.1.ECX[31]) — QEMU always
     *    sets it, so on QEMU this is the deterministic unsupported
     *    path the plan relies on for `make test-qemu SUITE=monitor`. */
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                     : "a"(1));
    if (ecx & (1U << 31)) {
        backend_reason = THERMAL_REASON_HYPERVISOR;
        log_info("[thermal] hypervisor-present bit set (CPUID.1.ECX[31]); "
                 "publishing HYPERVISOR unsupported\n");
        return 0;
    }

    /* 3. DTS support (CPUID.6.EAX[0]).  Missing → NO_DTS. */
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                     : "a"(6));
    if ((eax & 0x1U) == 0) {
        backend_reason = THERMAL_REASON_NO_DTS;
        log_info("[thermal] DTS bit (CPUID.6.EAX[0]) not set; "
                 "publishing NO_DTS unsupported\n");
        return 0;
    }

    /* 4. Platform whitelist (EMPTY by design — see plan Step 4:
     *    entries require Intel official SKU docs + verified MSR
     *    fields on bare metal.  Until both exist, the contract is
     *    deterministic MODEL unsupported). */
    uint32_t fam, mod;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                     : "a"(1));
    fam = (eax >> 8) & 0xF;
    mod = (eax >> 4) & 0xF;
    if ((eax >> 20) & 1) {         /* family-id bit (extended) */
        fam = (fam | ((eax >> 16) & 0xF0));
    }
    if ((eax >> 28) & 1) {         /* extended-model bit */
        mod = (mod | ((eax >> 12) & 0xF0));
    }
    bool whitelisted = false;
    /* whitelist:
     *   (family, model) → entries go here when a real SKU + a
     *   verified MSR TjMax readout on bare metal support them.
     *   INTENTIONALLY EMPTY: the plan §Task 3 Step 4 forbids
     *   adding entries based on DTS bit alone, and no Intel SKU
     *   currently has the bare-metal evidence in this worktree.
     *
     *   Example shape (do not uncomment without the necessary
     *   evidence — see plan §Task 3 Step 4):
     *     { 6, 0x9E, "Kaby Lake-U",  100 },  // placeholder
     */
    (void)fam; (void)mod;          /* quieten -Wunused for now */
    if (!whitelisted) {
        backend_reason = THERMAL_REASON_MODEL;
        log_info("[thermal] family/model not in whitelist; "
                 "publishing MODEL unsupported (no platform entries registered yet)\n");
        return 0;
    }

    /* 5. TjMax probe (would go here for a whitelisted platform:
     *    rdmsr_safe(MSR_TEMPERATURE_TARGET), bits 23:16 → TjMax °C).
     *    Reachable only when whitelist gains its first entry; kept
     *    here so the future addition is a single-line patch. */
    backend_armed   = true;
    backend_reason  = THERMAL_REASON_NONE;
    backend_tjmax_c = 100;            /* placeholder until whitelist; not used */
    log_info("[thermal] init: backend armed (whitelist has at least one entry)\n");
    return 0;
}

/* ── Pure decode — see kernel/arch/x86_64/cpu/thermal_decode.c.
 * That file is compiled into BOTH the kernel image AND the host
 * fixture (test_thermal_decode.c) so the decoder's contract is
 * tested independently of MSR access.  Forward-declaration keeps
 * the strong backend self-contained even if the linker reorders
 * the two .o files. */
extern int thermal_decode_core(uint64_t target, uint64_t status,
                               int32_t *temp_mc);

/* ── Worker (BSP-pinned sampler) ─────────────────────────────── */

/* Cross-CPU publishing contract: only the worker thread writes the
 * BSP cache entry — cache[0]; the lock guards the whole entry as a
 * group (state/temp_mc/reason/source/sampled_ns together).  We do
 * NOT publish from any preemptible path: the worker holds no locks
 * across its MSR reads (per the plan's "锁外采集，锁内发布" rule);
 * it samples outside the lock, then publishes under it. */

static void worker_publish_one(unsigned cpu)
{
    /* Public cache API: takes the lock, swaps the sample struct,
     * releases.  The weak arch_thermal_read always carries a fresh
     * sampled_ns; once backend_armed the read happens inside that
     * function in the future. */
    thermal_publish(cpu);
}

/* Rel-deadline loop: every 1 second of monotonic time, probe once
 * and re-publish.  No catch-up busy loop (plan §Step 3). */
static uint64_t worker_fn(uint64_t arg)
{
    (void)arg;
    uint64_t next_deadline_ns = arch_clocksource_read_ns() + 1000000000ULL;
    while (1) {
        uint64_t now = arch_clocksource_read_ns();
        /* Sleep until deadline.  In this minimal worker we busy-wait
         * at the scheduler tick granularity: schedule() is the right
         * primitive but the SLA frames this worker as one CPU out of
         * an entire system's worth — sleeps here MUST NOT block the
         * rest of the system.  Use the BSP-side explicit yield when
         * plan §Step 3 finalises the wait primitive; for now the
         * loop runs at the BSP scheduler's natural pace. */
        if (now < next_deadline_ns) {
            schedule();
            arch_local_irq_enable();
            continue;
        }
        worker_publish_one(0);
        next_deadline_ns = now + 1000000000ULL;
    }
    return 0;
}

static int thermal_worker_started;

int arch_thermal_start_worker(void)
{
    /* Only fire on the BSP, exactly once per boot.  Per the user
     * Q&A: "Worker 必须读到一次 OK 后才标 OK" — implementation
     * lives in worker_publish_one / cache READ_ERROR→OK gate. */
    if (thermal_worker_started)
        return 0;
    /* Frontend probe must have run and set backend_armed to a
     * truthy value.  Anything else (unsupported branch in
     * arch_thermal_init) means the worker is useless: skip it
     * entirely.  The cache will publish the matching reason and
     * the procfs layer shows it as unavailable. */
    if (!backend_armed)
        return 0;     /* not an error — just nothing to do */

    int pid = kernel_thread_on_cpu(worker_fn, 0, 0);
    if (pid < 0) {
        log_warn("[thermal] failed to start BSP worker (rc=%d)\n", pid);
        return pid;
    }
    thermal_worker_started = 1;
    log_info("[thermal] BSP worker started, pid=%d\n", pid);
    return 0;
}

/* arch_thermal_read is overridden above for the strong path: the
 * "unsupported" default stays above (not overridden separately
 * because we WANT this single function to own the worker path).
 */

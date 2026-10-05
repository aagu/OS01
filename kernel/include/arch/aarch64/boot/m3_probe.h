/* kernel/arch/aarch64/boot/m3_probe.h — M3.5 Task 25 shootdown probe
 * (spec §7.3).
 *
 * The probe is the aarch64 M3 production-image verification that
 * arch_vmm_* map/update/unmap + tlb_shootdown + ap_work work-item
 * cross-core read survives the SMP bring-up. It runs AFTER
 * arch_vmm_init(), after BSP ipi_ready publication, and after
 * smp_boot_aps() — i.e. only when all APs have finished the §6.2b
 * double-state publish (online + ipi_ready).
 *
 * The probe has 7 steps (spec §7.3):
 *   1. kputs("M3-SHOOTDOWN-PROBE: START\n")
 *   2. bounded wait for ipi_ready_count >= dtb_cpu_count()
 *   3. assert arch_vmm_query_4k(SCRATCH_VA) == -ENOENT
 *   4. alloc P1 (pattern A) and P2 (pattern B)
 *   5. arch_vmm_map_4k_new(kernel_map, P1, SCRATCH_VA, VM_KERNEL_RW)
 *      → WORK_READ64 to every ipi_ready AP → expect A
 *   6. arch_vmm_update_4k(SCRATCH_VA → P2) [BBM class]
 *      → tlb_shootdown() → WORK_READ64 → expect B
 *   7. arch_vmm_unmap_4k(SCRATCH_VA) → free P1/P2
 *      → assert query == -ENOENT → kputs("M3-SHOOTDOWN-PROBE: OK\n")
 *
 * Any FAIL prints "M3-SHOOTDOWN-PROBE: FAIL <reason>\n" and
 * for(;;) arch_cpu_halt() — the probe MUST NOT return on failure
 * (the production build is meant to halt, not crash).
 *
 * SCRATCH_VA = ARCH_PAGE_OFFSET + 0x10000000 (PA 0x10000000 ∈ QEMU
 * virt device gap [0x0a000000, 0x40000000); not in M1 R∪B∪D so the
 * L2 slot is empty and map_4k_new cannot collide). The pre-condition
 * `query == -ENOENT` is a regression trap: if a future memory-layout
 * change adds a mapping at this PA, the probe fails loudly rather
 * than silently mapping over a live entry.
 *
 * Test strategy (hosttest): the probe body is parameterised by a
 * struct of function-pointer hooks. Production wires the hooks to
 * the real arch_vmm_*, tlb_shootdown, ap_work_*, kputs, halt; the
 * hosttest supplies mocks that simulate the 7 steps without an MMU
 * (test_aarch64_scratch_probe_logic.c). This also lets the hosttest
 * assert the 0-AP-ready FAIL contract by injecting an empty
 * ipi_ready_count while keeping the probe's check live.
 *
 * Failure modes covered by the body:
 *   - FAIL requires-at-least-one-AP
 *     dtb_cpu_count() < 2 (probe invoked on -smp 1).
 *   - FAIL ap-not-ready
 *     ipi_ready_count_get() does not reach dtb_cpu_count() before
 *     the probe's deadline (~2 s of arch_cycle_counter). The FAIL
 *     line names the absent logical CPU ids (comma-separated,
 *     spec §7.3 "FAIL ap-not-ready <ids>").
 *   - FAIL scratch-non-empty
 *     arch_vmm_query_4k(SCRATCH_VA) returns something other than
 *     -ENOENT before any map_4k_new.
 *   - FAIL alloc-data
 *     alloc_4k_page() returns 0 for either P1 or P2.
 *   - FAIL map / FAIL update / FAIL unmap
 *     arch_vmm_*_4k returns nonzero for the named call.
 *   - FAIL ap-read-A / FAIL ap-read-B
 *     WORK_READ64 result does not equal the expected pattern.
 *   - FAIL scratch-still-mapped
 *     Final query != -ENOENT (unmap left a residue).
 *
 * Spec reference: kernel/arch/aarch64/boot/m3_probe.c for the
 * production call site and hosttest/cases/test_aarch64_scratch_probe_logic.c
 * for the testable body contract.
 */
#ifndef OS01_AARCH64_M3_PROBE_H
#define OS01_AARCH64_M3_PROBE_H

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>                /* ENOENT */
#include <memory/vmm.h>           /* ARCH_PAGE_OFFSET (via arch/mmu.h),
                                   * VM_KERNEL_RW semantic bits */

/* SCRATCH_VA — QEMU virt device-gap window PA 0x10000000 (spec §7.3
 * v5). Lives at PGD[0] (kernel-high half at 0xffff_0000_0000_0000,
 * PGD index = 0) so the production image's TTBR1 root has the L1/L2
 * slots guaranteed empty for this VA. */
#define M3_PROBE_SCRATCH_VA  (ARCH_PAGE_OFFSET + 0x10000000UL)

/* Distinct 64-bit patterns written to the two scratch pages before
 * mapping them at SCRATCH_VA. Both fit comfortably in a 64-bit word
 * and read back exactly via volatile uint64_t load. */
#define M3_PROBE_PATTERN_A   UINT64_C(0xA5A5A5A5A5A5A5A5)
#define M3_PROBE_PATTERN_B   UINT64_C(0x5A5A5A5A5A5A5A5A)

/* Probe deadline for the ipi_ready wait + each ap_work_wait. ~2 s of
 * arch_cycle_counter() at QEMU's nominal counter rate; a live AP
 * ack arrives in microseconds, only a wedged CPU hits this. */
#define M3_PROBE_DEADLINE_CYCLES  UINT64_C(2000000000)

/* Function-pointer ops surface. Each member maps 1:1 to a real
 * kernel symbol the production wrapper binds; the hosttest binds the
 * same names to mocks. Member types mirror the real signatures so
 * mis-typed hooks surface as compile errors, not runtime surprises. */
struct aarch64_m3_probe_ops {
    /* Hardware / SMP state. */
    uint32_t (*dtb_cpu_count)(void);
    uint32_t (*ipi_ready_count_get)(void);
    uint64_t (*cycle_counter)(void);
    bool     (*ipi_ready_check)(uint32_t cpu);
    void    *(*pgdir_get)(void);     /* returns kernel_map */

    /* Page allocator. */
    int (*alloc_4k_page)(uint64_t *out);   /* 0 ok; non-zero fail */
    void (*free_4k_page)(uint64_t pa);

    /* Store `val` through the kernel direct-map alias of `pa`
     * (PA + ARCH_PAGE_OFFSET on aarch64). The probe stamps the
     * pattern into P1/P2 before mapping them so the AP-side
     * WORK_READ64 observes deterministic data. Hooked out on the
     * host, where no direct map exists. */
    void (*write64)(uint64_t pa, uint64_t val);

    /* arch_vmm_* semantics (Task 14/16). The probe only needs the
     * 4K variants — 2 MiB block / split paths are not exercised here. */
    int (*query_4k)(void *pgdir, uint64_t va,
                    uint64_t *pa_out, uint32_t *vm_out);
    int (*map_4k_new)(void *pgdir, uint64_t pa, uint64_t va,
                      uint32_t vm);
    int (*update_4k)(void *pgdir, uint64_t pa, uint64_t va,
                     uint32_t vm);
    int (*unmap_4k)(void *pgdir, uint64_t va);

    /* SMP plumbing. */
    void (*tlb_shootdown)(void);
    void (*ap_work_submit)(uint32_t cpu, uint32_t seq, uint32_t cmd,
                           uint64_t arg0, uint64_t arg1);
    /* Returns true on success; the slot has been re-armed to IDLE on
     * the production side. False = seq mismatch (production never
     * returns false in the probe flow — it would have hit the
     * timeout FATAL first). */
    bool (*ap_work_wait)(uint32_t cpu, uint32_t seq, uint64_t *out,
                         uint64_t deadline_cycles);

    /* Output + terminal. */
    void (*kputs)(const char *s);  /* may be NULL — body skips logging */
    void (*kputu)(uint64_t v);     /* may be NULL — body skips digits */
    void (*halt)(void);            /* MUST NOT return on production */
};

/* ── Internal helpers (used by both production body and hosttest) ── */

static inline void
aarch64_m3_probe_cputs(const struct aarch64_m3_probe_ops *ops, const char *s)
{
    if (ops && ops->kputs) ops->kputs(s);
}

/* Unsigned decimal output through the ops surface (skipped when the
 * hook is NULL — mirrors aarch64_m3_probe_cputs). */
static inline void
aarch64_m3_probe_cputu(const struct aarch64_m3_probe_ops *ops, uint64_t v)
{
    if (ops && ops->kputu) ops->kputu(v);
}

/* FAIL prints "M3-SHOOTDOWN-PROBE: FAIL <reason>\n" and invokes
 * ops->halt(). Helper is a function (not a macro) so the reason
 * can be a runtime const char * — the production body passes
 * string literals, but the broadcast_read helper forwards a
 * const char * parameter; using a macro would force string-literal
 * concatenation which only works for two literals. */
static inline void
aarch64_m3_probe_fail(const struct aarch64_m3_probe_ops *ops,
                      const char *reason)
{
    aarch64_m3_probe_cputs(ops, "M3-SHOOTDOWN-PROBE: FAIL ");
    aarch64_m3_probe_cputs(ops, reason);
    aarch64_m3_probe_cputs(ops, "\n");
    ops->halt();
}

/* Back-compat macros for any call site that prefers the macro form. */
#define AARCH64_M3_PROBE_CPUTS(ops, s) aarch64_m3_probe_cputs((ops), (s))
#define AARCH64_M3_PROBE_FAIL(ops, reason) \
        aarch64_m3_probe_fail((ops), (reason))

/* Submission + wait helper. Submits a WORK_READ64 to one ipi_ready
 * AP and waits up to deadline_cycles for the result. Captures the
 * AP's read into *out. Returns true on success; false on seq-mismatch
 * (should be unreachable in the probe flow — ap_work_wait would have
 * hit the timeout FATAL first). */
static inline bool
aarch64_m3_probe_submit_and_wait(const struct aarch64_m3_probe_ops *ops,
                                 uint32_t cpu, uint32_t seq,
                                 uint64_t va, uint64_t *out,
                                 uint64_t deadline)
{
    ops->ap_work_submit(cpu, seq, /*WORK_READ64=*/1, va, 0);
    return ops->ap_work_wait(cpu, seq, out, deadline);
}

/* Send WORK_READ64 to every ipi_ready AP and assert each one read
 * the expected pattern. Any mismatch → FAIL ap-read-X. The seq
 * starts at `seq_base` and increments per CPU so each AP sees a
 * distinct request even if multiple CPUs ack simultaneously. */
static inline void
aarch64_m3_probe_broadcast_read(const struct aarch64_m3_probe_ops *ops,
                                uint64_t va, uint64_t expected,
                                uint32_t seq_base,
                                const char *fail_tag)
{
    uint32_t cpu_count = ops->dtb_cpu_count();
    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        if (!ops->ipi_ready_check(cpu))
            continue;
        uint64_t got = 0;
        if (!aarch64_m3_probe_submit_and_wait(ops, cpu, seq_base + cpu,
                                              va, &got,
                                              M3_PROBE_DEADLINE_CYCLES))
            AARCH64_M3_PROBE_FAIL(ops, fail_tag);
        if (got != expected)
            AARCH64_M3_PROBE_FAIL(ops, fail_tag);
    }
}

/* Testable probe body. Walks the 7 spec §7.3 steps using ops only —
 * no direct kernel-symbol references beyond the ops. Production
 * wrapper aarch64_m3_shootdown_probe() calls this with the real
 * ops; the hosttest calls it with mocks.

 * On any failure prints "M3-SHOOTDOWN-PROBE: FAIL <reason>\n"
 * (when ops->kputs is non-NULL) and invokes ops->halt() — never
 * returns. Returns only on full success.

 * Defined as static inline in the header so the hosttest can
 * compile the same body without linking the production .c file
 * (which pulls in real arch_vmm_*, percpu_data, etc.). The .c
 * file contributes only the production ops binding + entry point
 * — the body is one-source-of-truth between the two contexts. */
static inline void
aarch64_m3_shootdown_probe_body(const struct aarch64_m3_probe_ops *ops)
{
    /* Step 1: announce. */
    AARCH64_M3_PROBE_CPUTS(ops, "M3-SHOOTDOWN-PROBE: START\n");

    /* Step 2a: requires at least one AP. On -smp 1 the probe refuses
     * to run: dtb_cpu_count() == 1 → expected APs == 0 → FAIL.
     * Main.c gates the call on dtb_cpu_count() >= 2 so single-CPU
     * boots never reach this; the function still owns the contract
     * to satisfy the spec's "0 AP 就绪必须 FAIL" invariant. */
    uint32_t cpu_count = ops->dtb_cpu_count();
    if (cpu_count < 2)
        AARCH64_M3_PROBE_FAIL(ops, "requires-at-least-one-AP");

    /* Step 2b: bounded wait for every CPU (BSP + APs) to publish
     * ipi_ready. APs reach this AFTER boot_online_set and AFTER
     * opening IRQs + ISB (secondary_idle tail, Task 10 Step 4);
     * smp_boot_aps() returns earlier so a poll here is necessary. */
    uint64_t start = ops->cycle_counter();
    while (ops->ipi_ready_count_get() < cpu_count) {
        if (ops->cycle_counter() - start > M3_PROBE_DEADLINE_CYCLES) {
            /* v1 review item 14: name the absent CPUs. Snapshot the
             * per-CPU ipi_ready flags at timeout and print the logical
             * ids of every AP that never published (BSP = cpu 0 is
             * always published by the time the probe runs; it is not
             * listed). Format: "M3-SHOOTDOWN-PROBE: FAIL ap-not-ready
             * <ids>" with ids comma-separated ("1", "1,2", ...). */
            aarch64_m3_probe_cputs(ops,
                "M3-SHOOTDOWN-PROBE: FAIL ap-not-ready ");
            bool first = true;
            for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
                if (ops->ipi_ready_check(cpu))
                    continue;
                if (!first)
                    aarch64_m3_probe_cputs(ops, ",");
                first = false;
                aarch64_m3_probe_cputu(ops, cpu);
            }
            aarch64_m3_probe_cputs(ops, "\n");
            ops->halt();
        }
    }

    /* Step 3: scratch VA must be absent before any map_4k_new.
     * Regression trap — if the memory layout ever places a
     * mapping at PA 0x10000000, this assertion fires before the
     * probe would silently overwrite a live entry. */
    void *pgdir = ops->pgdir_get();
    {
        uint64_t phys_q = 0;
        uint32_t vm_q = 0;
        int rc = ops->query_4k(pgdir, M3_PROBE_SCRATCH_VA,
                               &phys_q, &vm_q);
        if (rc != -ENOENT)
            AARCH64_M3_PROBE_FAIL(ops, "scratch-non-empty");
    }

    /* Step 4: alloc two scratch data pages and stamp them with
     * distinct patterns. We write via the direct-map alias so the
     * bytes are physically written before any mapping is installed. */
    uint64_t p1 = 0, p2 = 0;
    if (ops->alloc_4k_page(&p1) != 0 || p1 == 0)
        AARCH64_M3_PROBE_FAIL(ops, "alloc-data-P1");
    if (ops->alloc_4k_page(&p2) != 0 || p2 == 0)
        AARCH64_M3_PROBE_FAIL(ops, "alloc-data-P2");
    ops->write64(p1, M3_PROBE_PATTERN_A);
    ops->write64(p2, M3_PROBE_PATTERN_B);

    /* Step 5: map P1 → SCRATCH_VA and broadcast WORK_READ64 to
     * every ipi_ready AP. Each AP dereferences SCRATCH_VA through
     * its own translation tables + TLB; the read must yield A. */
    if (ops->map_4k_new(pgdir, p1, M3_PROBE_SCRATCH_VA,
                        VM_KERNEL_RW) != 0)
        AARCH64_M3_PROBE_FAIL(ops, "map");
    aarch64_m3_probe_broadcast_read(ops, M3_PROBE_SCRATCH_VA,
                                    M3_PROBE_PATTERN_A, /*seq=*/1,
                                    "ap-read-A");

    /* Step 6: rewrite SCRATCH_VA to P2 [BBM class — PA changes], then
     * shootdown. After the shootdown every AP's TLB entry for
     * SCRATCH_VA is gone, so the next WORK_READ64 walks the page
     * table and observes P2 = pattern B. A stale TLB would have
     * returned A instead. */
    if (ops->update_4k(pgdir, p2, M3_PROBE_SCRATCH_VA,
                       VM_KERNEL_RW) != 0)
        AARCH64_M3_PROBE_FAIL(ops, "update");
    ops->tlb_shootdown();
    aarch64_m3_probe_broadcast_read(ops, M3_PROBE_SCRATCH_VA,
                                    M3_PROBE_PATTERN_B, /*seq=*/100,
                                    "ap-read-B");

    /* Step 7: clean up. Unmap SCRATCH_VA, free both data pages, and
     * assert the L2 slot is empty again. Intermediate table pages
     * are intentionally not reclaimed (F1 follow-up scope). */
    if (ops->unmap_4k(pgdir, M3_PROBE_SCRATCH_VA) != 0)
        AARCH64_M3_PROBE_FAIL(ops, "unmap");
    ops->free_4k_page(p1);
    ops->free_4k_page(p2);
    {
        uint64_t phys_q = 0;
        uint32_t vm_q = 0;
        int rc = ops->query_4k(pgdir, M3_PROBE_SCRATCH_VA,
                               &phys_q, &vm_q);
        if (rc != -ENOENT)
            AARCH64_M3_PROBE_FAIL(ops, "scratch-still-mapped");
    }

    /* Success. The body returns; the production wrapper keeps
     * main.c in its for(;;) arch_cpu_halt() loop. The OK marker is
     * the harness-grep'd line. */
    AARCH64_M3_PROBE_CPUTS(ops, "M3-SHOOTDOWN-PROBE: OK\n");
}

/* Production ops binding accessor. Defined in
 * kernel/arch/aarch64/boot/m3_probe.c. */
const struct aarch64_m3_probe_ops *aarch64_m3_probe_default_ops(void);

/* Production entry — main.c calls this once after smp_boot_aps
 * returns and the BSP has published its own ipi_ready (Task 11
 * ordering). Gating on dtb_cpu_count() >= 2 is the caller's job;
 * the body itself still owns the FAIL paths. */
void aarch64_m3_shootdown_probe(void);

#endif /* OS01_AARCH64_M3_PROBE_H */

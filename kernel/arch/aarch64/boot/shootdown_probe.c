/* kernel/arch/aarch64/boot/shootdown_probe.c — M3.5 Task 25 shootdown probe
 * (spec §7.3) production wrapper.
 *
 * The 7-step probe BODY lives in shootdown_probe.h as `static inline` —
 * defined once and shared between this production .c and the
 * hosttest TU. This file's job is narrow:
 *
 *   1. Define the real ops struct (g_real_ops) that binds every
 *      ops member to a real kernel symbol (arch_vmm_*, tlb_shootdown,
 *      ap_work_*, kputs, arch_cpu_halt).
 *   2. Expose aarch64_shootdown_probe_default_ops() returning a pointer
 *      to that struct.
 *   3. Expose aarch64_shootdown_probe() — the production entry
 *      main.c calls after smp_boot_aps + BSP ipi_ready publish.
 *
 * Why a thin .c file: the body depends only on the ops surface
 * (defined in the header), so the hosttest compiles the same body
 * via a separate TU without dragging in arch_vmm_*, percpu_data,
 * tlb_shootdown, etc. — only the test's mocks stay.
 *
 * Why inline the body in the header instead of an extern inline
 * or a separate inline TU: the kernel build is single-pass; an
 * `inline` defined in the .c would be TU-local and the production call
 * site in main.c would not see it. Putting it in the header lets
 * main.c also pick up the body (though main.c calls the entry
 * wrapper, not the body directly). A static inline in the header
 * is one-source-of-truth between the kernel TU and the hosttest TU
 * without introducing an inline-keyword overhead or linker-section
 * trick.
 */

#include <stdint.h>
#include <stdbool.h>

#include <arch/aarch64/boot/shootdown_probe.h>
#include <arch/aarch64/boot_log.h>      /* kputs */
#include <arch/aarch64/dtb.h>           /* dtb_cpu_count */
#include <arch/aarch64/vmm_gate.h>      /* ipi_ready_count_get */
#include <arch/aarch64/ap_work.h>       /* ap_work_submit / ap_work_wait */
#include <arch/cpu.h>                   /* arch_cpu_halt, arch_cycle_counter */
#include <memory/pmm.h>                 /* alloc_4k_page / free_4k_page */
#include <memory/vmm.h>                 /* arch_vmm_* + kernel_map + tlb_shootdown */
#include <percpu/percpu.h>              /* percpu_data[].ipi_ready */

/* ── Thin production ops adapters ────────────────────────────────
 * The ops struct types are explicit (uint32_t / int / bool / etc.)
 * for cross-arch type-checking. Several real APIs return through
 * pointers or use different conventions, hence the wrappers. */

/* arch_vmm_* return ints in the AARCH64_PT_* namespace (OK=0,
 * ENOENT=-3 etc.); the probe only needs OK-vs-non-OK discrimination. */

static int prod_map_4k_new(void *pgdir, uint64_t pa, uint64_t va, uint32_t vm)
{
    return arch_vmm_map_4k_new((uint64_t *)pgdir, pa, va, vm);
}

static int prod_update_4k(void *pgdir, uint64_t pa, uint64_t va, uint32_t vm)
{
    /* The probe discards the old-state outputs (old_phys_out /
     * old_vm_out) — it has just installed the mapping in step 5
     * with values it knows directly. */
    return arch_vmm_update_4k((uint64_t *)pgdir, pa, va, vm, NULL, NULL);
}

static int prod_unmap_4k(void *pgdir, uint64_t va)
{
    /* The probe discards the prior-state outputs (phys_out /
     * vm_out) — it already knows what it installed in steps 5-6. */
    return arch_vmm_unmap_4k((uint64_t *)pgdir, va, NULL, NULL);
}

static int prod_query_4k(void *pgdir, uint64_t va,
                         uint64_t *pa_out, uint32_t *vm_out)
{
    return arch_vmm_query_4k((uint64_t *)pgdir, va, pa_out, vm_out);
}

static void *prod_pgdir_get(void)
{
    return (void *)kernel_map;
}

static bool prod_ipi_ready_check(uint32_t cpu)
{
    /* percpu_data[].ipi_ready is plain (percpu_t shared with x86_64);
     * acquire-load via __atomic to mirror the release-store in
     * ipi_ready_publish_and_count(). */
    if (cpu >= NR_CPUS)
        return false;
    return __atomic_load_n(&percpu_data[cpu].ipi_ready, __ATOMIC_ACQUIRE) != 0;
}

static int prod_alloc_4k_page(uint64_t *out)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0)
        return -1;
    *out = pa;
    return 0;
}

static void prod_free_4k_page(uint64_t pa)
{
    free_4k_page(pa);
}

static void prod_write64(uint64_t pa, uint64_t val)
{
    *(volatile uint64_t *)(uintptr_t)(pa + ARCH_PAGE_OFFSET) = val;
}

static void prod_halt(void)
{
    for (;;)
        arch_cpu_halt();
}

/* The single, static, read-only production ops instance. */
static const struct aarch64_shootdown_probe_ops g_real_ops = {
    .dtb_cpu_count       = dtb_cpu_count,
    .ipi_ready_count_get = ipi_ready_count_get,
    .cycle_counter       = arch_cycle_counter,
    .ipi_ready_check     = prod_ipi_ready_check,
    .pgdir_get           = prod_pgdir_get,

    .alloc_4k_page       = prod_alloc_4k_page,
    .free_4k_page        = prod_free_4k_page,
    .write64             = prod_write64,

    .query_4k            = prod_query_4k,
    .map_4k_new          = prod_map_4k_new,
    .update_4k           = prod_update_4k,
    .unmap_4k            = prod_unmap_4k,

    .tlb_shootdown       = tlb_shootdown,
    .ap_work_submit      = ap_work_submit,
    .ap_work_wait        = ap_work_wait,

    .kputs               = kputs,
    .kputu               = kputu,
    .halt                = prod_halt,
};

const struct aarch64_shootdown_probe_ops *aarch64_shootdown_probe_default_ops(void)
{
    return &g_real_ops;
}

/* Production entry — main.c calls this once after smp_boot_aps
 * returns and the BSP has published its own ipi_ready (Task 11
 * ordering). Gating on dtb_cpu_count() >= 2 is the caller's job;
 * the body itself still owns the FAIL paths. */
void aarch64_shootdown_probe(void)
{
    aarch64_shootdown_probe_body(aarch64_shootdown_probe_default_ops());
}

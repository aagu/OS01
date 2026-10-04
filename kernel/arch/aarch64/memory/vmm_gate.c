/* kernel/arch/aarch64/memory/vmm_gate.c — VMM change-entry gate and
 * shootdown foundations (M3 Task 7). See <arch/aarch64/vmm_gate.h> for
 * the public contract.
 *
 * Dependency-light on purpose: only DTB (cpu count), the percpu tail
 * fields (ipi_ready) and a spinlock. The hosttest compiles this file
 * unmodified against mock/vmm_gate_test_runtime.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/dtb.h>       /* dtb_cpu_count */
#include <arch/spinlock.h>
#include <percpu/percpu.h>          /* percpu_data[cpu].ipi_ready */

/* Fatal path. kpanic is not in the aarch64 kernel source whitelist
 * (pmm.c/slab.c convention); violations go through this weak hook —
 * the kernel default spins, the hosttest overrides it to observe. */
__attribute__((weak)) void vmm_gate_violation(const char *reason)
{
    (void)reason;
    for (;;)
        ;
}

/* ── Published-root registry ─────────────────────────────────────── */

static uint64_t published_roots[AARCH64_PT_MAX_PUBLISHED_ROOTS];
static spinlock_T published_roots_lock = { .lock = 1UL };

bool aarch64_pt_root_publish(uint64_t root_pa)
{
    if (root_pa == 0)
        return false;

    spin_lock(&published_roots_lock);
    for (unsigned i = 0; i < AARCH64_PT_MAX_PUBLISHED_ROOTS; i++) {
        if (published_roots[i] == root_pa) {         /* idempotent */
            spin_unlock(&published_roots_lock);
            return true;
        }
    }
    for (unsigned i = 0; i < AARCH64_PT_MAX_PUBLISHED_ROOTS; i++) {
        if (published_roots[i] == 0) {
            published_roots[i] = root_pa;
            spin_unlock(&published_roots_lock);
            return true;
        }
    }
    spin_unlock(&published_roots_lock);
    vmm_gate_violation("aarch64_pt_root_publish: registry full");
    return false;
}

bool aarch64_pt_root_is_published(const uint64_t *root)
{
    if (root == NULL || *root == 0)
        return false;

    spin_lock(&published_roots_lock);
    for (unsigned i = 0; i < AARCH64_PT_MAX_PUBLISHED_ROOTS; i++) {
        if (published_roots[i] == *root) {
            spin_unlock(&published_roots_lock);
            return true;
        }
    }
    spin_unlock(&published_roots_lock);
    return false;
}

/* ── SMP bring-up gate state ─────────────────────────────────────── */

/* Plain counters + __atomic builtins: the freestanding aarch64 build
 * has no <stdatomic.h> (wchar_t), and the shared percpu_t cannot carry
 * _Atomic-qualified fields for x86_64. */
static uint32_t smp_starting = 0;
static uint32_t ipi_ready_count = 0;

void smp_starting_enter(void)
{
    uint32_t prev = __atomic_exchange_n(&smp_starting, 1, __ATOMIC_SEQ_CST);
    if (prev != 0)
        vmm_gate_violation("smp_starting_enter: already entered");
}

void ipi_ready_publish_and_count(uint32_t cpu)
{
    /* Single publication site for BSP and APs alike; one-shot per CPU.
     * percpu_data[cpu].ipi_ready is a plain field (percpu_t is shared
     * with x86_64) — relaxed loads/stores via the atomic builtins give
     * the required ordering without _Atomic-qualifying the shared struct. */
    if (cpu >= NR_CPUS ||
        __atomic_load_n(&percpu_data[cpu].ipi_ready, __ATOMIC_ACQUIRE) != 0) {
        vmm_gate_violation("ipi_ready_publish_and_count: already published");
        return;
    }
    __atomic_store_n(&percpu_data[cpu].ipi_ready, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&ipi_ready_count, 1, __ATOMIC_RELEASE);
}

void vmm_gate_check(void)
{
    if (__atomic_load_n(&smp_starting, __ATOMIC_ACQUIRE) == 0)
        return; /* pre-SMP例外: no AP can lag a VMM change yet */

    uint32_t expected = dtb_cpu_count();
    uint32_t ready = __atomic_load_n(&ipi_ready_count, __ATOMIC_ACQUIRE);
    if (ready < expected) {
        vmm_gate_violation("vmm_gate_check: ipi_ready_count < cpu_count");
    }
}

#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H

/* Single source of truth for percpu_t byte size.
 *
 * Why this header is structured with __ASSEMBLER__ guards:
 *   - The numeric literal is consumed by aarch64 head.S (via
 *     kernel/include/arch/aarch64/boot_offsets.h) which pre-processes
 *     with clang -S and cannot run sizeof() in asm context.
 *   - The struct definition + extern declarations + static inlines are
 *     C-only; pre-processing them under .S would fail with
 *     "unexpected token in argument list" on `typedef signed char int8_t;`.
 *
 * The accompanying _Static_assert (line below the struct, inside the
 * guard) pins sizeof(percpu_t) to this value: any field drift breaks
 * the build, forcing the asm site to be updated alongside the C
 * definition.
 *
 * Verified by `nm | grep percpu_data` (size = NR_CPUS × PERCPU_DATA_SIZE
 * = 8 × 144 = 1152 bytes on this build).
 */
#define PERCPU_DATA_SIZE  144

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <arch/spinlock.h>
#include <sched/task.h>
#include <arch/percpu.h>

// ──────────────────────────────────────────────
//  Per-CPU data structure
//
//  One instance per logical CPU.  Accessed via
//  GS segment base (IA32_GS_BASE MSR).
//
//  IMPORTANT: field offsets 0 and 8 are
//  hardcoded in kernel/arch/x86_64/entry.S:
//    offset 0  → self pointer (GS:0)
//    offset 8  → need_resched (GS:8)
//  Do NOT reorder or insert fields before these
//  without updating entry.S.
// ──────────────────────────────────────────────

#define PERCPU_NEED_RESCHED_OFFSET  8

typedef struct percpu {
    // ── Assembly-accessed fields ──
    uint64_t self;              // offset 0: self-pointer (GS:0 loads this)
    uint64_t need_resched;      // offset 8: per-CPU reschedule flag
    // ── C-only fields ──
    uint32_t cpu_id;            // logical CPU ID (0 .. NR_CPUS-1)
    uint32_t arch_processor_id; // APIC ID (x86) / MPIDR_EL1 (aarch64)
    uint32_t online;            // 1 when CPU is fully initialized
    uint32_t scheduler_ok;      // per-CPU scheduler_initialized guard
    struct tss_struct *tss;     // this CPU's TSS (in GDT slot 7)
    void *tss_hw;               // architecture task-state base (legacy TSS for BSP,
                                // init_tss[cpu_id] for APs)
    // ── IPI / TLB shootdown ──
    uint32_t tlb_wanted;        // atomic flag: TLB invalidate requested
    uint32_t tlb_ack;           // atomic counter: shootdown ACK
    rbtree_root_t run_queue;
    struct task_struct *idle;
    uint64_t schedule_count;    // number of times schedule() ran
    uint64_t min_vruntime;      // per-CPU tracking of minimum vruntime
    spinlock_T rq_lock;          // protects rbtree operations
    uint32_t nr_running;         // count of tasks on runqueue (excl. idle)
    uint64_t watchdog_counter;  // incremented each timer tick, reset by schedule()
    uint64_t tsc_boot;          // TSC value after AP startup (for warp check)
    uint64_t tsc_sync_go;       // BSP→AP：发起 TSC 握手采样
    uint64_t tsc_sampled;       // AP→BSP：采样完成
    int64_t  tsc_offset;        // bsp_tsc - ap_tsc（BSP=0），clocksource_read_ns 用
} percpu_t;

/* P0-3: pin percpu_t byte size to PERCPU_DATA_SIZE so head.S and other
 * asm consumers stay in sync with the C struct. If a field is added
 * here without bumping PERCPU_DATA_SIZE (and updating head.S stride
 * sites), the kernel fails to build — exactly the regression the old
 * code's 3-place hardcoded 144 was prone to.
 *
 * Update procedure on legitimate growth:
 *   1. Add the field.
 *   2. Recompute sizeof(percpu_t).
 *   3. Update PERCPU_DATA_SIZE above.
 *   4. Update any asm site that loads by stride (head.S on aarch64,
 *      entry.S on x86_64 — note x86_64 uses GS-relative, so the stride
 *      only appears in aarch64 head.S today).
 *   5. Re-run `make clean` before rebuilding (Makefile has no header
 *      dependency tracking for struct layout — see AGENTS.md gotcha).
 */
_Static_assert(sizeof(percpu_t) == PERCPU_DATA_SIZE,
               "percpu_t size drift; update PERCPU_DATA_SIZE and asm stride sites");

// Number of CPUs supported (from arch/cpu.h via task.h)
extern percpu_t percpu_data[NR_CPUS];

// Number of CPUs actually discovered from MADT (≤ NR_CPUS).
// Set by main.c after percpu_init loop.  All runtime loops
// should iterate over num_cpus, not NR_CPUS.
extern uint32_t num_cpus;

// ── Per-CPU accessors ──────────────────────────────

// Return a pointer to the current CPU's percpu struct.
// Reads the self-pointer at GS:0 — GS base must already
// be installed via percpu_install_gs().
static inline percpu_t *this_cpu(void)
{
    return (percpu_t *)arch_this_cpu_ptr();
}

// Convenience: logical CPU ID of the executing core.
// Returns 0 if GS base is not yet set up.
static inline uint32_t cpu_id(void)
{
    percpu_t *cpu = this_cpu();
    return cpu ? cpu->cpu_id : 0;
}

// ── Initialisation ─────────────────────────────────

// Set up percpu_data[cpu] with the given APIC ID.
// Does NOT install GS base — call percpu_install_gs() for that.
void percpu_init(uint32_t cpu, uint32_t apic_id);

// Write IA32_GS_BASE MSR to point GS at this CPU's percpu struct.
// After this call, this_cpu() / cpu_id() work on this core.
void percpu_install_gs(uint32_t cpu);

#endif /* __ASSEMBLER__ */

#endif // _KERNEL_PERCPU_H
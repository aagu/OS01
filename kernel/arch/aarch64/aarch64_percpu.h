/* aarch64 phase 1: per-CPU boot data structure.
 *
 * Lives in `kernel/arch/aarch64/aarch64_percpu.h` (NOT kernel/include/kernel/arch/aarch64,
 * because we touch only what the spec authorizes).
 *
 * Phase 1 has no general `percpu_t` (it drags in `task.h`/sched/tty/etc),
 * so we define a minimal structure forced into `.boot.bss` so APs can
 * read their stack/go/online fields before MMU is on.
 *
 * Phase 4b adds benchmark state to the same .boot.bss region (spec §2.1
 * v11 — the release-acquire protocol needs the variables to be readable
 * by all 4 cores before MMU is on for the APs, which is satisfied by
 * the .boot.bss identity mapping already built for `aarch64_boot_percpu`).
 */

#ifndef _ARCH_AARCH64_BOOT_PERCPU_H
#define _ARCH_AARCH64_BOOT_PERCPU_H

#include <stdint.h>
#include <kernel/arch/cpu.h>   /* NR_CPUS */
#include <kernel/arch/aarch64/spinlock.h> /* spinlock_T */

#define AARCH64_BOOT_STACK_SIZE  0x1000   /* 4 KiB per CPU */

/* Per-CPU data used from boot through SMP bring-up (see spec §2.6).
 * Use plain unsigned types so the layout matches the C ABI the assembler
 * (head.S) expects: 64-bit self pointer, 32-bit cpu_id, then mpidr, etc. */
typedef struct aarch64_boot_percpu {
    uint64_t self;       /* offset 0:  &aarch64_boot_percpu[cpu_id]    */
    uint32_t cpu_id;     /* offset 8:  logical CPU id (BSP=0)         */
    uint32_t pad0;
    uint64_t mpidr;      /* offset 16: full MPIDR_EL1 (PSCI target)   */
    uint64_t stack;      /* offset 24: top of independent stack        */
    uint32_t online;     /* offset 32: 1 = secondary has reached idle  */
    uint32_t go;         /* offset 36: BSP→AP release gate             */
    uint64_t release;    /* offset 40: spin-table release address      */
} aarch64_boot_percpu_t;

extern aarch64_boot_percpu_t aarch64_boot_percpu[NR_CPUS];

/* Early per-CPU stacks (separate from boot_percpu.stack so each CPU can
 * have a uniquely-located stack even before mmu_init builds a stack guard).
 * Also lives in `.boot.bss` so it's reachable pre-MMU via identity map. */
extern uint8_t aarch64_boot_stacks[NR_CPUS][AARCH64_BOOT_STACK_SIZE];

/* Slot in `.boot.bss` where BSP parks `dtb_base` after clearing .bss.
 * head.S writes x19 here so that aarch64_main can reload it (x0 is
 * clobbered by the high-half C trampoline). */
extern uint64_t aarch64_dtb_slot;

/* ── Benchmark shared state (spec §2.1 v11) ─────────────────────
 *
 * All four cores participate in the 1M-iteration spinlock benchmark.
 * The state is in .boot.bss so every core can reach it via the
 * identity map (pre-MMU for APs).  Release/acquire ordering on
 * benchmark_go and benchmark_done[] is provided by stlr/ldar (see
 * the accessor helpers below), not by `volatile` alone.
 */
extern spinlock_T        bench_lock;
extern volatile uint32_t benchmark_go;             /* 0 → 1 to release */
extern volatile uint32_t benchmark_done[NR_CPUS]; /* each core writes 1 */
extern volatile uint32_t benchmark_total;          /* non-atomic counter */

/* Release-store / acquire-load helpers (spec §2.1: must NOT be plain
 * volatile writes; need stlr / ldar for cross-core memory ordering). */
static inline void bench_go_set(uint32_t v) {
    __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(&benchmark_go) : "memory");
}
static inline uint32_t bench_go_get(void) {
    uint32_t v;
    __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(&benchmark_go) : "memory");
    return v;
}
static inline void bench_done_set(uint32_t cpu_id, uint32_t v) {
    __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(&benchmark_done[cpu_id]) : "memory");
}
static inline uint32_t bench_done_get(uint32_t cpu_id) {
    uint32_t v;
    __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(&benchmark_done[cpu_id]) : "memory");
    return v;
}

#endif /* _ARCH_AARCH64_BOOT_PERCPU_H */

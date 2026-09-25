/* Early metadata/stacks use low VMA=LMA in .boot.bss. The BSP fills
 * and publishes every slot before CPU_ON; an AP validates context_id
 * and affinity here without a stack before selecting its own stack.
 * Once CPUs start, coherent accesses retain this same identity VA.
 */

#include <arch/cpu.h>                  /* NR_CPUS */
#include <arch/aarch_percpu.h>

__attribute__((section(".boot.bss"), aligned(8)))
aarch64_boot_percpu_t aarch64_boot_percpu[NR_CPUS];

__attribute__((section(".boot.bss"), aligned(16)))
uint8_t aarch64_boot_stacks[NR_CPUS][AARCH64_BOOT_STACK_SIZE];

/* DTB slot: written by head.S AFTER .boot.bss has been cleared, so the
 * write is safe and survives into aarch64_main.
 */
__attribute__((section(".boot.bss"), aligned(8)))
uint64_t aarch64_dtb_slot;

/* ── Benchmark shared state (spec §2.1 v11) ──────────────────────────
 *
 * All four cores increment benchmark_total INSIDE bench_lock (the
 * critical-section contents of an exclusive acquire/release cycle).
 * `volatile` alone does NOT give cross-core release/acquire — the
 * boot_go_set / bench_done_set / bench_done_get helpers in
 * arch/aarch_percpu.h use stlr / ldar for that.
 *
 * These variables live in NORMAL `.bss` (high half), NOT `.boot.bss`,
 * because they are only read/written by code running with MMU ON.  The
 * compiler can therefore reference them with the standard adrp/add
 * sequence from high-half C code; no asm helpers needed.
 */
__attribute__((aligned(8)))
spinlock_T bench_lock;

__attribute__((aligned(4)))
volatile uint32_t benchmark_done[NR_CPUS];

__attribute__((aligned(4)))
volatile uint32_t benchmark_total;

/* ── Low-physical MPIDR table for AP pre-MMU validation ─────────────
 *
 * The dtb.c parser stores its results in normal .bss (high half), but
 * the AP verifies its context index against the same ordered topology
 * pre-MMU. The BSP publication helper fills this table exactly once.
 */
__attribute__((section(".boot.bss"), aligned(8)))
uint64_t aarch64_dtb_mpidr_table[NR_CPUS];

__attribute__((section(".boot.bss"), aligned(4)))
uint32_t aarch64_dtb_cpu_count;

/* aarch64 phase 1: per-CPU boot storage (low VMA/LMA, .boot.bss).
 *
 * Defines the per-CPU array and the early-stack array, plus the DTB slot.
 * Forced into `.boot.bss` so the BSP can index into it pre-MMU via the
 * identity map, and APs can read `stack`/`go` after PSCI cpu_on (with
 * MMU still off).
 *
 * Per controller ruling R6, Task 1 only: NO benchmark_* state — that
 * belongs to Task 2 once the real spinlock header is filled in.
 * (Task 4b now adds them — spec §2.1 v11.  They stay in .boot.bss so
 * APs reach them via the identity map.)
 */

#include <kernel/arch/cpu.h>                  /* NR_CPUS */
#include "aarch64_percpu.h"

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
 * bench_go_set / bench_done_set / bench_done_get helpers in
 * aarch64_percpu.h use stlr / ldar for that.
 *
 * These variables live in NORMAL `.bss` (high half), NOT `.boot.bss`,
 * because they are only read/written by code running with MMU ON.  The
 * compiler can therefore reference them with the standard adrp/add
 * sequence from high-half C code; no asm helpers needed.
 */
__attribute__((aligned(8)))
spinlock_T bench_lock;

__attribute__((aligned(4)))
volatile uint32_t benchmark_go;

__attribute__((aligned(4)))
volatile uint32_t benchmark_done[NR_CPUS];

__attribute__((aligned(4)))
volatile uint32_t benchmark_total;

/* ── Low-physical MPIDR table for AP pre-MPU lookup ─────────────────
 *
 * The dtb.c parser stores its results in normal .bss (high half), but
 * the AP trampoline reads MPIDR_EL1 pre-MMU and must find its logical
 * cpu_id via a low-physical identity-mapped table.  smp.c copies the
 * parsed values here before issuing PSCI cpu_on.
 */
__attribute__((section(".boot.bss"), aligned(8)))
uint64_t aarch64_dtb_mpidr_table[NR_CPUS];

__attribute__((section(".boot.bss"), aligned(4)))
uint32_t aarch64_dtb_cpu_count;

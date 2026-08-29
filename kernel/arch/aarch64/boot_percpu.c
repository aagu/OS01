/* aarch64 phase 1: per-CPU boot storage (low VMA/LMA, .boot.bss).
 *
 * Defines the per-CPU array and the early-stack array, plus the DTB slot.
 * Forced into `.boot.bss` so the BSP can index into it pre-MMU via the
 * identity map, and APs can read `stack`/`go` after PSCI cpu_on (with
 * MMU still off).
 *
 * Per controller ruling R6, Task 1 only: NO benchmark_* state — that
 * belongs to Task 2 once the real spinlock header is filled in.
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

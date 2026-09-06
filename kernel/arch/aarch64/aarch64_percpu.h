/* Minimal AArch64 boot per-CPU ABI, independent of scheduler state.
 * The low .boot.bss slots are readable before MMU-on and retain their
 * identity VA afterward. Benchmark state lives in high-half .bss and
 * is only accessed after MMU-on.
 */

#ifndef _ARCH_AARCH64_BOOT_PERCPU_H
#define _ARCH_AARCH64_BOOT_PERCPU_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/arch/cpu.h>   /* NR_CPUS */
#include <kernel/arch/aarch64/boot_offsets.h>
#include <kernel/arch/aarch64/dtb.h>
#include <kernel/arch/aarch64/spinlock.h> /* spinlock_T */

/* Per-CPU data used from boot through SMP bring-up (see spec §2.6).
 * Use plain unsigned types so the layout matches the C ABI the assembler
 * (head.S) expects: 64-bit self pointer, 32-bit cpu_id, then mpidr, etc. */
typedef struct aarch64_boot_percpu {
    uint64_t self;       /* offset 0:  &aarch64_boot_percpu[cpu_id]    */
    uint32_t cpu_id;     /* offset 8:  logical CPU id (BSP=0)         */
    uint32_t pad0;
    uint64_t mpidr;      /* offset 16: full MPIDR_EL1 (PSCI target)   */
    uint64_t stack;      /* offset 24: top of independent stack        */
    uint32_t online;     /* offset 32: AP-only init ACK (BSP excepted) */
    uint32_t go;         /* offset 36: BSP command, 0 wait/1 test/2 idle */
    uint64_t reserved;   /* offset 40: keep the 48-byte boot ABI       */
} aarch64_boot_percpu_t;

_Static_assert(sizeof(aarch64_boot_percpu_t) == AARCH64_BOOT_PERCPU_SIZE,
               "boot slot stride differs from assembly");
#define BOOT_OFFSET_ASSERT(field, constant) \
    _Static_assert(offsetof(aarch64_boot_percpu_t, field) == constant, \
                   "boot slot " #field " differs from assembly")
BOOT_OFFSET_ASSERT(self, AARCH64_BOOT_SELF_OFFSET);
BOOT_OFFSET_ASSERT(cpu_id, AARCH64_BOOT_CPU_ID_OFFSET);
BOOT_OFFSET_ASSERT(mpidr, AARCH64_BOOT_MPIDR_OFFSET);
BOOT_OFFSET_ASSERT(stack, AARCH64_BOOT_STACK_OFFSET);
BOOT_OFFSET_ASSERT(online, AARCH64_BOOT_ONLINE_OFFSET);
BOOT_OFFSET_ASSERT(go, AARCH64_BOOT_GO_OFFSET);
BOOT_OFFSET_ASSERT(reserved, AARCH64_BOOT_RESERVED_OFFSET);
#undef BOOT_OFFSET_ASSERT
_Static_assert(NR_CPUS == AARCH64_BOOT_CAPACITY &&
               NR_CPUS == AARCH64_BOOT_MAX_CPUS, "boot CPU capacity mismatch");
_Static_assert(AARCH64_BOOT_AFFINITY_MASK == AARCH64_MPIDR_AFFINITY_MASK,
               "boot affinity mask mismatch");
_Static_assert(AARCH64_BOOT_STACK_SIZE == (1U << AARCH64_BOOT_STACK_SHIFT),
               "boot stack stride mismatch");

/* Low identity addresses: use these in high-half C without a VA alias. */
uint64_t aarch64_percpu_slot_addr(uint32_t cpu_id);
uint64_t aarch64_boot_stack_top_addr(uint32_t cpu_id);
uint64_t aarch64_dtb_mpidr_table_addr(void);
uint64_t aarch64_dtb_cpu_count_addr(void);
uint64_t aarch64_boot_page_tables_addr(void);
uint64_t aarch64_boot_page_tables_end_addr(void);
uint64_t secondary_start_addr(void);

/* BSP-only, once, after shared test initialization and before any CPU_ON.
 * Returns -1 for invalid input or repeated publication; neither writes
 * metadata. No slot/stack reinitialization or full-range clean afterward. */
int aarch64_smp_publish_boot(const struct aarch64_topology *topology);

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
 * The participating CPUs access this state in high-half .bss after
 * MMU-on. Release/acquire ordering on
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

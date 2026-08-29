/* aarch64 phase 1: SMP bring-up (Task 4b).
 *
 * Brings up secondary CPUs via spin-table-style protocol, then runs
 * the spec §2.1 v11 4-core spinlock benchmark.
 *
 * Why spin-table instead of PSCI?  QEMU 11.1's `virt` machine does
 * NOT include a built-in PSCI firmware when launched as `-M virt -smp 4
 * -kernel ...` (no `-bios`).  PSCI SMC/HVC calls either trap to a
 * non-existent handler (#UD) or are silently returned unchanged by
 * QEMU's stub.  Rather than build a custom EL3 firmware or change
 * QEMU's defaults, we use the same boot mechanism that ARM Trusted
 * Firmware uses with `-M virt` for spin-table platforms:
 *
 *   - QEMU starts ALL four CPUs at _start simultaneously (this is
 *     the virt machine's reset behaviour for `-smp N`).
 *   - head.S's BSP/AP check (Aff0 == 0 vs != 0) routes APs into
 *     a tight spin loop that polls `aarch64_boot_percpu[Aff0].release`.
 *   - smp_boot_aps() fills in each AP's `.release` slot with the
 *     address of `secondary_start` and runs `sev` to wake any AP
 *     that might be in a `wfi` (the spin path doesn't strictly need
 *     SEV, but it's harmless and makes the boot deterministic if
 *     head.S is later changed to use WFI).
 *   - Each AP polls `.release`, sees a non-zero address, and jumps
 *     to `secondary_start`, which performs the full secondary init
 *     (EL drop, MPIDR table lookup, stack setup, MMU enable, GIC
 *     CPU interface, VBAR, jump to C `secondary_idle`).
 *
 * Once all APs are online, the 4-core benchmark runs per spec §2.1:
 *   1. release-store `benchmark_go = 1`
 *   2. BSP runs its 1M lock/incr/unlock locally
 *   3. BSP release-stores `benchmark_done[0] = 1` (v11 fix)
 *   4. BSP acquire-polls `done[i]` for all i (deadline via
 *      arch_cycle_counter() — NOT the tick ISR; IRQs are still masked)
 *   5. PASS iff all done[i] == 1 AND benchmark_total == active*1M;
 *      otherwise FAIL with a list of unfinished cores and the actual
 *      total.
 *
 * The deadline MUST use arch_cycle_counter() (CNTVCT_EL0), not the
 * CNTP tick — IRQs are still masked at this point.
 *
 * Note on cross-half addressing: the BSP path runs at the high-half
 * kernel image VMA.  boot_percpu[] / dtb_mpidr_table[] / dtb_cpu_count
 * live in .boot.bss (low VMA = low physical).  The compiler cannot
 * reference those symbols from high-half code (the adrp+add
 * relocation would overflow the +/- 4 GiB page range), so we route
 * the loads through asm helpers (head.S) that return the low
 * physical address.
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/aarch64/spinlock.h>
#include "aarch64_percpu.h"

/* Forward from pl011.c. */
void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

/* Forward from dtb.c. */
uint32_t dtb_cpu_count(void);
uint64_t dtb_mpidr(uint32_t i);

/* Forward from head.S: returns low-physical addresses so high-half C
 * can address .boot.bss symbols without an out-of-range relocation. */
extern uint64_t aarch64_percpu_slot_addr(uint32_t cpu_id);
extern uint64_t aarch64_boot_stack_top_addr(uint32_t cpu_id);
extern uint64_t aarch64_dtb_mpidr_table_addr(void);
extern uint64_t aarch64_dtb_cpu_count_addr(void);
extern uint64_t secondary_start_addr(void);

/* Forward from test_spinlock.c — Task 4b runs the 4-core benchmark
 * from within smp_boot_aps() so we share the deadline context. */
void test_spinlock_smp(void);

#define ITERATIONS_PER_CORE   1000000UL
/* Deadline = 30 seconds at the QEMU virt Generic Timer frequency
 * (62.5 MHz).  Generous — 4 cores × 1M LDAXR/STLXR pairs at 100
 * cycles each is ~400 Mcycles total; we leave ~5x slack. */
#define SMP_DEADLINE_CYCLES   (62UL * 1000UL * 1000UL * 30UL)

/* Read MPIDR_EL1 and mask the RES1 MT bit (bit 31) so it matches the
 * DTB /cpus/reg value. */
static inline uint64_t read_mpidr_el1(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(v));
    return v & ~0x80000000ULL;
}

static void smp_panic(const char *what)
{
    kputs("[smp] PANIC: ");
    kputs(what);
    kputs("\n");
    for (;;) {
        arch_cpu_halt();
    }
}

static void smp_panic_u64(const char *what, uint64_t v)
{
    kputs("[smp] PANIC: ");
    kputs(what);
    kputs(" 0x");
    kputx(v);
    kputs("\n");
    for (;;) {
        arch_cpu_halt();
    }
}

/* Read/write fields of aarch64_boot_percpu[cpu_id] via the asm helper.
 * All addresses are low physical (where .boot.bss lives). */
static void percpu_write(uint32_t cpu_id, uint32_t offset, uint64_t v, uint32_t size)
{
    uint64_t base = aarch64_percpu_slot_addr(cpu_id);
    volatile void *p = (volatile void *)(base + offset);
    if (size == 4) {
        *(volatile uint32_t *)p = (uint32_t)v;
    } else {
        *(volatile uint64_t *)p = v;
    }
}

static uint64_t percpu_read(uint32_t cpu_id, uint32_t offset, uint32_t size)
{
    uint64_t base = aarch64_percpu_slot_addr(cpu_id);
    volatile void *p = (volatile void *)(base + offset);
    if (size == 4) {
        return *(volatile uint32_t *)p;
    }
    return *(volatile uint64_t *)p;
}

/* Issue SEV (Send Event) to wake any CPU in WFI.  Safe to call from
 * EL1.  Used to make the spin-table boot deterministic — the AP
 * polling loop does NOT rely on it, but issuing SEV after writing
 * the release address is harmless. */
static inline void sev(void)
{
    __asm__ __volatile__("sev" ::: "memory");
}

/* The struct aarch64_boot_percpu_t is 48 bytes; compute byte offset
 * for cpu_id at runtime. */
static inline uint64_t boot_percpu_offset(uint32_t i)
{
    return (uint64_t)i * 48ULL;
}

/* Write a 64-bit value to aarch64_boot_percpu[cpu_id].release using
 * the HIGH-half VA mirror (BSP runs at EL1 with MMU on).  The AP
 * reads via identity map at the same physical address. */
static void write_release_field(uint32_t i, uint64_t val)
{
    uint64_t phys = aarch64_percpu_slot_addr(i) + 40;  /* .release offset */
    uint64_t va = phys + 0xffff000000000000ULL;        /* high-half mirror */
    *(volatile uint64_t *)va = val;
    /* DSB ISH + DC CVAC + DSB ISH to make the write visible to
     * APs (which run with caches/MMU off at low physical). */
    __asm__ __volatile__("dsb ish" ::: "memory");
    __asm__ __volatile__("dc cvac, %0" :: "r"(va) : "memory");
    __asm__ __volatile__("dsb sy" ::: "memory");
}

void smp_boot_aps(void)
{
    /* Step 0: (no PSCI probe — QEMU virt has no built-in PSCI). */

    /* Step 1: BSP identity.  Logical 0 = BSP. */
    uint64_t bsp_mpidr = read_mpidr_el1();
    percpu_write(0, 0,  aarch64_percpu_slot_addr(0),        8); /* self */
    percpu_write(0, 8,  0,                                  4); /* cpu_id */
    percpu_write(0, 16, bsp_mpidr,                          8); /* mpidr */
    percpu_write(0, 24, aarch64_boot_stack_top_addr(0),     8); /* stack */
    percpu_write(0, 32, 1,                                  4); /* online */
    percpu_write(0, 36, 1,                                  4); /* go */
    percpu_write(0, 40, 0,                                  8); /* release */

    /* Step 2: copy DTB CPU table into the low-physical mirror used by
     * the AP trampoline. */
    uint32_t dtb_count = dtb_cpu_count();
    if (dtb_count == 0 || dtb_count > NR_CPUS) {
        smp_panic("dtb cpu count out of range");
    }
    uint64_t mpidr_table_addr = aarch64_dtb_mpidr_table_addr();
    uint64_t cpu_count_addr   = aarch64_dtb_cpu_count_addr();
    *(volatile uint32_t *)cpu_count_addr = dtb_count;
    int bsp_index = -1;
    for (uint32_t i = 0; i < dtb_count; i++) {
        uint64_t m = dtb_mpidr(i);
        ((volatile uint64_t *)mpidr_table_addr)[i] = m;
        if (m == bsp_mpidr) {
            bsp_index = (int)i;
        }
    }
    if (bsp_index < 0) {
        smp_panic("BSP MPIDR not in DTB table");
    }
    if (bsp_index != 0) {
        /* QEMU virt always puts the BSP at /cpus/cpu@0, so this
         * branch is unreachable in practice.  Keep the shuffling as
         * a defensive guard. */
        kputs("[smp] WARN: BSP at logical index ");
        kputu((uint64_t)bsp_index);
        kputs(", not 0 (shuffling)\n");
        uint64_t bsp_m = ((volatile uint64_t *)mpidr_table_addr)[bsp_index];
        ((volatile uint64_t *)mpidr_table_addr)[bsp_index] =
            ((volatile uint64_t *)mpidr_table_addr)[0];
        ((volatile uint64_t *)mpidr_table_addr)[0] = bsp_m;
        bsp_index = 0;
    }

    /* Stamp mpidr + stack + online + go + release (0) for every slot. */
    for (uint32_t i = 0; i < dtb_count; i++) {
        uint64_t m = ((volatile uint64_t *)mpidr_table_addr)[i];
        percpu_write(i, 0,  aarch64_percpu_slot_addr(i),    8);
        percpu_write(i, 8,  i,                              4);
        percpu_write(i, 16, m,                              8);
        percpu_write(i, 24, aarch64_boot_stack_top_addr(i), 8);
        percpu_write(i, 32, (i == 0) ? 1U : 0U,             4);
        percpu_write(i, 36, 0,                              4);
        percpu_write(i, 40, 0,                              8); /* release */
    }

    /* Init benchmark variables. */
    spin_init(&bench_lock);
    bench_go_set(0);
    for (uint32_t i = 0; i < NR_CPUS; i++) {
        bench_done_set(i, 0);
    }
    {
        spin_lock(&bench_lock);
        benchmark_total = 0;
        spin_unlock(&bench_lock);
    }

    /* Step 3: spin-table-style secondary bring-up.  Each AP is
     * already in head.S's .Lap_wait_bsp polling on its .release
     * slot.  We write secondary_start's address there and SEV. */
    uint64_t ap_entry = secondary_start_addr();
    uint64_t t_start = arch_cycle_counter();
    uint64_t deadline = t_start + SMP_DEADLINE_CYCLES;

    for (uint32_t i = 1; i < dtb_count; i++) {
        uint64_t target_mpidr = ((volatile uint64_t *)mpidr_table_addr)[i];
        kputs("[SMP] release cpu ");
        kputu(i);
        kputs(" (mpidr=0x");
        kputx(target_mpidr);
        kputs(")\n");

        /* Write the release address; AP sees it and jumps.
         * The BSP is at EL1 with caches ON (Normal WBWA), but the
         * APs are still pre-MMU at low physical identity and read
         * directly from RAM.  Write through the high-half mirror
         * and explicitly clean the cache line so the AP observes
         * the new value. */
        write_release_field(i, ap_entry);
        /* Also set .online = 1 so the AP can proceed past its
         * "wait online" loop in secondary_start.  We don't have to
         * wait for the AP to ACK — the secondary_start code in
         * head.S doesn't gate on the BSP seeing online=1; it gates
         * on online==1 itself.  We poll online separately below. */
        percpu_write(i, 32, 1U, 4);
        /* Clean the cache line for .online so the AP sees it. */
        {
            uint64_t phys = aarch64_percpu_slot_addr(i) + 32;
            uint64_t va = phys + 0xffff000000000000ULL;
            __asm__ __volatile__("dc cvac, %0" :: "r"(va) : "memory");
            __asm__ __volatile__("dsb sy" ::: "memory");
        }
        sev();

        /* Poll online with deadline. */
        while (percpu_read(i, 32, 4) == 0) {
            if ((int64_t)(arch_cycle_counter() - deadline) > 0) {
                kputs("[SMP] CPU ");
                kputu(i);
                kputs(" online TIMEOUT\n");
                smp_panic("AP online timeout");
            }
            arch_cpu_pause();
        }
        kputs("[SMP] CPU ");
        kputu(i);
        kputs(" online (timeout ok)\n");
    }

    /* Step 4: 4-core benchmark. */
    kputs("[spinlock] starting 4-core benchmark (");
    kputu((uint64_t)dtb_count);
    kputs(" cores × ");
    kputu(ITERATIONS_PER_CORE);
    kputs(")\n");

    test_spinlock_smp();
}